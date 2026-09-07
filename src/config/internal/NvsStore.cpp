#include "config/internal/NvsStore.h"

bool NvsStore::begin() {
  available_ = prefs_.begin(kNamespace, /*readOnly=*/false);
  return available_;
}

void NvsStore::end() {
  if (available_) {
    prefs_.end();
    available_ = false;
  }
}

bool NvsStore::load(Settings& out) {
  if (!available_) {
    return false;
  }
  // Nothing has ever been written: let the caller keep its defaults.
  if (!prefs_.isKey(kKeySampleHz) && !prefs_.isKey(kKeyDeviceName)) {
    return false;
  }

  Settings loaded = Settings::defaults();
  if (prefs_.isKey(kKeyDeviceName)) {
    prefs_.getString(kKeyDeviceName, loaded.deviceName, sizeof(loaded.deviceName));
  }
  loaded.sampleHz = prefs_.getUChar(kKeySampleHz, loaded.sampleHz);

  // Refuse to hand back settings that would not have been accepted over HTTP.
  // A downgrade or a flash bit-flip lands here.
  if (!loaded.validate().ok()) {
    return false;
  }

  out = loaded;
  return true;
}

bool NvsStore::save(const Settings& s) {
  if (!available_) {
    return false;
  }
  if (prefs_.putString(kKeyDeviceName, s.deviceName) == 0) {
    return false;
  }
  if (prefs_.putUChar(kKeySampleHz, s.sampleHz) == 0) {
    return false;
  }
  return true;
}
