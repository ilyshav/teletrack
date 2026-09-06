#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "config/internal/ConfigApi.h"

void setUp() {}
void tearDown() {}

static void test_to_json_shape() {
  char buf[ConfigApi::kJsonBufferSize];
  const size_t n = ConfigApi::toJson(Settings::defaults(), buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING(
      "{\"schemaVersion\":1,\"deviceName\":\"teletrack\",\"sampleHz\":10}", buf);
}

static void test_to_json_refuses_a_buffer_that_is_too_small() {
  char buf[8];
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)ConfigApi::toJson(Settings::defaults(), buf, sizeof(buf)));
}

static void test_ok_json() {
  char buf[32];
  const size_t n = ConfigApi::okToJson(buf, sizeof(buf));
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_STRING("{\"ok\":true}", buf);
}

static void test_storage_error_json() {
  char buf[ConfigApi::kJsonBufferSize];
  ConfigApi::storageErrorToJson(buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"_\":\"could not write to storage\"}}", buf);
}

static void test_errors_json_shape() {
  ValidationResult v;
  v.add("sampleHz", SettingsError::kSampleHz);
  char buf[ConfigApi::kJsonBufferSize];
  ConfigApi::errorsToJson(v, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "{\"ok\":false,\"errors\":{\"sampleHz\":\"must be 1, 5, 10 or 25\"}}", buf);
}

static void test_apply_valid_payload_updates_settings() {
  Settings s = Settings::defaults();
  const char* body = "{\"deviceName\":\"bike-one\",\"sampleHz\":25}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_STRING("bike-one", s.deviceName);
  TEST_ASSERT_EQUAL_UINT8(25, s.sampleHz);
}

static void test_apply_partial_payload_leaves_other_fields_alone() {
  Settings s = Settings::defaults();
  const char* body = "{\"sampleHz\":5}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_UINT8(5, s.sampleHz);
  TEST_ASSERT_EQUAL_STRING("teletrack", s.deviceName);
}

static void test_apply_ignores_schema_version_from_the_client() {
  Settings s = Settings::defaults();
  const char* body = "{\"schemaVersion\":99,\"sampleHz\":5}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_UINT8(Settings::kSchemaVersion, s.schemaVersion);
}

static void test_apply_malformed_json_is_bad_json() {
  Settings s = Settings::defaults();
  const char* body = "{not json";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::BadJson);
  TEST_ASSERT_EQUAL_UINT8(10, s.sampleHz);  // untouched
}

static void test_apply_non_object_json_is_bad_json() {
  Settings s = Settings::defaults();
  const char* body = "[1,2,3]";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);
  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::BadJson);
}

static void test_apply_writes_nothing_when_any_field_is_invalid() {
  Settings s = Settings::defaults();
  // deviceName is fine, sampleHz is not. Neither may be applied.
  const char* body = "{\"deviceName\":\"bike-one\",\"sampleHz\":7}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Invalid);
  TEST_ASSERT_EQUAL_STRING(SettingsError::kSampleHz, r.validation.messageFor("sampleHz"));
  TEST_ASSERT_EQUAL_STRING("teletrack", s.deviceName);  // rolled back
  TEST_ASSERT_EQUAL_UINT8(10, s.sampleHz);
}

static void test_apply_rejects_an_oversized_device_name_rather_than_truncating() {
  Settings s = Settings::defaults();
  // 40 characters — must NOT be silently clipped to 31.
  const char* body =
      "{\"deviceName\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Invalid);
  TEST_ASSERT_EQUAL_STRING(SettingsError::kDeviceName, r.validation.messageFor("deviceName"));
  TEST_ASSERT_EQUAL_STRING("teletrack", s.deviceName);
}

static void test_apply_rejects_wrong_field_types() {
  Settings s = Settings::defaults();
  const char* body = "{\"deviceName\":42,\"sampleHz\":\"ten\"}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Invalid);
  TEST_ASSERT_EQUAL_STRING(SettingsError::kDeviceName, r.validation.messageFor("deviceName"));
  TEST_ASSERT_EQUAL_STRING(SettingsError::kSampleHz, r.validation.messageFor("sampleHz"));
}

static void test_apply_empty_object_is_a_no_op_success() {
  Settings s = Settings::defaults();
  const char* body = "{}";
  const ConfigApi::ParseResult r = ConfigApi::applyJson(body, strlen(body), s);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_UINT8(10, s.sampleHz);
}

static void test_round_trip_through_json() {
  Settings original = Settings::defaults();
  original.sampleHz = 25;
  snprintf(original.deviceName, sizeof(original.deviceName), "%s", "round-trip");

  char buf[ConfigApi::kJsonBufferSize];
  const size_t n = ConfigApi::toJson(original, buf, sizeof(buf));

  Settings restored = Settings::defaults();
  const ConfigApi::ParseResult r = ConfigApi::applyJson(buf, n, restored);

  TEST_ASSERT_TRUE(r.status == ConfigApi::ParseStatus::Ok);
  TEST_ASSERT_EQUAL_UINT8(25, restored.sampleHz);
  TEST_ASSERT_EQUAL_STRING("round-trip", restored.deviceName);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_to_json_shape);
  RUN_TEST(test_to_json_refuses_a_buffer_that_is_too_small);
  RUN_TEST(test_ok_json);
  RUN_TEST(test_storage_error_json);
  RUN_TEST(test_errors_json_shape);
  RUN_TEST(test_apply_valid_payload_updates_settings);
  RUN_TEST(test_apply_partial_payload_leaves_other_fields_alone);
  RUN_TEST(test_apply_ignores_schema_version_from_the_client);
  RUN_TEST(test_apply_malformed_json_is_bad_json);
  RUN_TEST(test_apply_non_object_json_is_bad_json);
  RUN_TEST(test_apply_writes_nothing_when_any_field_is_invalid);
  RUN_TEST(test_apply_rejects_an_oversized_device_name_rather_than_truncating);
  RUN_TEST(test_apply_rejects_wrong_field_types);
  RUN_TEST(test_apply_empty_object_is_a_no_op_success);
  RUN_TEST(test_round_trip_through_json);
  return UNITY_END();
}
