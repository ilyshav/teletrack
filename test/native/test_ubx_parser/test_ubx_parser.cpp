#include <string.h>
#include <unity.h>

#include "gps/UbxParser.h"

void setUp() {}
void tearDown() {}

namespace {

void putU2(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU4(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

void putI4(uint8_t* p, int32_t v) { putU4(p, static_cast<uint32_t>(v)); }

// The NAV-PVT fields the parser reads, with a plausible default for each.
struct PvtFields {
  uint16_t year = 2026;
  uint8_t month = 9;
  uint8_t day = 7;
  uint8_t hour = 14;
  uint8_t min = 23;
  uint8_t sec = 45;
  uint8_t valid = 0x03;        // validDate | validTime
  int32_t nano = 500000000;    // 500 ms
  uint8_t fixType = 3;         // 3D
  uint8_t flags = 0x01;        // gnssFixOK
  uint8_t numSV = 9;
  int32_t lon = 48952100;      // 4.89521 deg
  int32_t lat = 523713400;     // 52.37134 deg
  int32_t hMSL = 12000;        // mm -> 12 m
  int32_t gSpeed = 13389;      // mm/s -> 48.2004 km/h
  int32_t headMot = 12345000;  // 1e-5 deg -> 123.45
  uint16_t pDOP = 90;          // 0.9
};

void buildPvt(uint8_t out[92], const PvtFields& f) {
  memset(out, 0, 92);
  putU4(out + 0, 123456);  // iTOW, never read
  putU2(out + 4, f.year);
  out[6] = f.month;
  out[7] = f.day;
  out[8] = f.hour;
  out[9] = f.min;
  out[10] = f.sec;
  out[11] = f.valid;
  putI4(out + 16, f.nano);
  out[20] = f.fixType;
  out[21] = f.flags;
  out[23] = f.numSV;
  putI4(out + 24, f.lon);
  putI4(out + 28, f.lat);
  putI4(out + 36, f.hMSL);
  putI4(out + 60, f.gSpeed);
  putI4(out + 64, f.headMot);
  putU2(out + 76, f.pDOP);
}

struct Frame {
  uint8_t bytes[256] = {};
  size_t len = 0;
};

// Wraps a payload in sync bytes, header and a correct Fletcher checksum.
Frame makeFrame(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t plen) {
  Frame f;
  f.bytes[0] = 0xB5;
  f.bytes[1] = 0x62;
  f.bytes[2] = cls;
  f.bytes[3] = id;
  f.bytes[4] = static_cast<uint8_t>(plen);
  f.bytes[5] = static_cast<uint8_t>(plen >> 8);
  for (uint16_t i = 0; i < plen; ++i) {
    f.bytes[6 + i] = payload[i];
  }
  uint8_t a = 0;
  uint8_t b = 0;
  for (size_t i = 2; i < 6u + plen; ++i) {
    a += f.bytes[i];
    b += a;
  }
  f.bytes[6 + plen] = a;
  f.bytes[7 + plen] = b;
  f.len = 8u + plen;
  return f;
}

Frame makePvtFrame(const PvtFields& f = PvtFields{}) {
  uint8_t payload[92];
  buildPvt(payload, f);
  return makeFrame(UbxParser::kClassNav, UbxParser::kIdPvt, payload, 92);
}

// Feeds every byte, returning how many frames the parser accepted.
int feedAll(UbxParser& p, const uint8_t* bytes, size_t len) {
  int accepted = 0;
  for (size_t i = 0; i < len; ++i) {
    if (p.feed(bytes[i])) {
      ++accepted;
    }
  }
  return accepted;
}

}  // namespace

static void test_a_valid_frame_is_accepted_on_its_last_byte() {
  UbxParser parser;
  const Frame f = makePvtFrame();
  for (size_t i = 0; i + 1 < f.len; ++i) {
    TEST_ASSERT_FALSE(parser.feed(f.bytes[i]));
  }
  TEST_ASSERT_TRUE(parser.feed(f.bytes[f.len - 1]));
  TEST_ASSERT_EQUAL_UINT32(0, parser.checksumErrors());
}

static void test_a_flipped_payload_byte_is_rejected() {
  UbxParser parser;
  Frame f = makePvtFrame();
  f.bytes[30] ^= 0xFF;  // inside the payload, so the checksum no longer matches
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT32(1, parser.checksumErrors());
}

static void test_leading_garbage_is_skipped() {
  UbxParser parser;
  const uint8_t junk[] = {0x00, 0xFF, 0x24, 0x47, 0xB5, 0x01, 0x7E};
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, junk, sizeof(junk)));

  const Frame f = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
}

static void test_a_truncated_frame_does_not_block_the_next_one() {
  UbxParser parser;
  const Frame truncated = makePvtFrame();
  // Half a frame, then a whole one. The parser must recover.
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, truncated.bytes, 40));

  const Frame good = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, good.bytes, good.len));
}

static void test_doubled_sync_byte_still_syncs() {
  UbxParser parser;
  // 0xB5 0xB5 0x62 ... -- the second 0xB5 must restart the sync, not consume
  // the 0x62 that follows.
  const Frame f = makePvtFrame();
  uint8_t stream[300];
  stream[0] = 0xB5;
  memcpy(stream + 1, f.bytes, f.len);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, stream, f.len + 1));
}

static void test_another_ubx_message_is_consumed_but_not_reported() {
  UbxParser parser;
  const uint8_t ackPayload[2] = {0x06, 0x8A};
  const Frame ack = makeFrame(0x05, 0x01, ackPayload, 2);  // UBX-ACK-ACK
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, ack.bytes, ack.len));

  const Frame pvt = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, pvt.bytes, pvt.len));
}

static void test_a_message_longer_than_navpvt_does_not_overflow() {
  UbxParser parser;
  uint8_t big[200];
  memset(big, 0xAB, sizeof(big));
  const Frame f = makeFrame(0x01, 0x35, big, sizeof(big));  // UBX-NAV-SAT
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, f.bytes, f.len));

  const Frame pvt = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, pvt.bytes, pvt.len));
}

static void test_a_navpvt_of_the_wrong_length_is_rejected() {
  UbxParser parser;
  uint8_t shortPayload[40];
  memset(shortPayload, 0, sizeof(shortPayload));
  const Frame f = makeFrame(UbxParser::kClassNav, UbxParser::kIdPvt,
                            shortPayload, sizeof(shortPayload));
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, f.bytes, f.len));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_valid_frame_is_accepted_on_its_last_byte);
  RUN_TEST(test_a_flipped_payload_byte_is_rejected);
  RUN_TEST(test_leading_garbage_is_skipped);
  RUN_TEST(test_a_truncated_frame_does_not_block_the_next_one);
  RUN_TEST(test_doubled_sync_byte_still_syncs);
  RUN_TEST(test_another_ubx_message_is_consumed_but_not_reported);
  RUN_TEST(test_a_message_longer_than_navpvt_does_not_overflow);
  RUN_TEST(test_a_navpvt_of_the_wrong_length_is_rejected);
  return UNITY_END();
}
