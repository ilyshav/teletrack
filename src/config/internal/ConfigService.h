#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config/Settings.h"
#include "config/SettingsStore.h"
#include "config/internal/ConfigApi.h"

// Decision logic for POST /api/config. Pure orchestration: it takes a request
// body and a store, and returns the HTTP status plus the response body. No
// network types appear here, which is what makes the rollback and degraded
// paths testable on the host.
namespace ConfigService {

struct SaveOutcome {
  uint16_t httpStatus = 500;
  size_t bodyLength = 0;
};

// On success `settings` holds the new values. On any failure `settings` is
// exactly what it was on entry.
SaveOutcome save(const char* requestBody, size_t len, Settings& settings,
                 SettingsStore& store, char* out, size_t outSize);

}  // namespace ConfigService
