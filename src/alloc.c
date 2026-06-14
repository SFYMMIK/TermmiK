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
    int is_free;
} Block;

static Block *head = NULL;

// Helper to merge adjacent free blocks to prevent catastrophic fragmentation
static void coalesce() {
    Block *curr = head;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            // Ensure they are actually contiguous in physical memory
            if ((char *)curr + sizeof(Block) + curr->size == (char *)curr->next) {
                curr->size += sizeof(Block) + curr->next->size;
                curr->next = curr->next->next;
                continue; // Check again with the new next block
            }
        }
        curr = curr->next;
    }
}

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

    if (mmap_size >= total + sizeof(Block) + 8) {
        block->size = size;
        Block *split = (Block *)((char *)block + total);
        split->size = mmap_size - total - sizeof(Block);
        split->is_free = 1;
        split->next = head;
        
        block->next = split;
        head = block;
    } else {
        block->size = mmap_size - sizeof(Block);
        block->next = head;
        head = block;
    }

    return (char *)block + sizeof(Block);
}

void my_free(void *ptr) {
    if (!ptr) return;
    Block *block = (Block *)((char *)ptr - sizeof(Block));
    block->is_free = 1;
    coalesce(); // Keep the list clean
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