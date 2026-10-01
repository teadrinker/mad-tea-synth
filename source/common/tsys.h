#ifndef TSYS_H
#define TSYS_H

// #define TSYS_FORCE_PURE_IMPL  // this will replace any use of tsys_crt with tsys_pure

#include <stddef.h>

typedef struct Tsys Tsys;

struct Tsys {
    void *(*malloc )(size_t size);
    void *(*realloc)(void *ptr, size_t size);
    void  (*free   )(void *ptr);
    void *(*memset )(void *s, int c, size_t n);
    void *(*memcpy )(void *dest, const void *src, size_t n);
    void  (*print  )(const char *s);
    void  (*error  )(const char *s);
};

// ---- alloc macros (replace raw sys->malloc/free/realloc everywhere) ----
// Define TSYS_TRACK_ALLOCS to enable file/line tracking for leak debugging.
#ifdef TSYS_TRACK_ALLOCS
  void *_tsys_tracked_malloc(Tsys *sys, size_t size, const char *file, int line);
  void  _tsys_tracked_free(Tsys *sys, void *ptr);
  void *_tsys_tracked_realloc(Tsys *sys, void *ptr, size_t size, const char *file, int line);
  #define S_MALLOC(sys, size)          _tsys_tracked_malloc((sys), (size), __FILE__, __LINE__)
  #define S_FREE(sys, ptr)             _tsys_tracked_free((sys), (ptr))
  #define S_REALLOC(sys, ptr, size)    _tsys_tracked_realloc((sys), (ptr), (size), __FILE__, __LINE__)
#else
  #define S_MALLOC(sys, size)          ((sys)->malloc(size))
  #define S_FREE(sys, ptr)             ((sys)->free(ptr))
  #define S_REALLOC(sys, ptr, size)    ((sys)->realloc(ptr, size))
#endif

#endif
