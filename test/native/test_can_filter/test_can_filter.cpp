#include <string.h>
#include <unity.h>

#include "can/CanFilter.h"
#include "can/CanFrame.h"

void setUp() {}
void tearDown() {}

namespace {

// RaceChrono writes these to characteristic 0x0002. Big-endian inside.
void denyAll(CanFilter& f) {
  const uint8_t cmd[1] = {0};
  TEST_ASSERT_TRUE(f.applyCommand(cmd, sizeof(cmd)));
}

void allowAll(CanFilter& f, uint16_t intervalMs) {
  const uint8_t cmd[3] = {1, static_cast<uint8_t>(intervalMs >> 8),
                          static_cast<uint8_t>(intervalMs)};
  TEST_ASSERT_TRUE(f.applyCommand(cmd, sizeof(cmd)));
}

void allowOne(CanFilter& f, uint32_t id, uint16_t intervalMs) {
  const uint8_t cmd[7] = {2,
                          static_cast<uint8_t>(intervalMs >> 8),
                          static_cast<uint8_t>(intervalMs),
                          static_cast<uint8_t>(id >> 24),
                          static_cast<uint8_t>(id >> 16),
                          static_cast<uint8_t>(id >> 8),
                          static_cast<uint8_t>(id)};
  TEST_ASSERT_TRUE(f.applyCommand(cmd, sizeof(cmd)));
}

}  // namespace

static void test_nothing_is_allowed_before_a_command_arrives() {
  CanFilter f;
  TEST_ASSERT_FALSE(f.shouldNotify(0x202, 1000));
}

static void test_deny_all_forgets_ids_previously_allowed() {
  CanFilter f;
  allowOne(f, 0x202, 0);
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1000));
  denyAll(f);
  TEST_ASSERT_FALSE(f.shouldNotify(0x202, 2000));
}

static void test_an_allowed_id_passes_and_an_unlisted_one_does_not() {
  CanFilter f;
  denyAll(f);
  allowOne(f, 0x202, 0);
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1000));
  TEST_ASSERT_FALSE(f.shouldNotify(0x78, 1000));
}

static void test_the_first_frame_for_an_id_is_never_delayed() {
  // Waiting one interval before the first frame would make a 1 Hz channel
  // take a second to appear, for no reason.
  CanFilter f;
  denyAll(f);
  allowOne(f, 0x202, 500);
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 10000));
}

static void test_an_interval_throttles_that_id() {
  CanFilter f;
  denyAll(f);
  allowOne(f, 0x202, 100);
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1000));
  TEST_ASSERT_FALSE(f.shouldNotify(0x202, 1099));
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1100));
}

static void test_zero_interval_means_no_throttling() {
  CanFilter f;
  denyAll(f);
  allowOne(f, 0x202, 0);
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1000));
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1000));
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1001));
}

static void test_ids_are_throttled_independently() {
  // The whole reason the interval is per id: a 10 ms channel must not consume
  // the budget of a 500 ms one, and a busy id must not starve a quiet one.
  CanFilter f;
  denyAll(f);
  allowOne(f, 0x202, 10);
  allowOne(f, 0x86, 500);

  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1000));
  TEST_ASSERT_TRUE(f.shouldNotify(0x86, 1000));

  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1010));   // fast one is due again
  TEST_ASSERT_FALSE(f.shouldNotify(0x86, 1010));   // slow one is not
  TEST_ASSERT_TRUE(f.shouldNotify(0x86, 1500));
}

static void test_allow_all_passes_an_id_nobody_asked_for() {
  CanFilter f;
  allowAll(f, 100);
  TEST_ASSERT_TRUE(f.shouldNotify(0x3FF, 1000));
  TEST_ASSERT_FALSE(f.shouldNotify(0x3FF, 1050));
  TEST_ASSERT_TRUE(f.shouldNotify(0x3FF, 1100));
}

static void test_allow_all_replaces_an_earlier_filter() {
  CanFilter f;
  denyAll(f);
  allowOne(f, 0x202, 0);
  allowAll(f, 50);
  TEST_ASSERT_TRUE(f.allowAll());
  TEST_ASSERT_EQUAL_UINT32(0, f.trackedIds());
}

static void test_a_later_allow_one_updates_the_interval() {
  CanFilter f;
  denyAll(f);
  allowOne(f, 0x202, 1000);
  allowOne(f, 0x202, 10);
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 5000));
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 5010));
  TEST_ASSERT_EQUAL_UINT32(1, f.trackedIds());  // not a second entry
}

static void test_a_full_table_keeps_what_it_has_and_counts_the_rest() {
  CanFilter f;
  allowAll(f, 0);
  for (uint32_t id = 1; id <= CanFilter::kMaxIds; ++id) {
    TEST_ASSERT_TRUE(f.shouldNotify(id, 1000));
  }
  TEST_ASSERT_EQUAL_UINT32(CanFilter::kMaxIds, f.trackedIds());

  // One too many: refused, counted, and the ids already held still work.
  TEST_ASSERT_FALSE(f.shouldNotify(0xBEEF, 1000));
  TEST_ASSERT_EQUAL_UINT32(1, f.droppedUnknown());
  TEST_ASSERT_TRUE(f.shouldNotify(1, 2000));
}

static void test_malformed_commands_are_rejected_not_guessed_at() {
  CanFilter f;
  const uint8_t denyWithExtra[2] = {0, 0};
  const uint8_t allowAllShort[2] = {1, 0};
  const uint8_t allowOneShort[6] = {2, 0, 100, 0, 0, 2};
  const uint8_t unknownCmd[1] = {9};
  TEST_ASSERT_FALSE(f.applyCommand(denyWithExtra, sizeof(denyWithExtra)));
  TEST_ASSERT_FALSE(f.applyCommand(allowAllShort, sizeof(allowAllShort)));
  TEST_ASSERT_FALSE(f.applyCommand(allowOneShort, sizeof(allowOneShort)));
  TEST_ASSERT_FALSE(f.applyCommand(unknownCmd, sizeof(unknownCmd)));
  TEST_ASSERT_FALSE(f.applyCommand(nullptr, 0));
}

static void test_the_command_fields_are_big_endian() {
  // Byte order is the easiest thing to get wrong here, and getting it wrong
  // yields an id that looks plausible and matches nothing on the bus.
  CanFilter f;
  denyAll(f);
  const uint8_t cmd[7] = {2, 0x01, 0x2C, 0x00, 0x00, 0x02, 0x02};  // 300ms, 0x202
  TEST_ASSERT_TRUE(f.applyCommand(cmd, sizeof(cmd)));
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1000));
  TEST_ASSERT_FALSE(f.shouldNotify(0x202, 1299));
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 1300));
}

static void test_a_millis_wrap_does_not_stall_an_id() {
  CanFilter f;
  denyAll(f);
  allowOne(f, 0x202, 100);
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 0xFFFFFFF0u));
  // Unsigned subtraction carries across the wrap: 0x54 - 0xFFFFFFF0 == 0x64,
  // i.e. exactly the 100ms interval, which is the same >= boundary every
  // other throttle test in this file uses.
  TEST_ASSERT_TRUE(f.shouldNotify(0x202, 0x00000054u));
}

static void test_the_can_id_is_encoded_little_endian() {
  // The one field in this protocol that is NOT big-endian. RaceChrono's own
  // spec calls it out, and every GPS field beside it goes the other way.
  CanFrame frame;
  frame.id = 0x00000202;
  frame.dlc = 8;
  for (uint8_t i = 0; i < 8; ++i) {
    frame.data[i] = static_cast<uint8_t>(0xA0 + i);
  }

  uint8_t out[CanFrame::kMaxPacketBytes];
  const size_t len = encodeCanPacket(frame, out);

  TEST_ASSERT_EQUAL_UINT32(12, len);
  TEST_ASSERT_EQUAL_UINT8(0x02, out[0]);
  TEST_ASSERT_EQUAL_UINT8(0x02, out[1]);
  TEST_ASSERT_EQUAL_UINT8(0x00, out[2]);
  TEST_ASSERT_EQUAL_UINT8(0x00, out[3]);
  TEST_ASSERT_EQUAL_UINT8(0xA0, out[4]);
  TEST_ASSERT_EQUAL_UINT8(0xA7, out[11]);
}

static void test_a_short_frame_is_not_padded() {
  CanFrame frame;
  frame.id = 0x78;
  frame.dlc = 3;
  frame.data[0] = 0x11;
  frame.data[1] = 0x22;
  frame.data[2] = 0x33;

  uint8_t out[CanFrame::kMaxPacketBytes];
  TEST_ASSERT_EQUAL_UINT32(7, encodeCanPacket(frame, out));
  TEST_ASSERT_EQUAL_UINT8(0x78, out[0]);
  TEST_ASSERT_EQUAL_UINT8(0x33, out[6]);
}

static void test_an_extended_id_survives_the_round_trip() {
  CanFrame frame;
  frame.id = 0x18DAF110;  // 29-bit
  frame.dlc = 1;
  frame.data[0] = 0x5A;

  uint8_t out[CanFrame::kMaxPacketBytes];
  TEST_ASSERT_EQUAL_UINT32(5, encodeCanPacket(frame, out));
  const uint32_t decoded = static_cast<uint32_t>(out[0]) |
                           (static_cast<uint32_t>(out[1]) << 8) |
                           (static_cast<uint32_t>(out[2]) << 16) |
                           (static_cast<uint32_t>(out[3]) << 24);
  TEST_ASSERT_EQUAL_UINT32(0x18DAF110u, decoded);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_is_allowed_before_a_command_arrives);
  RUN_TEST(test_deny_all_forgets_ids_previously_allowed);
  RUN_TEST(test_an_allowed_id_passes_and_an_unlisted_one_does_not);
  RUN_TEST(test_the_first_frame_for_an_id_is_never_delayed);
  RUN_TEST(test_an_interval_throttles_that_id);
  RUN_TEST(test_zero_interval_means_no_throttling);
  RUN_TEST(test_ids_are_throttled_independently);
  RUN_TEST(test_allow_all_passes_an_id_nobody_asked_for);
  RUN_TEST(test_allow_all_replaces_an_earlier_filter);
  RUN_TEST(test_a_later_allow_one_updates_the_interval);
  RUN_TEST(test_a_full_table_keeps_what_it_has_and_counts_the_rest);
  RUN_TEST(test_malformed_commands_are_rejected_not_guessed_at);
  RUN_TEST(test_the_command_fields_are_big_endian);
  RUN_TEST(test_a_millis_wrap_does_not_stall_an_id);
  RUN_TEST(test_the_can_id_is_encoded_little_endian);
  RUN_TEST(test_a_short_frame_is_not_padded);
  RUN_TEST(test_an_extended_id_survives_the_round_trip);
  return UNITY_END();
}
