#include "config/internal/ConfigService.h"

namespace ConfigService {
namespace {

SaveOutcome singleError(const char* message, char* out, size_t outSize,
                        uint16_t status) {
  ValidationResult v;
  v.add("_", message);
  SaveOutcome outcome;
  outcome.httpStatus = status;
  outcome.bodyLength = ConfigApi::errorsToJson(v, out, outSize);
  return outcome;
}

}  // namespace

SaveOutcome save(const char* requestBody, size_t len, Settings& settings,
                 SettingsStore& store, char* out, size_t outSize) {
  if (len > ConfigApi::kMaxBodyBytes) {
    return singleError(SettingsError::kTooLarge, out, outSize, 413);
  }

  const Settings previous = settings;
  const ConfigApi::ParseResult parsed =
      ConfigApi::applyJson(requestBody, len, settings);

  if (parsed.status == ConfigApi::ParseStatus::BadJson) {
    return singleError(SettingsError::kBadJson, out, outSize, 400);
  }
  if (parsed.status == ConfigApi::ParseStatus::Invalid) {
    SaveOutcome outcome;
    outcome.httpStatus = 400;
    outcome.bodyLength = ConfigApi::errorsToJson(parsed.validation, out, outSize);
    return outcome;
  }

  if (!store.save(settings)) {
    settings = previous;
    SaveOutcome outcome;
    outcome.httpStatus = 500;
    outcome.bodyLength = ConfigApi::storageErrorToJson(out, outSize);
    return outcome;
  }

  SaveOutcome outcome;
  outcome.httpStatus = 200;
  outcome.bodyLength = ConfigApi::okToJson(out, outSize);
  return outcome;
}

}  // namespace ConfigService
