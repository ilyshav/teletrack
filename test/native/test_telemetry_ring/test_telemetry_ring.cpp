#include <unity.h>
#include <string.h>

#include "ble/TelemetryRing.h"

void setUp() {}
void tearDown() {}

static void fillPayload(uint8_t* p, uint8_t value) {
  memset(p, value, TelemetrySample::kPayloadBytes);
}

static uint32_t seqAt(const uint8_t* buf, size_t index) {
  uint32_t seq = 0;
  memcpy(&seq, buf + index * TelemetrySample::kSize, sizeof(seq));
  return seq;
}

static void test_starts_empty() {
  TelemetryRing ring;
  TEST_ASSERT_TRUE(ring.empty());
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)ring.pending());
  TEST_ASSERT_EQUAL_UINT32(0u, ring.dropped());
  TEST_ASSERT_EQUAL_UINT32(0u, ring.produced());
}

static void test_drain_of_empty_ring_writes_nothing() {
  TelemetryRing ring;
  uint8_t out[256];
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)ring.drain(out, sizeof(out)));
}

static void test_push_then_drain_round_trips() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes];
  fillPayload(payload, 0xAB);
  ring.push(1234, payload);

  TEST_ASSERT_EQUAL_UINT(1u, (unsigned)ring.pending());

  uint8_t out[TelemetrySample::kSize];
  const size_t n = ring.drain(out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT((unsigned)TelemetrySample::kSize, (unsigned)n);
  TEST_ASSERT_EQUAL_UINT32(0u, seqAt(out, 0));

  uint32_t uptime = 0;
  memcpy(&uptime, out + sizeof(uint32_t), sizeof(uptime));
  TEST_ASSERT_EQUAL_UINT32(1234u, uptime);
  TEST_ASSERT_EQUAL_UINT8(0xAB, out[2 * sizeof(uint32_t)]);
  TEST_ASSERT_TRUE(ring.empty());
}

static void test_sequence_numbers_are_monotonic() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  for (int i = 0; i < 5; ++i) {
    ring.push((uint32_t)i, payload);
  }
  uint8_t out[5 * TelemetrySample::kSize];
  ring.drain(out, sizeof(out));
  for (uint32_t i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL_UINT32(i, seqAt(out, i));
  }
}

static void test_drain_copies_whole_samples_only() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  for (int i = 0; i < 10; ++i) {
    ring.push((uint32_t)i, payload);
  }
  // Room for 3 samples and a bit: must return exactly 3, never a partial one.
  uint8_t out[3 * TelemetrySample::kSize + 7];
  const size_t n = ring.drain(out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT((unsigned)(3 * TelemetrySample::kSize), (unsigned)n);
  TEST_ASSERT_EQUAL_UINT(7u, (unsigned)ring.pending());
}

static void test_drain_smaller_than_one_sample_returns_nothing() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  ring.push(0, payload);
  uint8_t out[TelemetrySample::kSize - 1];
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)ring.drain(out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT(1u, (unsigned)ring.pending());  // still there
}

static void test_overflow_drops_oldest_and_counts_it() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  const size_t over = 10;
  for (size_t i = 0; i < TelemetryRing::kCapacity + over; ++i) {
    ring.push((uint32_t)i, payload);
  }
  TEST_ASSERT_EQUAL_UINT32((uint32_t)over, ring.dropped());
  TEST_ASSERT_EQUAL_UINT((unsigned)TelemetryRing::kCapacity, (unsigned)ring.pending());
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(TelemetryRing::kCapacity + over), ring.produced());
}

static void test_sequence_gap_after_overflow_equals_drop_count() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  const size_t over = 10;
  for (size_t i = 0; i < TelemetryRing::kCapacity + over; ++i) {
    ring.push((uint32_t)i, payload);
  }
  // The oldest surviving sample must be seq == over: that is what lets the
  // receiver measure loss from sequence gaps alone.
  uint8_t out[TelemetrySample::kSize];
  ring.drain(out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)over, seqAt(out, 0));
}

static void test_drain_then_push_reuses_space_without_dropping() {
  TelemetryRing ring;
  uint8_t payload[TelemetrySample::kPayloadBytes] = {};
  for (size_t i = 0; i < TelemetryRing::kCapacity; ++i) {
    ring.push((uint32_t)i, payload);
  }
  uint8_t out[64 * TelemetrySample::kSize];
  ring.drain(out, sizeof(out));  // free 64 slots
  for (size_t i = 0; i < 64; ++i) {
    ring.push(0, payload);
  }
  TEST_ASSERT_EQUAL_UINT32(0u, ring.dropped());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_empty);
  RUN_TEST(test_drain_of_empty_ring_writes_nothing);
  RUN_TEST(test_push_then_drain_round_trips);
  RUN_TEST(test_sequence_numbers_are_monotonic);
  RUN_TEST(test_drain_copies_whole_samples_only);
  RUN_TEST(test_drain_smaller_than_one_sample_returns_nothing);
  RUN_TEST(test_overflow_drops_oldest_and_counts_it);
  RUN_TEST(test_sequence_gap_after_overflow_equals_drop_count);
  RUN_TEST(test_drain_then_push_reuses_space_without_dropping);
  return UNITY_END();
}
