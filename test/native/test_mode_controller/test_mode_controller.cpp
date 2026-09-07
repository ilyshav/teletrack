#include <unity.h>

#include "radio/RadioMode.h"

void setUp() {}
void tearDown() {}

static void test_boots_into_ble() {
  ModeController c;
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Ble);
  TEST_ASSERT_FALSE(c.switchPending());
}

static void test_button_hold_toggles_to_wifi() {
  ModeController c;
  TEST_ASSERT_TRUE(c.handle(ModeEvent::ButtonHeld));
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Wifi);
  TEST_ASSERT_TRUE(c.switchPending());
}

static void test_button_hold_toggles_back_to_ble() {
  ModeController c;
  c.handle(ModeEvent::ButtonHeld);
  c.switchComplete();
  TEST_ASSERT_TRUE(c.handle(ModeEvent::ButtonHeld));
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Ble);
}

static void test_event_while_switch_pending_is_ignored() {
  ModeController c;
  TEST_ASSERT_TRUE(c.handle(ModeEvent::ButtonHeld));
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Wifi);

  // Radio teardown is not reentrant: a second hold mid-switch must not start
  // another one, and must not flip the mode back.
  TEST_ASSERT_FALSE(c.handle(ModeEvent::ButtonHeld));
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Wifi);
  TEST_ASSERT_TRUE(c.switchPending());
}

static void test_switch_complete_clears_pending_and_reenables_events() {
  ModeController c;
  c.handle(ModeEvent::ButtonHeld);
  c.switchComplete();
  TEST_ASSERT_FALSE(c.switchPending());
  TEST_ASSERT_TRUE(c.handle(ModeEvent::ButtonHeld));
}

static void test_switch_complete_without_pending_is_harmless() {
  ModeController c;
  c.switchComplete();
  TEST_ASSERT_TRUE(c.mode() == RadioMode::Ble);
  TEST_ASSERT_FALSE(c.switchPending());
}

static void test_many_toggles_alternate() {
  ModeController c;
  for (int i = 0; i < 10; ++i) {
    c.handle(ModeEvent::ButtonHeld);
    c.switchComplete();
    const RadioMode expected = (i % 2 == 0) ? RadioMode::Wifi : RadioMode::Ble;
    TEST_ASSERT_TRUE(c.mode() == expected);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_boots_into_ble);
  RUN_TEST(test_button_hold_toggles_to_wifi);
  RUN_TEST(test_button_hold_toggles_back_to_ble);
  RUN_TEST(test_event_while_switch_pending_is_ignored);
  RUN_TEST(test_switch_complete_clears_pending_and_reenables_events);
  RUN_TEST(test_switch_complete_without_pending_is_harmless);
  RUN_TEST(test_many_toggles_alternate);
  return UNITY_END();
}
