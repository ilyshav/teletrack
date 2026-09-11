#include <unity.h>

#include "can/BusStatus.h"

void setUp() {}
void tearDown() {}

static void test_a_controller_that_never_started_is_starting() {
  // Nothing said about the bus can mean anything while the driver is down,
  // so a frame count is not allowed to override this.
  TEST_ASSERT_EQUAL(BusStatus::Starting, busStatus(false, 0, 0));
  TEST_ASSERT_EQUAL(BusStatus::Starting, busStatus(false, 1800, 0));
}

static void test_a_silent_bus_is_silent() {
  // This is the state the screen exists to rule out: it is what says to swap
  // CAN RX and TX before suspecting anything subtler.
  TEST_ASSERT_EQUAL(BusStatus::Silent, busStatus(true, 0, 0));
}

static void test_frames_with_nothing_lost_are_healthy() {
  TEST_ASSERT_EQUAL(BusStatus::Healthy, busStatus(true, 1, 0));
  TEST_ASSERT_EQUAL(BusStatus::Healthy, busStatus(true, 1847, 0));
}

static void test_losing_frames_is_lossy_not_healthy() {
  TEST_ASSERT_EQUAL(BusStatus::Lossy, busStatus(true, 1847, 3));
}

static void test_a_window_of_nothing_but_losses_is_not_silent() {
  // Frames were on the wire and the controller could not hand any over. The
  // wiring is fine, which is the opposite of what Silent would claim.
  TEST_ASSERT_EQUAL(BusStatus::Lossy, busStatus(true, 0, 200));
}

static void test_recovery_returns_to_healthy() {
  TEST_ASSERT_EQUAL(BusStatus::Lossy, busStatus(true, 1800, 40));
  TEST_ASSERT_EQUAL(BusStatus::Healthy, busStatus(true, 1800, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_controller_that_never_started_is_starting);
  RUN_TEST(test_a_silent_bus_is_silent);
  RUN_TEST(test_frames_with_nothing_lost_are_healthy);
  RUN_TEST(test_losing_frames_is_lossy_not_healthy);
  RUN_TEST(test_a_window_of_nothing_but_losses_is_not_silent);
  RUN_TEST(test_recovery_returns_to_healthy);
  return UNITY_END();
}
