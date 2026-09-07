#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "config/Settings.h"

void setUp() {}
void tearDown() {}

static Settings withName(const char* name) {
  Settings s = Settings::defaults();
  memset(s.deviceName, 0, sizeof(s.deviceName));
  snprintf(s.deviceName, sizeof(s.deviceName), "%s", name);
  return s;
}

static void test_defaults() {
  const Settings s = Settings::defaults();
  TEST_ASSERT_EQUAL_STRING("teletrack", s.deviceName);
  TEST_ASSERT_EQUAL_UINT8(10, s.sampleHz);
  TEST_ASSERT_TRUE(s.validate().ok());
}

static void test_device_name_empty_is_invalid() {
  const ValidationResult v = withName("").validate();
  TEST_ASSERT_FALSE(v.ok());
  TEST_ASSERT_EQUAL_STRING(SettingsError::kDeviceName, v.messageFor("deviceName"));
}

static void test_device_name_single_char_is_valid() {
  TEST_ASSERT_TRUE(withName("a").validate().ok());
}

static void test_device_name_31_chars_is_valid() {
  char name[32];
  memset(name, 'a', 31);
  name[31] = '\0';
  TEST_ASSERT_TRUE(withName(name).validate().ok());
}

static void test_device_name_rejects_illegal_characters() {
  TEST_ASSERT_FALSE(withName("has space").validate().ok());
  TEST_ASSERT_FALSE(withName("has.dot").validate().ok());
  TEST_ASSERT_FALSE(withName("has/slash").validate().ok());
}

static void test_device_name_accepts_legal_characters() {
  TEST_ASSERT_TRUE(withName("Track-Day_01").validate().ok());
}

static void test_sample_hz_accepts_only_the_four_rates() {
  const uint8_t valid[] = {1, 5, 10, 25};
  for (uint8_t rate : valid) {
    Settings s = Settings::defaults();
    s.sampleHz = rate;
    TEST_ASSERT_TRUE(s.validate().ok());
  }
  const uint8_t invalid[] = {0, 2, 9, 11, 24, 26, 255};
  for (uint8_t rate : invalid) {
    Settings s = Settings::defaults();
    s.sampleHz = rate;
    const ValidationResult v = s.validate();
    TEST_ASSERT_FALSE(v.ok());
    TEST_ASSERT_EQUAL_STRING(SettingsError::kSampleHz, v.messageFor("sampleHz"));
  }
}

static void test_reports_multiple_errors_together() {
  Settings s = withName("bad name");
  s.sampleHz = 3;
  const ValidationResult v = s.validate();
  TEST_ASSERT_EQUAL_UINT(2u, (unsigned)v.count);
  TEST_ASSERT_EQUAL_STRING(SettingsError::kDeviceName, v.messageFor("deviceName"));
  TEST_ASSERT_EQUAL_STRING(SettingsError::kSampleHz, v.messageFor("sampleHz"));
}

static void test_message_for_unknown_field_is_null() {
  TEST_ASSERT_NULL(Settings::defaults().validate().messageFor("nope"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_defaults);
  RUN_TEST(test_device_name_empty_is_invalid);
  RUN_TEST(test_device_name_single_char_is_valid);
  RUN_TEST(test_device_name_31_chars_is_valid);
  RUN_TEST(test_device_name_rejects_illegal_characters);
  RUN_TEST(test_device_name_accepts_legal_characters);
  RUN_TEST(test_sample_hz_accepts_only_the_four_rates);
  RUN_TEST(test_reports_multiple_errors_together);
  RUN_TEST(test_message_for_unknown_field_is_null);
  return UNITY_END();
}
