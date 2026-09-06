#include "config/internal/ConfigApi.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace ConfigApi {
namespace {

size_t serializeIfItFits(const JsonDocument& doc, char* out, size_t outSize) {
  if (out == nullptr || measureJson(doc) + 1 > outSize) {
    return 0;
  }
  return serializeJson(doc, out, outSize);
}

}  // namespace

size_t toJson(const Settings& s, char* out, size_t outSize) {
  JsonDocument doc;
  doc["schemaVersion"] = s.schemaVersion;
  doc["deviceName"] = s.deviceName;
  doc["sampleHz"] = s.sampleHz;
  return serializeIfItFits(doc, out, outSize);
}

size_t okToJson(char* out, size_t outSize) {
  if (out == nullptr) {
    return 0;
  }
  const int n = snprintf(out, outSize, "{\"ok\":true}");
  if (n < 0 || static_cast<size_t>(n) >= outSize) {
    return 0;
  }
  return static_cast<size_t>(n);
}

size_t errorsToJson(const ValidationResult& v, char* out, size_t outSize) {
  JsonDocument doc;
  doc["ok"] = false;
  JsonObject errors = doc["errors"].to<JsonObject>();
  for (size_t i = 0; i < v.count; ++i) {
    errors[v.errors[i].field] = v.errors[i].message;
  }
  return serializeIfItFits(doc, out, outSize);
}

size_t storageErrorToJson(char* out, size_t outSize) {
  ValidationResult v;
  v.add("_", SettingsError::kStorage);
  return errorsToJson(v, out, outSize);
}

ParseResult applyJson(const char* body, size_t len, Settings& inOut) {
  ParseResult result;
  result.status = ParseStatus::BadJson;

  if (body == nullptr || len == 0 || len > kMaxBodyBytes) {
    return result;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) {
    return result;
  }
  if (!doc.is<JsonObject>()) {
    return result;
  }
  JsonObject obj = doc.as<JsonObject>();

  // Build a candidate. inOut is only overwritten if everything checks out.
  Settings candidate = inOut;
  candidate.schemaVersion = Settings::kSchemaVersion;  // never client-supplied

  ValidationResult typeErrors;

  if (!obj["deviceName"].isNull()) {
    const char* name = obj["deviceName"].is<const char*>()
                           ? obj["deviceName"].as<const char*>()
                           : nullptr;
    // An over-long name is rejected, never silently clipped to the buffer size.
    if (name == nullptr || strlen(name) >= Settings::kDeviceNameSize) {
      typeErrors.add("deviceName", SettingsError::kDeviceName);
    } else {
      memset(candidate.deviceName, 0, sizeof(candidate.deviceName));
      memcpy(candidate.deviceName, name, strlen(name));
    }
  }

  if (!obj["sampleHz"].isNull()) {
    if (!obj["sampleHz"].is<unsigned int>()) {
      typeErrors.add("sampleHz", SettingsError::kSampleHz);
    } else {
      const unsigned int hz = obj["sampleHz"].as<unsigned int>();
      if (hz > 255u || !Settings::isValidSampleHz(static_cast<uint8_t>(hz))) {
        typeErrors.add("sampleHz", SettingsError::kSampleHz);
      } else {
        candidate.sampleHz = static_cast<uint8_t>(hz);
      }
    }
  }

  if (!typeErrors.ok()) {
    result.status = ParseStatus::Invalid;
    result.validation = typeErrors;
    return result;
  }

  const ValidationResult validation = candidate.validate();
  if (!validation.ok()) {
    result.status = ParseStatus::Invalid;
    result.validation = validation;
    return result;
  }

  inOut = candidate;
  result.status = ParseStatus::Ok;
  return result;
}

}  // namespace ConfigApi
