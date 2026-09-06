#pragma once

#include "config/Settings.h"

// Abstract persistence for Settings. The only reason this interface exists is so
// that everything above it can be tested on the host against MemoryStore.
class SettingsStore {
 public:
  virtual ~SettingsStore() = default;

  // Reads persisted settings into `out`. Returns false when nothing valid could
  // be read; `out` is left untouched in that case, so the caller keeps its
  // defaults.
  virtual bool load(Settings& out) = 0;

  // Persists `s`. Returns false when the write failed.
  virtual bool save(const Settings& s) = 0;

  // False when the backing store is unusable. The device still runs, from RAM
  // defaults, and reports persistDegraded to the client.
  virtual bool available() const = 0;
};
