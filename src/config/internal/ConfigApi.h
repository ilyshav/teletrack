#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config/Settings.h"

// Wire format for the settings API. Pure: no HTTP, no storage. The *ToJson
// functions return the number of characters written; callers pass a
// kJsonBufferSize buffer, which is far larger than any document produced here.
namespace ConfigApi {

// Largest POST body accepted. Anything longer is rejected before buffering.
inline constexpr size_t kMaxBodyBytes = 1024;
// Large enough for any document this module produces.
inline constexpr size_t kJsonBufferSize = 512;

enum class ParseStatus : uint8_t {
  Ok,       // body parsed, validated, and applied to inOut
  BadJson,  // not parseable, or not a JSON object
  Invalid,  // parsed but failed validation; inOut untouched
};

struct ParseResult {
  ParseStatus status = ParseStatus::BadJson;
  ValidationResult validation;
};

size_t toJson(const Settings& s, char* out, size_t outSize);

// Applies the fields present in `body` on top of `inOut`. `inOut` is modified
// only when the returned status is Ok — a payload with one bad field changes
// nothing.
ParseResult applyJson(const char* body, size_t len, Settings& inOut);

size_t okToJson(char* out, size_t outSize);
size_t errorsToJson(const ValidationResult& v, char* out, size_t outSize);
size_t storageErrorToJson(char* out, size_t outSize);

}  // namespace ConfigApi
