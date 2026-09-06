#include <Arduino.h>
#include <unity.h>

#include "config/internal/NvsStore.h"

void setUp() {}
void tearDown() {}

static void test_begin_succeeds() {
  NvsStore store;
  TEST_ASSERT_TRUE(store.begin());
  store.end();
}

static void test_operations_fail_before_begin() {
  NvsStore store;
  Settings s = Settings::defaults();
  TEST_ASSERT_FALSE(store.save(s));
  TEST_ASSERT_FALSE(store.load(s));
}

static void test_save_then_load_round_trips() {
  NvsStore store;
  TEST_ASSERT_TRUE(store.begin());

  Settings in = Settings::defaults();
  in.sampleHz = 25;
  snprintf(in.deviceName, sizeof(in.deviceName), "%s", "nvs-round-trip");
  TEST_ASSERT_TRUE(store.save(in));

  Settings out;
  TEST_ASSERT_TRUE(store.load(out));
  TEST_ASSERT_EQUAL_UINT8(25, out.sampleHz);
  TEST_ASSERT_EQUAL_STRING("nvs-round-trip", out.deviceName);
  store.end();
}

static void test_load_survives_a_close_and_reopen() {
  {
    NvsStore store;
    TEST_ASSERT_TRUE(store.begin());
    Settings in = Settings::defaults();
    in.sampleHz = 5;
    snprintf(in.deviceName, sizeof(in.deviceName), "%s", "persisted");
    TEST_ASSERT_TRUE(store.save(in));
    store.end();
  }
  {
    NvsStore store;
    TEST_ASSERT_TRUE(store.begin());
    Settings out;
    TEST_ASSERT_TRUE(store.load(out));
    TEST_ASSERT_EQUAL_UINT8(5, out.sampleHz);
    TEST_ASSERT_EQUAL_STRING("persisted", out.deviceName);
    store.end();
  }
}

static void test_corrupt_stored_value_is_rejected() {
  NvsStore store;
  TEST_ASSERT_TRUE(store.begin());

  Settings good = Settings::defaults();
  TEST_ASSERT_TRUE(store.save(good));

  // Write a sample rate that is not one of the four allowed values, behind the
  // store's back, the way a firmware downgrade or a flash bit-flip would.
  Preferences raw;
  TEST_ASSERT_TRUE(raw.begin(NvsStore::kNamespace, false));
  raw.putUChar("hz", 77);
  raw.end();

  Settings out = Settings::defaults();
  out.sampleHz = 10;
  TEST_ASSERT_FALSE(store.load(out));   // refuses to hand back invalid settings
  TEST_ASSERT_EQUAL_UINT8(10, out.sampleHz);  // and leaves the caller's defaults
  store.end();
}

void setup() {
  // Native USB CDC discards writes until the host opens the port, and the host
  // only does that after the post-upload re-enumeration. Wait for it rather
  // than guessing a delay, with a bound so a headless boot still runs.
  Serial.begin(115200);
  const uint32_t deadline = millis() + 10000;
  while (!Serial && millis() < deadline) {
    delay(10);
  }
  delay(200);  // let the host settle before the first write

  UNITY_BEGIN();
  RUN_TEST(test_begin_succeeds);
  RUN_TEST(test_operations_fail_before_begin);
  RUN_TEST(test_save_then_load_round_trips);
  RUN_TEST(test_load_survives_a_close_and_reopen);
  RUN_TEST(test_corrupt_stored_value_is_rejected);
  UNITY_END();
}

void loop() {}
