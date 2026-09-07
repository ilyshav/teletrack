#include "core/Format.h"

#include <stdio.h>

namespace Format {

void uptime(uint32_t ms, char* out, size_t outSize) {
  const uint32_t total = ms / 1000u;
  unsigned hours = static_cast<unsigned>(total / 3600u);
  unsigned mins = static_cast<unsigned>((total / 60u) % 60u);
  unsigned secs = static_cast<unsigned>(total % 60u);
  // millis() wraps at ~49 days, so hours can reach 1193 and overrun a
  // fixed-width field. Saturating keeps the column aligned.
  if (hours > 99u) {
    hours = 99u;
    mins = 59u;
    secs = 59u;
  }
  snprintf(out, outSize, "%02u:%02u:%02u", hours, mins, secs);
}

void logLine(uint32_t ms, LogLevel level, const char* tag, const char* msg,
             char* out, size_t outSize) {
  char stamp[9];
  uptime(ms, stamp, sizeof(stamp));

  const int written = snprintf(out, outSize, "%s [%s] %s: %s", stamp,
                               logLevelName(level), tag, msg);
  if (static_cast<size_t>(written) >= outSize) {
    out[outSize - 2] = '~';
  }
}

}  // namespace Format
