#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>
#include "tsys.h"

typedef struct Chunk Chunk;

typedef struct Arena {
    Chunk *head;
    Chunk *current;
    char *ptr; // current alloc pointer in current chunk
    char *end; // end of current chunk
    size_t total;      // bytes in all chunks so far
    Tsys *sys; 
} Arena;

void arena_init(Arena *a, Tsys *sys);
void *arena_alloc(Arena *a, size_t size);
char *arena_strdup(Arena *a, const char *str);
void *arena_memdup(Arena *a, const void *src, size_t size);
void arena_free_all(Arena *a);

#endif
