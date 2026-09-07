#include <unity.h>
#include <string.h>

#include "ble/TelemetryRing.h"

void setUp() {}
void tearDown() {}

// Test-only packet shape: first 4 bytes carry a marker so tests can identify
// which packet came out the other end without knowing anything about
// RaceChrono's real packet layout (TelemetryRing treats packets as opaque).
static void fillPacket(uint8_t* p, uint32_t marker) {
  memcpy(p, &marker, sizeof(marker));
  memset(p + sizeof(marker), 0xAB, TelemetrySample::kSize - sizeof(marker));
}

static uint32_t markerAt(const uint8_t* buf, size_t index) {
  uint32_t marker = 0;
  memcpy(&marker, buf + index * TelemetrySample::kSize, sizeof(marker));
  return marker;
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

static void test_push_then_drain_round_trips_exact_bytes() {
  TelemetryRing ring;
  uint8_t packet[TelemetrySample::kSize];
  fillPacket(packet, 0xAABBCCDDu);
  ring.push(packet);

  TEST_ASSERT_EQUAL_UINT(1u, (unsigned)ring.pending());

  uint8_t out[TelemetrySample::kSize];
  const size_t n = ring.drain(out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT((unsigned)TelemetrySample::kSize, (unsigned)n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(packet, out, TelemetrySample::kSize);
  TEST_ASSERT_TRUE(ring.empty());
}

static void test_packets_drain_in_fifo_order() {
  TelemetryRing ring;
  uint8_t packet[TelemetrySample::kSize];
  for (uint32_t i = 0; i < 5; ++i) {
    fillPacket(packet, i);
    ring.push(packet);
  }
  uint8_t out[5 * TelemetrySample::kSize];
  ring.drain(out, sizeof(out));
  for (uint32_t i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL_UINT32(i, markerAt(out, i));
  }
}

static void test_drain_copies_whole_samples_only() {
  TelemetryRing ring;
  uint8_t packet[TelemetrySample::kSize];
  for (uint32_t i = 0; i < 10; ++i) {
    fillPacket(packet, i);
    ring.push(packet);
  }
  // Room for 3 samples and a bit: must return exactly 3, never a partial one.
  uint8_t out[3 * TelemetrySample::kSize + 7];
  const size_t n = ring.drain(out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT((unsigned)(3 * TelemetrySample::kSize), (unsigned)n);
  TEST_ASSERT_EQUAL_UINT(7u, (unsigned)ring.pending());
}

static void test_drain_smaller_than_one_sample_returns_nothing() {
  TelemetryRing ring;
  uint8_t packet[TelemetrySample::kSize] = {};
  ring.push(packet);
  uint8_t out[TelemetrySample::kSize - 1];
  TEST_ASSERT_EQUAL_UINT(0u, (unsigned)ring.drain(out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT(1u, (unsigned)ring.pending());  // still there
}

static void test_overflow_drops_oldest_and_counts_it() {
  TelemetryRing ring;
  uint8_t packet[TelemetrySample::kSize];
  const size_t over = 10;
  for (uint32_t i = 0; i < TelemetryRing::kCapacity + over; ++i) {
    fillPacket(packet, i);
    ring.push(packet);
  }
  TEST_ASSERT_EQUAL_UINT32((uint32_t)over, ring.dropped());
  TEST_ASSERT_EQUAL_UINT((unsigned)TelemetryRing::kCapacity, (unsigned)ring.pending());
  TEST_ASSERT_EQUAL_UINT32((uint32_t)(TelemetryRing::kCapacity + over), ring.produced());
}

static void test_oldest_surviving_packet_after_overflow_is_the_first_kept_one() {
  TelemetryRing ring;
  uint8_t packet[TelemetrySample::kSize];
  const size_t over = 10;
  for (uint32_t i = 0; i < TelemetryRing::kCapacity + over; ++i) {
    fillPacket(packet, i);
    ring.push(packet);
  }
  // The first `over` pushes were overwritten, so the oldest survivor is the
  // one pushed with marker == over.
  uint8_t out[TelemetrySample::kSize];
  ring.drain(out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)over, markerAt(out, 0));
}

static void test_drain_then_push_reuses_space_without_dropping() {
  TelemetryRing ring;
  uint8_t packet[TelemetrySample::kSize] = {};
  for (size_t i = 0; i < TelemetryRing::kCapacity; ++i) {
    ring.push(packet);
  }
  uint8_t out[64 * TelemetrySample::kSize];
  ring.drain(out, sizeof(out));  // free 64 slots
  for (size_t i = 0; i < 64; ++i) {
    ring.push(packet);
  }
  TEST_ASSERT_EQUAL_UINT32(0u, ring.dropped());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_empty);
  RUN_TEST(test_drain_of_empty_ring_writes_nothing);
  RUN_TEST(test_push_then_drain_round_trips_exact_bytes);
  RUN_TEST(test_packets_drain_in_fifo_order);
  RUN_TEST(test_drain_copies_whole_samples_only);
  RUN_TEST(test_drain_smaller_than_one_sample_returns_nothing);
  RUN_TEST(test_overflow_drops_oldest_and_counts_it);
  RUN_TEST(test_oldest_surviving_packet_after_overflow_is_the_first_kept_one);
  RUN_TEST(test_drain_then_push_reuses_space_without_dropping);
  return UNITY_END();
}
