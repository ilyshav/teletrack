#include <unity.h>

#include "can/CanStats.h"

void setUp() {}
void tearDown() {}

static void test_a_fresh_window_reports_nothing() {
  CanStats s;
  TEST_ASSERT_EQUAL_UINT32(0, s.snapshot().framesPerSec);
  TEST_ASSERT_EQUAL_UINT32(0, s.snapshot().lostPerSec);
}

static void test_frames_are_not_reported_until_the_window_rolls() {
  // A partial window must not be published, or the rate reads low for the
  // first second after every switch to the screen.
  CanStats s;
  for (int i = 0; i < 500; ++i) {
    s.recordFrame(0x202);
  }
  TEST_ASSERT_FALSE(s.tick(999));
  TEST_ASSERT_EQUAL_UINT32(0, s.snapshot().framesPerSec);
}

static void test_the_window_publishes_its_count_once_a_second() {
  CanStats s;
  for (int i = 0; i < 1847; ++i) {
    s.recordFrame(0x202);
  }
  TEST_ASSERT_TRUE(s.tick(1000));
  TEST_ASSERT_EQUAL_UINT32(1847, s.snapshot().framesPerSec);
}

static void test_a_new_window_replaces_the_old_count_rather_than_adding() {
  CanStats s;
  for (int i = 0; i < 1800; ++i) {
    s.recordFrame(0x202);
  }
  TEST_ASSERT_TRUE(s.tick(1000));
  for (int i = 0; i < 12; ++i) {
    s.recordFrame(0x202);
  }
  TEST_ASSERT_TRUE(s.tick(2000));
  TEST_ASSERT_EQUAL_UINT32(12, s.snapshot().framesPerSec);
}

static void test_a_quiet_window_reports_zero_rather_than_the_last_value() {
  // A bus that goes silent must read silent, or BusStatus never says so.
  CanStats s;
  s.recordFrame(0x202);
  TEST_ASSERT_TRUE(s.tick(1000));
  TEST_ASSERT_TRUE(s.tick(2000));
  TEST_ASSERT_EQUAL_UINT32(0, s.snapshot().framesPerSec);
}

static void test_lost_frames_are_counted_per_window_too() {
  CanStats s;
  s.recordLost(3);
  s.recordLost(9);
  TEST_ASSERT_TRUE(s.tick(1000));
  TEST_ASSERT_EQUAL_UINT32(12, s.snapshot().lostPerSec);
  TEST_ASSERT_TRUE(s.tick(2000));
  TEST_ASSERT_EQUAL_UINT32(0, s.snapshot().lostPerSec);
}

static void test_a_stalled_loop_is_normalised_to_a_true_rate() {
  // loop() can be held up -- a full OLED repaint is ~25 ms and the window is
  // only rolled when tick() is called. Reporting the raw count of a two-second
  // window as a per-second rate would double it.
  CanStats s;
  for (int i = 0; i < 2000; ++i) {
    s.recordFrame(0x202);
  }
  TEST_ASSERT_TRUE(s.tick(2000));
  TEST_ASSERT_EQUAL_UINT32(1000, s.snapshot().framesPerSec);
}

static void test_the_window_survives_the_millis_rollover() {
  // nowMs wraps every 49 days. Unsigned subtraction handles it; an operand
  // order that compares absolute values would stall the window for 49 days.
  CanStats s;
  const uint32_t nearEnd = 0xFFFFFF00u;
  s.tick(nearEnd);  // seats the window just short of the wrap
  s.recordFrame(0x202);
  // This nowMs has wrapped past zero and is numerically far SMALLER than the
  // window start it is being compared against.
  TEST_ASSERT_TRUE(s.tick(nearEnd + 1000u));
  TEST_ASSERT_EQUAL_UINT32(1, s.snapshot().framesPerSec);
}

static void test_no_ids_have_been_seen_on_a_fresh_bus() {
  CanStats s;
  TEST_ASSERT_TRUE(s.tick(1000));
  TEST_ASSERT_EQUAL_UINT32(0, s.snapshot().idsSeen);
}

static void test_the_same_id_twice_is_one_id() {
  CanStats s;
  s.recordFrame(0x202);
  s.recordFrame(0x202);
  s.recordFrame(0x202);
  TEST_ASSERT_TRUE(s.tick(1000));
  TEST_ASSERT_EQUAL_UINT32(1, s.snapshot().idsSeen);
  TEST_ASSERT_EQUAL_UINT32(3, s.snapshot().framesPerSec);
}

static void test_distinct_ids_are_counted_separately() {
  CanStats s;
  s.recordFrame(0x202);
  s.recordFrame(0x078);
  s.recordFrame(0x086);
  TEST_ASSERT_TRUE(s.tick(1000));
  TEST_ASSERT_EQUAL_UINT32(3, s.snapshot().idsSeen);
}

static void test_ids_accumulate_across_windows_unlike_rates() {
  // Discovery is cumulative: the count answers "what is on this bus", which
  // does not stop being true when the window rolls.
  CanStats s;
  s.recordFrame(0x202);
  TEST_ASSERT_TRUE(s.tick(1000));
  TEST_ASSERT_EQUAL_UINT32(1, s.snapshot().idsSeen);
  s.recordFrame(0x078);
  TEST_ASSERT_TRUE(s.tick(2000));
  TEST_ASSERT_EQUAL_UINT32(2, s.snapshot().idsSeen);
  TEST_ASSERT_EQUAL_UINT32(1, s.snapshot().framesPerSec);
}

static void test_both_ends_of_the_11_bit_range_are_tracked() {
  // Id 0 is a real CAN id, so a set that treats zero as "unseen" loses it, and
  // 0x7FF is the last index of the bitset -- an off-by-one drops it silently.
  CanStats s;
  s.recordFrame(0x000);
  s.recordFrame(0x7FF);
  TEST_ASSERT_TRUE(s.tick(1000));
  TEST_ASSERT_EQUAL_UINT32(2, s.snapshot().idsSeen);
}

static void test_extended_ids_are_counted_apart_from_the_11_bit_set() {
  // A 29-bit id cannot index the bitset. It still gets counted as a frame, and
  // separately flagged, so "ids seen" never silently disagrees with the bus.
  CanStats s;
  s.recordFrame(0x202);
  s.recordFrame(0x18DAF110);
  TEST_ASSERT_TRUE(s.tick(1000));
  TEST_ASSERT_EQUAL_UINT32(1, s.snapshot().idsSeen);
  TEST_ASSERT_EQUAL_UINT32(1, s.snapshot().extendedFrames);
  TEST_ASSERT_EQUAL_UINT32(2, s.snapshot().framesPerSec);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_fresh_window_reports_nothing);
  RUN_TEST(test_frames_are_not_reported_until_the_window_rolls);
  RUN_TEST(test_the_window_publishes_its_count_once_a_second);
  RUN_TEST(test_a_new_window_replaces_the_old_count_rather_than_adding);
  RUN_TEST(test_a_quiet_window_reports_zero_rather_than_the_last_value);
  RUN_TEST(test_lost_frames_are_counted_per_window_too);
  RUN_TEST(test_a_stalled_loop_is_normalised_to_a_true_rate);
  RUN_TEST(test_the_window_survives_the_millis_rollover);
  RUN_TEST(test_no_ids_have_been_seen_on_a_fresh_bus);
  RUN_TEST(test_the_same_id_twice_is_one_id);
  RUN_TEST(test_distinct_ids_are_counted_separately);
  RUN_TEST(test_ids_accumulate_across_windows_unlike_rates);
  RUN_TEST(test_both_ends_of_the_11_bit_range_are_tracked);
  RUN_TEST(test_extended_ids_are_counted_apart_from_the_11_bit_set);
  return UNITY_END();
}
