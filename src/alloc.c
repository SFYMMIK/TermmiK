/*
 * TermmiK
 * Copyright (C) 2026 SfymmiK
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * TermmiK's allocator: an address-ordered free list over mmap'd chunks.
 *
 * Design notes
 * ------------
 * - Every chunk is a whole number of pages obtained from mmap(). A chunk is
 *   carved into Block headers + payloads; the payload is always 16-byte
 *   aligned (SSE/AVX friendly).
 * - Blocks kept on the list in address order, so a free block can coalesce
 *   with both neighbours in O(1) and an isolated, large-enough chunk can be
 *   returned to the OS with munmap().
 * - A next-fit "rover" starts the first-fit search where the previous search
 *   ended. Sequential allocation patterns (glyph caches, scrollback rows,
 *   row buffers during a resize) then re-locate the previous block in a few
 *   steps instead of walking the whole list from the beginning.
 * - Small allocations are served from shared 64 KiB chunks instead of one
 *   mmap() per allocation. Untouched tail pages are not resident, so this
 *   costs no RSS while cutting syscalls by orders of magnitude.
 * - All entry points take a mutex: the terminal is single-threaded today,
 *   but the allocator is safe to use from worker threads (parser / renderer
 *   offloading) without changes.
 */

#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>

typedef struct Block {
    size_t size;
    struct Block *next;
    struct Block *prev;
    int is_free;
    int zeroed; // payload is known to be all zeroes (fresh from mmap)
} Block;

static Block *head = NULL;
static Block *rover = NULL; // next-fit search hint

static pthread_mutex_t alloc_lock = PTHREAD_MUTEX_INITIALIZER;

#define ALIGN16(x)  (((x) + 15u) & ~(size_t)15)
// Payload bytes a split block must still be able to carry to be worth making.
#define MIN_SPLIT   (sizeof(Block) + 16)
// Smallest mmap() chunk served for small allocations.
#define MIN_CHUNK   ((size_t)64 * 1024)
// Free chunks at least this large (as a whole isolated mmap region) are
// returned to the OS instead of being kept for reuse.
#define RELEASE_MIN ((size_t)64 * 1024)

static void allocator_lock(void)   { pthread_mutex_lock(&alloc_lock); }
static void allocator_unlock(void) { pthread_mutex_unlock(&alloc_lock); }

// Split `block` so that its payload is exactly `size` bytes. Caller holds the
// lock and has already made sure a split is possible.
static void split_block(Block *block, size_t size) {
    Block *split = (Block *)((char *)block + sizeof(Block) + size);
    split->size = block->size - size - sizeof(Block);
    split->is_free = 1;
    split->zeroed = block->zeroed; // both halves inherit the zero state
    split->next = block->next;
    split->prev = block;
    if (block->next) block->next->prev = split;
    block->size = size;
    block->next = split;
    if (rover == block) rover = split;
}

// Merge `block` with its contiguous free successor(s). Caller holds the lock.
static void coalesce_next(Block *block) {
    Block *next = block->next;
    if (next && next->is_free &&
        (char *)block + sizeof(Block) + block->size == (char *)next) {
        if (rover == next) rover = block;
        block->zeroed = block->zeroed && next->zeroed;
        block->size += sizeof(Block) + next->size;
        block->next = next->next;
        if (next->next) next->next->prev = block;
    }
}

// Core allocation. When `was_zeroed` is non-NULL the caller is told whether
// the payload is already all zeroes (a fresh mmap chunk is), which lets
// my_calloc() skip the memset — keeping large zero-initialized allocations
// lazily committed instead of touching every page.
static void *alloc_internal(size_t size, int *was_zeroed) {
    if (size == 0) return NULL;
    // Reject sizes whose aligned block would overflow the header arithmetic.
    if (size > SIZE_MAX - 15 - sizeof(Block) - 4096) return NULL;
    size = ALIGN16(size);

    allocator_lock();

    // 1. First fit, starting from the rover (wraps around once).
    Block *start = rover ? rover : head;
    Block *found = NULL;
    for (Block *curr = start; curr; curr = curr->next) {
        if (curr->is_free && curr->size >= size) { found = curr; break; }
    }
    if (!found) {
        for (Block *curr = head; curr && curr != start; curr = curr->next) {
            if (curr->is_free && curr->size >= size) { found = curr; break; }
        }
    }

    if (found) {
        int zeroed = found->zeroed;
        if (found->size >= size + MIN_SPLIT) split_block(found, size);
        found->is_free = 0;
        found->zeroed = 0; // ownership passes to the caller
        rover = found;
        allocator_unlock();
        if (was_zeroed) *was_zeroed = zeroed;
        return (char *)found + sizeof(Block);
    }

    // 2. Memory miss -> ask the OS for a fresh chunk.
    size_t total = size + sizeof(Block);
    size_t mmap_size = (total + 4095) & ~(size_t)4095;
    if (mmap_size < MIN_CHUNK) mmap_size = MIN_CHUNK;

    Block *block = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) {
        allocator_unlock();
        return NULL;
    }

    Block *new_tail = block;
    if (mmap_size >= total + MIN_SPLIT) {
        block->size = size;
        Block *split = (Block *)((char *)block + total);
        split->size = mmap_size - total - sizeof(Block);
        split->is_free = 1;
        split->zeroed = 1; // fresh mmap memory is zero
        split->next = NULL;
        split->prev = block;
        block->next = split;
        new_tail = split;
    } else {
        block->size = mmap_size - sizeof(Block);
        block->next = NULL;
    }
    block->is_free = 0;
    block->zeroed = 1; // fresh mmap memory is zero

    // Insert the new chunk into the list in memory-address order.
    if (!head) {
        block->prev = NULL;
        head = block;
    } else if ((char *)block < (char *)head) {
        new_tail->next = head;
        head->prev = new_tail;
        block->prev = NULL;
        head = block;
    } else {
        Block *pos = head;
        while (pos->next && (char *)pos->next < (char *)block) {
            pos = pos->next;
        }
        new_tail->next = pos->next;
        if (pos->next) pos->next->prev = new_tail;
        pos->next = block;
        block->prev = pos;
    }

    // Handle the rare case where mmap returned a region contiguous with an
    // existing free block: merge forward so the free list stays compact.
    if (new_tail->is_free) coalesce_next(new_tail);

    rover = block;
    allocator_unlock();
    if (was_zeroed) *was_zeroed = 1;
    return (char *)block + sizeof(Block);
}

// Public malloc: the plain form of alloc_internal().
void *my_malloc(size_t size) {
    return alloc_internal(size, NULL);
}

void my_free(void *ptr) {
    if (!ptr) return;
    Block *block = (Block *)((char *)ptr - sizeof(Block));

    allocator_lock();

    // Cheap double-free guard: catch the common immediate repeat before the
    // block can be reshaped by coalescing.
    if (block->is_free) {
        allocator_unlock();
        return;
    }
    block->is_free = 1;
    block->zeroed = 0; // the caller may have written anything

    // O(1) coalesce with the following block, then with the preceding one.
    coalesce_next(block);

    Block *prev = block->prev;
    if (prev && prev->is_free &&
        (char *)prev + sizeof(Block) + prev->size == (char *)block) {
        if (rover == block) rover = prev;
        prev->zeroed = prev->zeroed && block->zeroed;
        prev->size += sizeof(Block) + block->size;
        prev->next = block->next;
        if (block->next) block->next->prev = prev;
        block = prev;
    }

    // Return a whole isolated mmap region to the OS once it is big enough.
    int isolated_start = (!block->prev ||
        (char *)block->prev + sizeof(Block) + block->prev->size != (char *)block);
    int isolated_end = (!block->next ||
        (char *)block + sizeof(Block) + block->size != (char *)block->next);

    if (isolated_start && isolated_end && block->size + sizeof(Block) >= RELEASE_MIN) {
        if (block->prev) block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
        if (head == block) head = block->next;
        if (rover == block) rover = block->next;
        munmap(block, block->size + sizeof(Block));
    }

    allocator_unlock();
}

void my_print(const char *str) {
    if (!str) return;
    size_t len = 0;
    while (str[len]) len++;
    write(STDERR_FILENO, str, len);
}

void *my_calloc(size_t nmemb, size_t size) {
    if (nmemb == 0 || size == 0) return NULL;

    // Check for multiplication overflow
    if (nmemb > SIZE_MAX / size) return NULL;

    size_t total_size = nmemb * size;
    int zeroed = 0;
    void *ptr = alloc_internal(total_size, &zeroed);
    // Fresh mmap memory is already zero — skipping the memset keeps those
    // pages uncommitted until they are actually used.
    if (ptr && !zeroed) memset(ptr, 0, total_size);
    return ptr;
}

void *my_realloc(void *ptr, size_t size) {
    if (!ptr) return my_malloc(size);
    if (size == 0) { my_free(ptr); return NULL; }
    if (size > SIZE_MAX - 15) return NULL;

    size = ALIGN16(size);
    Block *block = (Block *)((char *)ptr - sizeof(Block));

    if (block->size >= size) {
        // Shrinking: give the tail back so it can be reused (important for
        // growable arrays that double their capacity and then release it).
        allocator_lock();
        if (block->size >= size + MIN_SPLIT + 64) {
            split_block(block, size);
            coalesce_next(block->next);
        }
        allocator_unlock();
        return ptr;
    }

    void *new_ptr = my_malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size); // block->size = old valid payload
        my_free(ptr);
    }
    return new_ptr;
}
