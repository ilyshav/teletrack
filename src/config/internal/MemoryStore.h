#pragma once

#include "config/SettingsStore.h"

// In-RAM SettingsStore. This is the test double: it is the only implementation
// that host tests link against, and its failure switches let tests drive the
// degraded and write-failure paths deterministically.
class MemoryStore : public SettingsStore {
 public:
  bool load(Settings& out) override;
  bool save(const Settings& s) override;

  void failNextSave();
  bool hasStored() const;
  const Settings& stored() const;

 private:
  Settings stored_ = Settings::defaults();
  bool hasStored_ = false;
  bool failNextSave_ = false;
};
