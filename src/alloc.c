/*
 * TermmiK
 * Copyright (C) 2026 Szymon Grajner (SfymmiK)
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

#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

typedef struct Block {
    size_t size;
    struct Block *next;
    struct Block *prev;
    int is_free;
} Block;

static Block *head = NULL;

void *my_malloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 7) & ~7; // 8-byte alignment

    // 1. Search existing list
    Block *curr = head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            // Split block if there's enough leftover space for a new Block + 8 bytes
            if (curr->size >= size + sizeof(Block) + 8) {
                Block *split = (Block *)((char *)curr + sizeof(Block) + size);
                split->size = curr->size - size - sizeof(Block);
                split->is_free = 1;
                split->next = curr->next;
                split->prev = curr;
                if (curr->next) curr->next->prev = split;
                
                curr->size = size;
                curr->next = split;
            }
            curr->is_free = 0;
            return (char *)curr + sizeof(Block);
        }
        curr = curr->next;
    }

    // 2. Memory miss -> Ask OS for more memory
    size_t total = size + sizeof(Block);
    // Request a minimum of 4KB chunks (or larger if allocation demands it)
    size_t mmap_size = (total + 4095) & ~4095; 

    Block *block = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) return NULL;

    block->is_free = 0;

    Block *new_tail = block;
    if (mmap_size >= total + sizeof(Block) + 8) {
        block->size = size;
        Block *split = (Block *)((char *)block + total);
        split->size = mmap_size - total - sizeof(Block);
        split->is_free = 1;
        split->next = NULL;
        split->prev = block;
        
        block->next = split;
        new_tail = split;
    } else {
        block->size = mmap_size - sizeof(Block);
        block->next = NULL;
    }

    // Insert the new chunk [block ... new_tail] into the doubly-linked list in memory-address order
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
        // Insert after pos
        new_tail->next = pos->next;
        if (pos->next) pos->next->prev = new_tail;
        pos->next = block;
        block->prev = pos;
    }
    
    // Check if new_tail (if free) can coalesce with the block right after it
    // (This handles the rare case where mmap returns a contiguous region to an existing block)
    if (new_tail->is_free && new_tail->next && new_tail->next->is_free &&
        (char *)new_tail + sizeof(Block) + new_tail->size == (char *)new_tail->next) {
        new_tail->size += sizeof(Block) + new_tail->next->size;
        new_tail->next = new_tail->next->next;
        if (new_tail->next) new_tail->next->prev = new_tail;
    }

    return (char *)block + sizeof(Block);
}

void my_free(void *ptr) {
    if (!ptr) return;
    Block *block = (Block *)((char *)ptr - sizeof(Block));
    block->is_free = 1;
    
    // O(1) coalesce with next
    if (block->next && block->next->is_free && 
        (char *)block + sizeof(Block) + block->size == (char *)block->next) {
        block->size += sizeof(Block) + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }
    
    // O(1) coalesce with prev
    if (block->prev && block->prev->is_free && 
        (char *)block->prev + sizeof(Block) + block->prev->size == (char *)block) {
        block->prev->size += sizeof(Block) + block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
        block = block->prev;
    }
    
    // High-water mark release via munmap
    // If block is completely isolated (no contiguous neighbors), it's a full mmap region
    int isolated_start = (!block->prev || (char *)block->prev + sizeof(Block) + block->prev->size != (char *)block);
    int isolated_end = (!block->next || (char *)block + sizeof(Block) + block->size != (char *)block->next);
    
    // Free back to OS if it's an entire mmap region and reasonably large (>= 64KB)
    if (isolated_start && isolated_end && (block->size + sizeof(Block)) >= 65536) {
        if (block->prev) block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
        if (head == block) head = block->next;
        
        munmap(block, block->size + sizeof(Block));
    }
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
    void *ptr = my_malloc(total_size);
    if (ptr) memset(ptr, 0, total_size);
    return ptr;
}

void *my_realloc(void *ptr, size_t size) {
    if (!ptr) return my_malloc(size);
    if (size == 0) { my_free(ptr); return NULL; }
    
    size = (size + 7) & ~7;
    Block *block = (Block *)((char *)ptr - sizeof(Block));
    if (block->size >= size) return ptr; // Already fits

    void *new_ptr = my_malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size); // Safe because block->size is the old valid limit
        my_free(ptr);
    }
    return new_ptr;
}