#include <unity.h>

#include "radio/HoldDetector.h"

void setUp() {}
void tearDown() {}

namespace {

// Feeds a press of `pressMs`, then a release of `releaseMs`, one sample per
// millisecond, and returns the last non-None event seen. One sample per ms is
// far finer than loop() manages, which is the point: the detector must not
// depend on sample spacing.
ButtonEvent feed(HoldDetector& d, uint32_t& now, uint32_t pressMs,
                 uint32_t releaseMs) {
  ButtonEvent seen = ButtonEvent::None;
  for (uint32_t i = 0; i < pressMs; ++i) {
    const ButtonEvent e = d.update(true, now++);
    if (e != ButtonEvent::None) {
      seen = e;
    }
  }
  for (uint32_t i = 0; i < releaseMs; ++i) {
    const ButtonEvent e = d.update(false, now++);
    if (e != ButtonEvent::None) {
      seen = e;
    }
  }
  return seen;
}

}  // namespace

static void test_idle_never_fires() {
  HoldDetector d;
  for (uint32_t t = 0; t < 10000; t += 10) {
    TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(false, t));
  }
}

static void test_hold_fires_once_at_the_threshold_while_still_down() {
  HoldDetector d;
  TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(true, 0));
  TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(true, HoldDetector::kHoldMs - 1));
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, d.update(true, HoldDetector::kHoldMs));
  // Still held: must not repeat.
  TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(true, HoldDetector::kHoldMs + 5000));
}

static void test_release_before_the_threshold_fires_no_hold() {
  HoldDetector d;
  d.update(true, 0);
  d.update(true, 2999);
  TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(false, 3000));
}

static void test_release_then_hold_again_fires_again() {
  HoldDetector d;
  d.update(true, 0);
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, d.update(true, 3000));
  for (uint32_t t = 3001; t < 3200; ++t) {
    d.update(false, t);
  }
  d.update(true, 4000);
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, d.update(true, 7000));
}

static void test_bounce_shorter_than_debounce_does_not_restart_the_hold() {
  HoldDetector d;
  d.update(true, 0);
  d.update(false, 1000);              // contact drops
  d.update(true, 1000 + HoldDetector::kDebounceMs - 1);  // and returns
  // The hold never broke, so it still completes 3000 ms after the first press.
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, d.update(true, 3000));
}

static void test_three_clicks_fire_triple_click() {
  HoldDetector d;
  uint32_t now = 0;
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
  TEST_ASSERT_EQUAL(ButtonEvent::TripleClick, feed(d, now, 60, 100));
}

static void test_two_clicks_alone_fire_nothing() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
}

static void test_a_fourth_click_does_not_fire_again() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);
  feed(d, now, 60, 100);
  TEST_ASSERT_EQUAL(ButtonEvent::TripleClick, feed(d, now, 60, 100));
  // The sequence is consumed; a lone click afterwards starts a new one.
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
}

static void test_clicks_spread_past_the_window_do_not_accumulate() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);  // click one
  // Click two, then a gap longer than the window. Note the gap has to come
  // BEFORE the third click: put it after and the third click completes the
  // sequence normally and the test passes for the wrong reason.
  feed(d, now, 60, HoldDetector::kMultiClickWindowMs + 200);
  // So this click lands outside the window the first one opened, and starts a
  // fresh sequence rather than completing the stale one.
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
}

static void test_a_long_press_is_not_a_click() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);
  feed(d, now, 60, 100);
  // Two clicks, then a press longer than kClickMaxMs. Asserting on this call
  // is the point: without it, deleting the guard entirely still passes. The
  // long press would push the count to three, fire a TripleClick nobody
  // checked, and reset the count -- leaving the final assertion below true for
  // completely the wrong reason.
  TEST_ASSERT_EQUAL(ButtonEvent::None,
                    feed(d, now, HoldDetector::kClickMaxMs + 100, 100));
  TEST_ASSERT_EQUAL(ButtonEvent::None, feed(d, now, 60, 100));
}

static void test_a_hold_still_works_after_two_clicks() {
  HoldDetector d;
  uint32_t now = 0;
  feed(d, now, 60, 100);
  feed(d, now, 60, 100);
  ButtonEvent seen = ButtonEvent::None;
  for (uint32_t i = 0; i < 3100; ++i) {
    const ButtonEvent e = d.update(true, now++);
    if (e != ButtonEvent::None) {
      seen = e;
    }
  }
  TEST_ASSERT_EQUAL(ButtonEvent::Hold, seen);
}

static void test_bouncing_contacts_do_not_manufacture_a_triple_click() {
  // The failure this guards against: a worn switch chattering three times
  // inside the window and switching the screen on its own.
  HoldDetector d;
  uint32_t now = 0;
  d.update(true, now++);
  for (int bounce = 0; bounce < 6; ++bounce) {
    for (uint32_t i = 0; i < HoldDetector::kDebounceMs - 5; ++i) {
      TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(false, now++));
    }
    for (uint32_t i = 0; i < 5; ++i) {
      TEST_ASSERT_EQUAL(ButtonEvent::None, d.update(true, now++));
    }
  }
}

static void test_a_click_survives_a_loop_that_misses_the_release() {
  // loop() does not sample every millisecond. One display redraw blocks the
  // I2C bus for about 25 ms, and a radio restart far longer, so a release can
  // go entirely unobserved: the next sample sees the button already pressed
  // again. The click still has to count, or three deliberate clicks register
  // two and the gesture silently does nothing.
  HoldDetector d;
  d.update(true, 0);
  d.update(false, 60);   // release begins
  d.update(true, 200);   // no sample in between -- already pressed again

  d.update(false, 260);
  d.update(false, 400);  // this release is observed normally

  d.update(true, 500);
  d.update(false, 560);
  TEST_ASSERT_EQUAL(ButtonEvent::TripleClick, d.update(false, 700));
}

static void test_held_ms_reports_progress_and_zero_when_idle() {
  HoldDetector d;
  TEST_ASSERT_EQUAL_UINT32(0, d.heldMs(0));
  d.update(true, 1000);
  TEST_ASSERT_EQUAL_UINT32(500, d.heldMs(1500));
  d.update(false, 2000);
  TEST_ASSERT_EQUAL_UINT32(0, d.heldMs(2500));
}

static void test_held_ms_keeps_counting_past_the_hold_threshold() {
  // The header reads this to decide what to show, and it must not wrap or
  // saturate at kHoldMs.
  HoldDetector d;
  d.update(true, 0);
  d.update(true, 3000);
  TEST_ASSERT_EQUAL_UINT32(9000, d.heldMs(9000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_idle_never_fires);
  RUN_TEST(test_hold_fires_once_at_the_threshold_while_still_down);
  RUN_TEST(test_release_before_the_threshold_fires_no_hold);
  RUN_TEST(test_release_then_hold_again_fires_again);
  RUN_TEST(test_bounce_shorter_than_debounce_does_not_restart_the_hold);
  RUN_TEST(test_three_clicks_fire_triple_click);
  RUN_TEST(test_two_clicks_alone_fire_nothing);
  RUN_TEST(test_a_fourth_click_does_not_fire_again);
  RUN_TEST(test_clicks_spread_past_the_window_do_not_accumulate);
  RUN_TEST(test_a_long_press_is_not_a_click);
  RUN_TEST(test_a_hold_still_works_after_two_clicks);
  RUN_TEST(test_bouncing_contacts_do_not_manufacture_a_triple_click);
  RUN_TEST(test_a_click_survives_a_loop_that_misses_the_release);
  RUN_TEST(test_held_ms_reports_progress_and_zero_when_idle);
  RUN_TEST(test_held_ms_keeps_counting_past_the_hold_threshold);
  return UNITY_END();
}
