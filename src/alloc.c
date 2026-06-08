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

#include "alloc.h"
#include <sys/mman.h>
#include <string.h>

// A very simple mmap allocator with basic splitting to prevent massive waste
typedef struct Block {
    size_t size;
    struct Block *next;
    int is_free;
} Block;

static Block *head = NULL;

void *my_malloc(size_t size) {
    if (size == 0) return NULL;
    size = (size + 7) & ~7; // align 8
    
    Block *curr = head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            curr->is_free = 0;
            return (char *)curr + sizeof(Block);
        }
        curr = curr->next;
    }
    
    size_t total = size + sizeof(Block);
    size_t mmap_size = (total + 4095) & ~4095; // page align
    
    Block *block = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) return NULL;
    
    block->size = size;
    block->is_free = 0;
    block->next = head;
    head = block;
    
    // If we have remaining space in this page, split it
    if (mmap_size > total + sizeof(Block) + 16) {
        Block *split = (Block *)((char *)block + total);
        split->size = mmap_size - total - sizeof(Block);
        split->is_free = 1;
        split->next = head;
        head = split;
    }
    
    return (char *)block + sizeof(Block);
}

void my_free(void *ptr) {
    if (!ptr) return;
    Block *block = (Block *)((char *)ptr - sizeof(Block));
    block->is_free = 1;
}

void *my_calloc(size_t nmemb, size_t size) {
    void *ptr = my_malloc(nmemb * size);
    if (ptr) memset(ptr, 0, nmemb * size);
    return ptr;
}

void *my_realloc(void *ptr, size_t size) {
    if (!ptr) return my_malloc(size);
    if (size == 0) { my_free(ptr); return NULL; }
    Block *block = (Block *)((char *)ptr - sizeof(Block));
    if (block->size >= size) return ptr;
    
    void *new_ptr = my_malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        my_free(ptr);
    }
    return new_ptr;
}

void my_print(const char *str) {
    write(2, str, strlen(str));
}
