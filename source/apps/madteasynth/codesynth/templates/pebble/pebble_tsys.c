// pebble_tsys.c -- Pebble Tsys backed by the SDK heap.
#include <pebble.h>
#include "teatime_log.h"   // TEATIME_NO_LOG: compiles APP_LOG out
#include <stdlib.h>
#include <string.h>
#include "pebble_tsys.h"

static void *pool_malloc(size_t n)  { return malloc(n); }
static void *pool_realloc(void *p, size_t n) { return realloc(p, n); }
static void  pool_free(void *p)     { free(p); }
static void *pool_memset(void *s, int c, size_t n) { return memset(s, c, n); }
static void *pool_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void  pool_print(const char *s) { APP_LOG(APP_LOG_LEVEL_INFO, "%s", s); }
static void  pool_error(const char *s) { APP_LOG(APP_LOG_LEVEL_ERROR, "%s", s); }

Tsys pebble_tsys_init(void) {
    Tsys sys;
    sys.malloc  = pool_malloc;
    sys.realloc = pool_realloc;
    sys.free    = pool_free;
    sys.memset  = pool_memset;
    sys.memcpy  = pool_memcpy;
    sys.print   = pool_print;
    sys.error   = pool_error;
    return sys;
}
