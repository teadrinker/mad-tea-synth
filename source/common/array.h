#ifndef ARRAY_H
#define ARRAY_H

#include <stddef.h>
#include "tsys.h"

#ifndef NULL
    #define NULL ((void*)0)
#endif

typedef struct {
    void *data;
    size_t size;
    size_t capacity;
    size_t element_size;
} Array;

void array_init(Array *a, size_t element_size);
static inline void *array_get(Array *a, size_t index) {
    if (index >= a->size) return NULL;
    return (char *)a->data + (index * a->element_size);
}
void array_set(Array *a, size_t index, const void *value);
void array_push(Array *a, const void *value, Tsys *sys);
void array_free(Array *a, Tsys *sys);
size_t array_len(Array *a);
void *array_back(Array *a);

// Append `count` elements from `values` in one go. Capacity grows to the next
// power of two that fits, so one call costs one realloc however large the
// chunk -- unlike count separate array_push calls, which each pay a call and a
// per-element copy.
//
// Returns 1 on success, 0 if the allocation failed (the array is left exactly
// as it was). A caller that can degrade -- dropping output rather than
// aborting -- checks the return; array_push, which reports through sys->error
// and silently does not push, gives it nothing to check.
int array_push_n(Array *a, const void *values, size_t count, Tsys *sys);

// Drop every element but KEEP the allocation, so an array that is refilled
// over and over (a per-frame buffer) stops reallocating after its first few
// passes. array_free is still what releases the memory.
void array_clear(Array *a);

// ---- tracking overrides ----
// Define TSYS_TRACK_ALLOCS to capture __FILE__/__LINE__ per array op.
// array_init also warns if called on an array that still holds data (leak).
#ifdef TSYS_TRACK_ALLOCS
  void _array_init_track(Array *a, size_t es, const char *file, int line);
  void _array_free_track(Array *a, Tsys *sys, const char *file, int line);
  void _array_push_track(Array *a, const void *v, Tsys *sys, const char *file, int line);
  int  _array_push_n_track(Array *a, const void *v, size_t n, Tsys *sys,
                           const char *file, int line);
  #undef array_init
  #undef array_free
  #undef array_push
  #undef array_push_n
  #define array_init(a, es)       _array_init_track((a), (es), __FILE__, __LINE__)
  #define array_free(a, sys)      _array_free_track((a), (sys), __FILE__, __LINE__)
  #define array_push(a, v, sys)   _array_push_track((a), (v), (sys), __FILE__, __LINE__)
  #define array_push_n(a, v, n, sys) \
      _array_push_n_track((a), (v), (n), (sys), __FILE__, __LINE__)
  // array_clear allocates nothing, so it needs no tracking variant.
#endif

#endif
