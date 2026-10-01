#include "string_view.h"
#include "string_pure.h"

StringView sv_from_parts(const char *start, size_t length) {
    StringView sv;
    sv.start = start;
    sv.length = length;
    return sv;
}

StringView sv_from_cstring(const char *str) {
    return sv_from_parts(str, str ? s_strlen(str) : 0);
}

int sv_eq(StringView a, StringView b) {
    if (a.length != b.length) return 0;
    if (a.start == b.start) return 1; // Same ptr
    return s_strncmp(a.start, b.start, a.length) == 0;
}

int sv_eq_cstr(StringView a, const char *b) {
    if (!b) return 0;
    size_t b_len = s_strlen(b);
    if (a.length != b_len) return 0;
    return s_strncmp(a.start, b, a.length) == 0;
}

size_t sv_len(StringView sv) {
    return sv.length;
}

int sv_starts_with(StringView sv, StringView prefix) {
    if (prefix.length > sv.length) return 0;
    return s_strncmp(sv.start, prefix.start, prefix.length) == 0;
}
