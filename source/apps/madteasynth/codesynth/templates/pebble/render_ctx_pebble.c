// Waf globs src/c/**/*.c, so font/render_ctx.c needs a TU in here to be
// compiled. It uses no sizing knobs, so no macro setup.
#include "font/render_ctx.c"
