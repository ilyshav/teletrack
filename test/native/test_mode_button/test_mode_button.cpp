#include <unity.h>

#include "radio/HoldDetector.h"

void setUp() {}
void tearDown() {}

static void test_idle_never_fires() {
  HoldDetector d;
  for (uint32_t t = 0; t < 10000; t += 100) {
    TEST_ASSERT_FALSE(d.update(false, t));
  }
}

static void test_fires_once_exactly_at_the_hold_threshold() {
  HoldDetector d;
  TEST_ASSERT_FALSE(d.update(true, 0));
  TEST_ASSERT_FALSE(d.update(true, 2999));
  TEST_ASSERT_TRUE(d.update(true, 3000));
}

static void test_does_not_fire_again_while_still_held() {
  HoldDetector d;
  d.update(true, 0);
  TEST_ASSERT_TRUE(d.update(true, 3000));
  for (uint32_t t = 3100; t < 12000; t += 100) {
    TEST_ASSERT_FALSE(d.update(true, t));
  }
}

static void test_release_before_threshold_fires_nothing() {
  HoldDetector d;
  d.update(true, 0);
  d.update(true, 2500);
  TEST_ASSERT_FALSE(d.update(false, 2600));
  TEST_ASSERT_FALSE(d.isHolding());
}

static void test_release_then_hold_again_fires() {
  HoldDetector d;
  d.update(true, 0);
  d.update(false, 1000);
  d.update(true, 2000);
  TEST_ASSERT_FALSE(d.update(true, 4999));
  TEST_ASSERT_TRUE(d.update(true, 5000));  // 3000 ms after the second press
}

static void test_bounce_shorter_than_debounce_does_not_restart_the_hold() {
  HoldDetector d;
  d.update(true, 0);
  // A 10 ms glitch to released, well under kDebounceMs.
  d.update(false, 1000);
  d.update(true, 1010);
  // Still measured from t=0, so it fires at 3000, not 4010.
  TEST_ASSERT_TRUE(d.update(true, 3000));
}

static void test_release_longer_than_debounce_does_restart_the_hold() {
  HoldDetector d;
  d.update(true, 0);
  d.update(false, 1000);
  d.update(false, 1100);  // 100 ms released, past kDebounceMs
  d.update(true, 1200);
  TEST_ASSERT_FALSE(d.update(true, 3000));   // would have fired on the old timer
  TEST_ASSERT_TRUE(d.update(true, 4200));    // 3000 ms after the restart
}

static void test_held_ms_reports_progress() {
  HoldDetector d;
  TEST_ASSERT_EQUAL_UINT32(0u, d.heldMs(0));
  d.update(true, 1000);
  TEST_ASSERT_EQUAL_UINT32(500u, d.heldMs(1500));
  TEST_ASSERT_TRUE(d.isHolding());
  d.update(false, 2000);
  TEST_ASSERT_EQUAL_UINT32(0u, d.heldMs(2500));
}

static void test_held_ms_is_zero_when_not_pressed() {
  HoldDetector d;
  TEST_ASSERT_EQUAL_UINT32(0u, d.heldMs(5000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_idle_never_fires);
  RUN_TEST(test_fires_once_exactly_at_the_hold_threshold);
  RUN_TEST(test_does_not_fire_again_while_still_held);
  RUN_TEST(test_release_before_threshold_fires_nothing);
  RUN_TEST(test_release_then_hold_again_fires);
  RUN_TEST(test_bounce_shorter_than_debounce_does_not_restart_the_hold);
  RUN_TEST(test_release_longer_than_debounce_does_restart_the_hold);
  RUN_TEST(test_held_ms_reports_progress);
  RUN_TEST(test_held_ms_is_zero_when_not_pressed);
  return UNITY_END();
}
