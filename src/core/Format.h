#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/LogLevel.h"

namespace Format {

// "HH:MM:SS", hours saturating at 99.
void uptime(uint32_t ms, char* out, size_t outSize);

// "HH:MM:SS [LVL] tag: msg", truncated to outSize-1 characters with a
// trailing '~' when it does not fit.
void logLine(uint32_t ms, LogLevel level, const char* tag, const char* msg,
             char* out, size_t outSize);

}  // namespace Format
