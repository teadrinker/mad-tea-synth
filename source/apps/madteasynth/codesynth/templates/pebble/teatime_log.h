#pragma once
// TEATIME_NO_LOG compiles APP_LOG out: include this instead of relying on
// pebble.h's. It removes the call and its two .rodata strings (virtual-size
// cap), not firmware logging or the developer connection. Arguments stay inside
// sizeof: type-checked, never evaluated.
#include <pebble.h>

#ifdef TEATIME_NO_LOG
#undef APP_LOG
#define APP_LOG(level, fmt, args...) \
  ((void)sizeof(app_log((level), "", 0, (fmt), ## args)))
#endif
