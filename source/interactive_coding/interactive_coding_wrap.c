// The one piece of interactive_coding.c with nothing to do with the editor, so
// a consumer that only wraps bodies need not link textmode_ui/textarea/font.

#include "interactive_coding.h"
#include "common/string_pure.h"

#include <stdarg.h>

// string_pure only exposes the va_list form.
static int wrap_snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = s_vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

char *interactive_coding_wrap_func_body(Tsys *sys, const char *name, const char *params,
                                         char open, char close, const char *body) {
    if (!sys || !name || !name[0] || !params) return NULL;
    if (!body) body = "";

    size_t name_len   = s_strlen(name);
    size_t params_len = s_strlen(params);
    size_t body_len   = s_strlen(body);
    size_t wrap_len   = name_len + params_len + body_len + 24;
    char *wrapped = (char*)sys->malloc(wrap_len);
    if (!wrapped) return NULL;

    wrap_snprintf(wrapped, wrap_len, "%s = (%s) => %c\n%s\n%c", name, params, open, body, close);
    return wrapped;
}
