#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "config/internal/MemoryStore.h"

void setUp() {}
void tearDown() {}

static void test_load_on_empty_store_returns_false_and_leaves_out_untouched() {
  MemoryStore store;
  Settings out = Settings::defaults();
  out.sampleHz = 25;

  TEST_ASSERT_FALSE(store.load(out));
  TEST_ASSERT_EQUAL_UINT8(25, out.sampleHz);  // untouched
}

static void test_save_then_load_round_trips() {
  MemoryStore store;
  Settings in = Settings::defaults();
  in.sampleHz = 25;
  snprintf(in.deviceName, sizeof(in.deviceName), "%s", "bike-one");

  TEST_ASSERT_TRUE(store.save(in));

  Settings out;
  TEST_ASSERT_TRUE(store.load(out));
  TEST_ASSERT_EQUAL_UINT8(25, out.sampleHz);
  TEST_ASSERT_EQUAL_STRING("bike-one", out.deviceName);
}

static void test_fail_next_save_fails_once_and_stores_nothing() {
  MemoryStore store;
  Settings in = Settings::defaults();
  in.sampleHz = 25;

  store.failNextSave();
  TEST_ASSERT_FALSE(store.save(in));
  TEST_ASSERT_FALSE(store.hasStored());

  TEST_ASSERT_TRUE(store.save(in));  // the next one succeeds
  TEST_ASSERT_TRUE(store.hasStored());
}

static void test_usable_through_the_base_interface() {
  MemoryStore concrete;
  SettingsStore& store = concrete;

  Settings in = Settings::defaults();
  in.sampleHz = 5;
  TEST_ASSERT_TRUE(store.save(in));

  Settings out;
  TEST_ASSERT_TRUE(store.load(out));
  TEST_ASSERT_EQUAL_UINT8(5, out.sampleHz);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_load_on_empty_store_returns_false_and_leaves_out_untouched);
  RUN_TEST(test_save_then_load_round_trips);
  RUN_TEST(test_fail_next_save_fails_once_and_stores_nothing);
  RUN_TEST(test_usable_through_the_base_interface);
  return UNITY_END();
}
