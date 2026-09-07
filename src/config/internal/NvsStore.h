#pragma once

#include <Preferences.h>

#include "config/SettingsStore.h"

// SettingsStore backed by ESP32 NVS via Preferences.
//
// NVS keys are limited to 15 characters and are not the wire field names.
class NvsStore : public SettingsStore {
 public:
  static constexpr const char* kNamespace = "teletrack";
  static constexpr const char* kKeyDeviceName = "name";
  static constexpr const char* kKeySampleHz = "hz";

  // Opens the namespace. Returns false when NVS is unusable — the caller then
  // runs from RAM defaults and says so in the log.
  bool begin();
  void end();

  bool load(Settings& out) override;
  bool save(const Settings& s) override;

 private:
  Preferences prefs_;
  bool available_ = false;
};
