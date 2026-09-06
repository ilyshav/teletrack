#pragma once

#include <stddef.h>
#include <stdint.h>

// The exact strings sent to the client. Single source of truth — the web UI,
// ConfigApi and the tests all reference these, never literals.
namespace SettingsError {
inline constexpr const char* kDeviceName = "1-31 characters, letters digits _ - only";
inline constexpr const char* kSampleHz = "must be 1, 5, 10 or 25";
inline constexpr const char* kStorage = "could not write to storage";
}  // namespace SettingsError

struct ValidationResult {
  static constexpr size_t kMaxErrors = 4;

  struct Error {
    const char* field;
    const char* message;
  };

  Error errors[kMaxErrors] = {};
  size_t count = 0;

  bool ok() const { return count == 0; }

  // Adds an error, replacing any existing error for the same field. Silently
  // drops the error if kMaxErrors is already reached — there are only two
  // fields, so that cannot happen in practice.
  void add(const char* field, const char* message);

  // Returns nullptr when the field has no error.
  const char* messageFor(const char* field) const;
};

struct Settings {
  static constexpr uint8_t kSchemaVersion = 1;
  static constexpr size_t kDeviceNameSize = 32;  // 31 usable characters + NUL

  uint8_t schemaVersion = kSchemaVersion;
  char deviceName[kDeviceNameSize] = {};
  uint8_t sampleHz = 10;

  static Settings defaults();

  // Pure. Never touches storage, JSON or the network.
  ValidationResult validate() const;

  static bool isValidSampleHz(uint8_t hz);
};
