#include "array.h"

// Undo any tracking macros so the real definitions below are not remapped.
#ifdef TSYS_TRACK_ALLOCS
#undef array_init
#undef array_free
#undef array_push
#undef array_push_n
#endif

void array_init(Array *a, size_t element_size) {
    a->data = NULL;
    a->size = 0;
    a->capacity = 0;
    a->element_size = element_size;
}

void array_set(Array *a, size_t index, const void *value) {
    if (index >= a->size) return;
    char *dest = (char *)a->data + (index * a->element_size);
    const char *src = (const char *)value;
    for (size_t i = 0; i < a->element_size; ++i) {
        dest[i] = src[i];
    }
}

void array_push(Array *a, const void *value, Tsys *sys) {
    if (a->size == a->capacity) {
        size_t new_cap = a->capacity == 0 ? 8 : a->capacity * 2;
        void *new_data = S_REALLOC(sys, a->data, new_cap * a->element_size);
        if (!new_data) {
            if (sys->error) sys->error("Out of memory in array_push");
            return;
        }
        a->data = new_data;
        a->capacity = new_cap;
    }
    char *dest = (char *)a->data + (a->size * a->element_size);
    // The common element sizes (ids, pointers) skip the indirect memcpy call;
    // a constant-count byte copy compiles to one move and is alias-safe.
    const char *src4 = (const char *)value;
    if (a->element_size == 4) {
        for (int i = 0; i < 4; i++) dest[i] = src4[i];
    } else if (a->element_size == 8) {
        for (int i = 0; i < 8; i++) dest[i] = src4[i];
    } else if (sys->memcpy) {
        sys->memcpy(dest, value, a->element_size);
    } else {
        const char *src = (const char *)value;
        for (size_t i = 0; i < a->element_size; ++i) {
            dest[i] = src[i];
        }
    }
    a->size++;
}

int array_push_n(Array *a, const void *values, size_t count, Tsys *sys) {
    if (count == 0) return 1;
    size_t need = a->size + count;
    if (need > a->capacity) {
        // Same doubling array_push uses, run far enough to cover the whole
        // chunk -- so one bulk append is one realloc, not one per doubling.
        size_t new_cap = a->capacity == 0 ? 8 : a->capacity;
        while (new_cap < need) new_cap *= 2;
        void *new_data = S_REALLOC(sys, a->data, new_cap * a->element_size);
        if (!new_data) return 0;    // array untouched; caller decides
        a->data = new_data;
        a->capacity = new_cap;
    }
    char *dest = (char *)a->data + (a->size * a->element_size);
    size_t bytes = count * a->element_size;
    if (sys->memcpy) {
        sys->memcpy(dest, values, bytes);
    } else {
        const char *src = (const char *)values;
        for (size_t i = 0; i < bytes; ++i) dest[i] = src[i];
    }
    a->size = need;
    return 1;
}

void array_clear(Array *a) {
    a->size = 0;
}

void array_free(Array *a, Tsys *sys) {
    if (a->data) {
        S_FREE(sys, a->data);
        a->data = NULL;
    }
    a->size = 0;
    a->capacity = 0;
}

size_t array_len(Array *a) {
    return a->size;
}

void *array_back(Array *a) {
    if (a->size == 0) return NULL;
    return array_get(a, a->size - 1);
}
