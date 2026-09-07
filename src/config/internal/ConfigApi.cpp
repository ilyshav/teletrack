#include "config/internal/ConfigApi.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

namespace ConfigApi {

size_t toJson(const Settings& s, char* out, size_t outSize) {
  JsonDocument doc;
  doc["deviceName"] = s.deviceName;
  doc["sampleHz"] = s.sampleHz;
  return serializeJson(doc, out, outSize);
}

size_t okToJson(char* out, size_t outSize) {
  return static_cast<size_t>(snprintf(out, outSize, "{\"ok\":true}"));
}

size_t errorsToJson(const ValidationResult& v, char* out, size_t outSize) {
  JsonDocument doc;
  doc["ok"] = false;
  JsonObject errors = doc["errors"].to<JsonObject>();
  for (size_t i = 0; i < v.count; ++i) {
    errors[v.errors[i].field] = v.errors[i].message;
  }
  return serializeJson(doc, out, outSize);
}

size_t storageErrorToJson(char* out, size_t outSize) {
  ValidationResult v;
  v.add("_", SettingsError::kStorage);
  return errorsToJson(v, out, outSize);
}

size_t statusToJson(const DeviceStatus& status, char* out, size_t outSize) {
  JsonDocument doc;
  doc["ssid"] = status.ssid;
  doc["ip"] = status.ip;
  doc["clients"] = status.clients;
  doc["uptimeMs"] = status.uptimeMs;
  doc["freeHeap"] = status.freeHeap;
  doc["apUp"] = status.apUp;
  return serializeJson(doc, out, outSize);
}

ParseResult applyJson(const char* body, size_t len, Settings& inOut) {
  ParseResult result;
  result.status = ParseStatus::BadJson;

  if (len > kMaxBodyBytes) {
    return result;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body, len) != DeserializationError::Ok) {
    return result;
  }
  if (!doc.is<JsonObject>()) {
    return result;
  }
  JsonObjectConst obj = doc.as<JsonObjectConst>();

  // Build a candidate. inOut is only overwritten if everything checks out.
  Settings candidate = inOut;

  ValidationResult typeErrors;

  // Presence is tested with is<JsonVariantConst>(), not isNull(): isNull() is
  // true both for an absent key and for a key whose value is an explicit null,
  // so it would report "saved" while silently discarding {"sampleHz":null}.
  const JsonVariantConst nameField = obj["deviceName"];
  if (nameField.is<JsonVariantConst>()) {
    const char* name = nameField.as<const char*>();
    // An over-long name is rejected, never silently clipped to the buffer size.
    if (!nameField.is<const char*>() ||
        strlen(name) >= Settings::kDeviceNameSize) {
      typeErrors.add("deviceName", SettingsError::kDeviceName);
    } else {
      snprintf(candidate.deviceName, sizeof(candidate.deviceName), "%s", name);
    }
  }

  const JsonVariantConst hzField = obj["sampleHz"];
  if (hzField.is<JsonVariantConst>()) {
    if (!hzField.is<unsigned int>()) {
      typeErrors.add("sampleHz", SettingsError::kSampleHz);
    } else {
      // The range check below is what rejects negatives, not the type check
      // above: ArduinoJson's integral check ignores the requested type, so
      // is<unsigned int>() accepts -1 and as<unsigned int>() wraps it to
      // 4294967295. Do not remove `hz > 255u` as redundant.
      const unsigned int hz = hzField.as<unsigned int>();
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
