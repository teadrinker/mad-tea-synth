// The few libc symbols a freestanding build needs (microw8 cart, win32
// `build.bat tiny`); the compiler lowers struct copies and array init into them.
// TINY_RUNTIME_NO_MEMSET: crt_stub_win32.c already supplies memset.

// __SIZE_TYPE__ matches the compiler's builtin declarations on both x64 and wasm32.
typedef __SIZE_TYPE__ tiny_size_t;

#ifndef TINY_RUNTIME_NO_MEMSET
void *memset(void *dst, int c, tiny_size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
#endif

void *memcpy(void *dst, const void *src, tiny_size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, tiny_size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) { while (n--) *d++ = *s++; }
    else       { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
