// Minimal CRT replacement for tiny Windows executables.
// Link with -nostdlib /NODEFAULTLIB to drop the full CRT (~40KB saved).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The compiler emits a reference to _fltused when float math is present.
// We define it here so we don't need the CRT.
#ifdef __clang__
__attribute__((used))
#endif
int _fltused = 0x9875;

// Forward declare main() so the entry point can call it.
int main(void);

// Real entry point.  /entry:mainCRTStartup must be passed to the linker.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((force_align_arg_pointer))
#endif
void mainCRTStartup(void)
{
    int r = main();
    ExitProcess((UINT)r);
}

// The compiler may emit calls to memset for struct/array zero-init.
// Provide a simple implementation.
void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
