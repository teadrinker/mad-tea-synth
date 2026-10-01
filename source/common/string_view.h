#ifndef STRING_VIEW_H
#define STRING_VIEW_H

#include <stddef.h>

typedef struct {
    const char *start;
    size_t length;
} StringView;

#define SV_FMT "%.*s"
#define SV_ARG(sv) (int)(sv).length, (sv).start

// Constructors
StringView sv_from_parts(const char *start, size_t length);
StringView sv_from_cstring(const char *str);

// Comparisons
int sv_eq(StringView a, StringView b);
int sv_eq_cstr(StringView a, const char *b);

// Utils
size_t sv_len(StringView sv);
int sv_starts_with(StringView sv, StringView prefix);

#endif
