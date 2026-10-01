#include "arena.h"

// Chunks are 4KB until an arena holds 64KB, then as big as the arena already
// is, up to 256KB: a large arena costs a few dozen mallocs rather than one per
// 4KB, while the many small ones (a statement, a function) waste no more than
// they did with fixed chunks.
#define ARENA_CHUNK_SIZE     4096
#define ARENA_GROW_AFTER     (64 * 1024)
#ifndef ARENA_CHUNK_SIZE_MAX
#define ARENA_CHUNK_SIZE_MAX (256 * 1024)
#endif

struct Chunk {
    Chunk *next;
    char data[]; // Flexible array member
};

void arena_init(Arena *a, Tsys *sys) {
    a->head = NULL;
    a->current = NULL;
    a->ptr = NULL;
    a->end = NULL;
    a->total = 0;
    a->sys = sys;
}

void *arena_alloc(Arena *a, size_t size) {
    // Align size to 8 bytes
    size_t aligned_size = (size + 7) & ~7;
    
    if (a->ptr && (a->ptr + aligned_size <= a->end)) {
        void *res = a->ptr;
        a->ptr += aligned_size;
        return res;
    }
    
    size_t chunk_size = ARENA_CHUNK_SIZE;
    if (a->total >= ARENA_GROW_AFTER)
        chunk_size = a->total < ARENA_CHUNK_SIZE_MAX ? a->total : ARENA_CHUNK_SIZE_MAX;
    size_t min_size = aligned_size + sizeof(Chunk);
    if (min_size > chunk_size) chunk_size = min_size + 1024;
    
    Chunk *new_chunk = (Chunk *)S_MALLOC(a->sys, chunk_size);
    // A grown chunk may not fit a small heap; the request itself still might.
    if (!new_chunk && chunk_size > min_size) {
        chunk_size = min_size;
        new_chunk = (Chunk *)S_MALLOC(a->sys, chunk_size);
    }
    // Out of memory: hand back NULL and leave the arena as it was. Writing the
    // chunk header through a null pointer would look like an allocator that
    // kept working, and the corruption would surface somewhere else entirely.
    if (!new_chunk) return NULL;
    new_chunk->next = NULL;
    
    if (a->current) {
        a->current->next = new_chunk;
    } else {
        a->head = new_chunk;
    }
    a->current = new_chunk;
    
    // Setup pointers
    // data starts after struct Chunk
    char *start = new_chunk->data;
    a->ptr = start + aligned_size;
    a->end = (char*)new_chunk + chunk_size;
    a->total += chunk_size;
    
    return start;
}

char *arena_strdup(Arena *a, const char *str) {
    if (!str) return NULL;
    size_t len = 0;
    while(str[len]) len++;
    
    char *res = (char *)arena_alloc(a, len + 1);
    for(size_t i=0; i<len; i++) res[i] = str[i];
    res[len] = '\0';
    return res;
}

void *arena_memdup(Arena *a, const void *src, size_t size) {
    if (!src || size == 0) return NULL;
    void *res = arena_alloc(a, size);
    // Use sys->memcpy or manual loop? Tsys has memcpy.
    if (a->sys->memcpy) {
        a->sys->memcpy(res, src, size);
    } else {
        // Fallback
        char *d = (char*)res;
        const char *s = (const char*)src;
        for(size_t i=0; i<size; i++) d[i] = s[i];
    }
    return res;
}

void arena_free_all(Arena *a) {
    Chunk *c = a->head;
    while (c) {
        Chunk *next = c->next;
        S_FREE(a->sys, c);
        c = next;
    }
    a->head = NULL;
    a->current = NULL;
    a->ptr = NULL;
    a->end = NULL;
    a->total = 0;
}
