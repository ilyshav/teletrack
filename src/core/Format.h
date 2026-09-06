#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/LogLevel.h"

namespace Format {

// "MM:SS.d". Minutes wrap modulo 100 so the field stays a fixed 7 characters.
// This is a relative marker for the log console; use uptimeLong() for absolute time.
void uptimeShort(uint32_t ms, char* out, size_t outSize);

// "HH:MM:SS", hours saturating at 99.
void uptimeLong(uint32_t ms, char* out, size_t outSize);

// "MM:SS.d [LVL] tag: msg", truncated to outSize-1 characters with a trailing '~'
// when it does not fit.
void logLine(uint32_t ms, LogLevel level, const char* tag, const char* msg,
             char* out, size_t outSize);

}  // namespace Format
