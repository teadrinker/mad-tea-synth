// Waf globs src/c/**/*.c, so font/render_ctx.c needs a TU in here to be
// compiled. One encoding on the watch, so no runtime layouts.
#define RENDER_CTX_LAYOUTS 0
#include "font/render_ctx.c"
