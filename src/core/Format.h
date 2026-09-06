#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/LogLevel.h"

namespace Format {

// "MM:SS.d". Minutes wrap modulo 100 so the field stays a fixed 7 characters.
// This is a relative marker for the log console; use uptimeLong() for absolute time.
// Needs outSize >= 8.
void uptimeShort(uint32_t ms, char* out, size_t outSize);

// "HH:MM:SS", hours saturating at 99. Needs outSize >= 9.
void uptimeLong(uint32_t ms, char* out, size_t outSize);

// "MM:SS.d [LVL] tag: msg", truncated to outSize-1 characters with a trailing '~'
// when it does not fit. A null tag renders as "?", a null msg as empty.
// Needs outSize >= 2.
void logLine(uint32_t ms, LogLevel level, const char* tag, const char* msg,
             char* out, size_t outSize);

}  // namespace Format
