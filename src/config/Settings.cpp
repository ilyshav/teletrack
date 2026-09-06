#include "config/Settings.h"

#include <stdio.h>
#include <string.h>

void ValidationResult::add(const char* field, const char* message) {
  if (count >= kMaxErrors) {
    return;
  }
  errors[count].field = field;
  errors[count].message = message;
  ++count;
}

const char* ValidationResult::messageFor(const char* field) const {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(errors[i].field, field) == 0) {
      return errors[i].message;
    }
  }
  return nullptr;
}

Settings Settings::defaults() {
  Settings s;
  s.schemaVersion = kSchemaVersion;
  memset(s.deviceName, 0, sizeof(s.deviceName));
  snprintf(s.deviceName, sizeof(s.deviceName), "%s", "teletrack");
  s.sampleHz = 10;
  return s;
}

bool Settings::isValidSampleHz(uint8_t hz) {
  return hz == 1 || hz == 5 || hz == 10 || hz == 25;
}

static bool isLegalNameChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

ValidationResult Settings::validate() const {
  ValidationResult result;

  const size_t len = strnlen(deviceName, kDeviceNameSize);
  bool nameOk = len >= 1 && len <= kDeviceNameSize - 1;
  if (nameOk) {
    for (size_t i = 0; i < len; ++i) {
      if (!isLegalNameChar(deviceName[i])) {
        nameOk = false;
        break;
      }
    }
  }
  if (!nameOk) {
    result.add("deviceName", SettingsError::kDeviceName);
  }

  if (!isValidSampleHz(sampleHz)) {
    result.add("sampleHz", SettingsError::kSampleHz);
  }

  return result;
}
