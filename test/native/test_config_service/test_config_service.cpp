#include <unity.h>
#include <string.h>

#include "config/internal/ConfigService.h"
#include "config/internal/MemoryStore.h"

void setUp() {}
void tearDown() {}

static void test_valid_save_returns_200_and_persists() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{\"deviceName\":\"bike-one\",\"sampleHz\":25}";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(200, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", body);
  TEST_ASSERT_EQUAL_UINT8(25, settings.sampleHz);
  TEST_ASSERT_TRUE(store.hasStored());
  TEST_ASSERT_EQUAL_STRING("bike-one", store.stored().deviceName);
}

static void test_invalid_field_returns_400_and_persists_nothing() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{\"deviceName\":\"bike-one\",\"sampleHz\":7}";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(400, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"sampleHz\":\"must be 1, 5, 10 or 25\"}}", body);
  TEST_ASSERT_EQUAL_STRING("teletrack", settings.deviceName);
  TEST_ASSERT_EQUAL_UINT8(10, settings.sampleHz);
  TEST_ASSERT_FALSE(store.hasStored());
}

static void test_malformed_json_returns_400() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{not json";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(400, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"_\":\"malformed JSON body\"}}", body);
  TEST_ASSERT_FALSE(store.hasStored());
}

static void test_oversized_body_returns_413_without_parsing() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const ConfigService::SaveOutcome out = ConfigService::save(
      "{}", ConfigApi::kMaxBodyBytes + 1, settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(413, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"_\":\"body exceeds 1024 bytes\"}}", body);
  TEST_ASSERT_FALSE(store.hasStored());
}

static void test_storage_write_failure_returns_500_and_rolls_back() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  store.failNextSave();
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{\"deviceName\":\"bike-one\",\"sampleHz\":25}";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT16(500, out.httpStatus);
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"_\":\"could not write to storage\"}}", body);
  // The in-memory settings must not keep a change that was never persisted.
  TEST_ASSERT_EQUAL_STRING("teletrack", settings.deviceName);
  TEST_ASSERT_EQUAL_UINT8(10, settings.sampleHz);
}

static void test_body_length_matches_what_was_written() {
  Settings settings = Settings::defaults();
  MemoryStore store;
  char body[ConfigApi::kJsonBufferSize];

  const char* request = "{\"sampleHz\":5}";
  const ConfigService::SaveOutcome out =
      ConfigService::save(request, strlen(request), settings, store, body, sizeof(body));

  TEST_ASSERT_EQUAL_UINT((unsigned)strlen(body), (unsigned)out.bodyLength);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_valid_save_returns_200_and_persists);
  RUN_TEST(test_invalid_field_returns_400_and_persists_nothing);
  RUN_TEST(test_malformed_json_returns_400);
  RUN_TEST(test_oversized_body_returns_413_without_parsing);
  RUN_TEST(test_storage_write_failure_returns_500_and_rolls_back);
  RUN_TEST(test_body_length_matches_what_was_written);
  return UNITY_END();
}
