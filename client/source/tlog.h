#pragma once
// Logging hooks: empty in this build (the format strings are still checked).
#include <3ds.h>

static inline void tlog_open(void) {}
static inline __attribute__((format(printf, 1, 2))) void tlog(const char* fmt, ...) { (void)fmt; }
static inline void tlog_close(void) {}
