#ifndef STRING_PURE_H
#define STRING_PURE_H

#include <stdarg.h>
#include <stddef.h>

#define S_FROM_NUMBER_MAX_CHARS 64
#define S_FROM_NUMBER_FLAG_FORCE_EXP_FORM 1
#define S_FROM_NUMBER_FLAG_FORCE_DECIMAL_FORM 2
// Print the shortest digit string that reads back as the exact same double
// (up to S_FROM_NUMBER_MAX_SIG digits) instead of %g's 6 significant digits.
// Required whenever the text IS the value -- source literals, generated code --
// because 6 digits silently rewrites 110566002.1 as 110566000.
#define S_FROM_NUMBER_FLAG_SHORTEST 4
#define S_FROM_NUMBER_MAX_SIG 17
// Relative slack, about 4 ulp, for the SHORTEST round-trip test: s_to_number
// accumulates a few ulp of its own, so "reads back exactly" is not reachable
// for every value -- "reads back indistinguishably" is.
#define S_ROUND_TRIP_REL_EPS 1e-15

// ---- growable string buffer ----
typedef struct Tsys Tsys;

typedef struct {
    char *buf;
    int   len;
    int   cap;
    int   ok;
    Tsys *sys;
} StrBuf;

void sb_init(StrBuf *sb, Tsys *sys);
int  sb_reserve(StrBuf *sb, int need);
void sb_char(StrBuf *sb, char c);
void sb_str(StrBuf *sb, const char *s);
void sb_int(StrBuf *sb, long long v);
void sb_append(StrBuf *sb, const char *s, int n);

// ---- pure string functions ----

char *s_strcat(char *dest, const char *src);
char *s_strchr(const char *s, int c);
int s_strcmp(const char *a, const char *b);
char *s_strcpy(char *dest, const char *src);
char *s_strncpy(char *dest, const char *src, size_t n);
char *s_strdup(const char *s, void *(*allocator)(size_t));
size_t s_strlen(const char *s);
int s_strncmp(const char *a, const char *b, size_t n);
const char *s_strstr(const char *hay, const char *needle);
double s_to_number(const char *s);
void s_from_number(double d, char *dst);
int s_from_number_flags(double d, char *dst, int flags); 
int s_vsnprintf(char *str, size_t size, const char *format, va_list args);
int s_snprintf(char *str, size_t size, const char *format, ...);

#endif
