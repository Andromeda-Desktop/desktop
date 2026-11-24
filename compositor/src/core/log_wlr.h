#pragma once

#include <cstdarg>

extern "C" {
#include <wlr/util/log.h>
}

// Bridges wlroots/scenefx logging into core/log.h. Pass to wlr_log_init instead of nullptr: wlroots' built-in
// handler writes straight to stderr with no rate limiting, which lets a per-frame failure stall the compositor on
// console I/O.
void wlrLogHandler(enum wlr_log_importance importance, const char* fmt, va_list args);
