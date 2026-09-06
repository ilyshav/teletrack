#include "core/Format.h"

#include <stdio.h>

namespace Format {

void uptimeShort(uint32_t ms, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  const unsigned deci = static_cast<unsigned>((ms / 100u) % 10u);
  const unsigned secs = static_cast<unsigned>((ms / 1000u) % 60u);
  const unsigned mins = static_cast<unsigned>((ms / 60000u) % 100u);
  snprintf(out, outSize, "%02u:%02u.%u", mins, secs, deci);
}

void uptimeLong(uint32_t ms, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  const uint32_t total = ms / 1000u;
  unsigned hours = static_cast<unsigned>(total / 3600u);
  unsigned mins = static_cast<unsigned>((total / 60u) % 60u);
  unsigned secs = static_cast<unsigned>(total % 60u);
  if (hours > 99u) {
    hours = 99u;
    mins = 59u;
    secs = 59u;
  }
  snprintf(out, outSize, "%02u:%02u:%02u", hours, mins, secs);
}

void logLine(uint32_t ms, LogLevel level, const char* tag, const char* msg,
             char* out, size_t outSize) {
  if (out == nullptr || outSize < 2) {
    if (out != nullptr && outSize == 1) {
      out[0] = '\0';
    }
    return;
  }
  char stamp[9];
  uptimeShort(ms, stamp, sizeof(stamp));

  const int written = snprintf(out, outSize, "%s [%s] %s: %s", stamp,
                               logLevelName(level), tag != nullptr ? tag : "?",
                               msg != nullptr ? msg : "");
  if (written < 0) {
    out[0] = '\0';
    return;
  }
  if (static_cast<size_t>(written) >= outSize) {
    out[outSize - 2] = '~';
  }
}

}  // namespace Format
