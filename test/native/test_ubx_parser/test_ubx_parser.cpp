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

static void test_a_truncated_frame_costs_one_more_frame_then_recovers() {
  UbxParser parser;

  // 40 bytes of a 100-byte frame: 6 of header and 34 of payload, leaving the
  // parser waiting for 58 more payload bytes and 2 of checksum.
  const Frame truncated = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, truncated.bytes, 40));

  // Those 60 bytes come out of the NEXT frame, which is therefore lost -- its
  // tail fails the checksum of the frame it was mistaken for. This is the
  // correct cost: a parser must not go hunting for sync bytes inside a
  // declared payload, because payload bytes are arbitrary binary and a
  // B5 62 pair occurs there roughly once every 720 frames.
  const Frame first = makePvtFrame();
  const Frame second = makePvtFrame();
  const int accepted = feedAll(parser, first.bytes, first.len) +
                       feedAll(parser, second.bytes, second.len);
  TEST_ASSERT_EQUAL_INT(1, accepted);
  TEST_ASSERT_EQUAL_UINT32(1, parser.checksumErrors());
}

static void test_a_payload_containing_a_sync_pair_still_frames() {
  // Longitude 0x62B50000 is 165.5701504 degrees east -- Vanuatu -- and puts a
  // literal B5 62 at payload offsets 26 and 27. A parser that rescans for sync
  // inside a payload would abandon this frame and report nothing.
  PvtFields fields;
  fields.lon = 0x62B50000;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_UINT8(0xB5, f.bytes[6 + 26]);
  TEST_ASSERT_EQUAL_UINT8(0x62, f.bytes[6 + 27]);

  UbxParser parser;
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT32(0, parser.checksumErrors());
}

static void test_an_absurd_declared_length_does_not_swallow_the_stream() {
  UbxParser parser;
  // A header claiming a 0x2000-byte payload. Only reachable from a
  // desynchronised stream, but without a bound it would eat 8 kB -- most of a
  // second at 115200 baud -- and every frame inside that window.
  const uint8_t bogus[6] = {0xB5, 0x62, 0x01, 0x07, 0x00, 0x20};
  TEST_ASSERT_EQUAL_INT(0, feedAll(parser, bogus, sizeof(bogus)));

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

static void test_every_field_decodes() {
  UbxParser parser;
  const Frame f = makePvtFrame();
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));

  const GpsFix& fix = parser.fix();
  TEST_ASSERT_EQUAL_UINT16(2026, fix.year);
  TEST_ASSERT_EQUAL_UINT8(9, fix.month);
  TEST_ASSERT_EQUAL_UINT8(7, fix.day);
  TEST_ASSERT_EQUAL_UINT8(14, fix.hour);
  TEST_ASSERT_EQUAL_UINT8(23, fix.minute);
  TEST_ASSERT_EQUAL_UINT8(45, fix.seconds);
  TEST_ASSERT_EQUAL_UINT16(500, fix.millis);
  TEST_ASSERT_EQUAL_UINT8(3, fix.fixType);
  TEST_ASSERT_EQUAL_UINT8(1, fix.fixQuality);
  TEST_ASSERT_EQUAL_UINT8(9, fix.satellites);
  TEST_ASSERT_EQUAL_INT32(523713400, fix.latE7);
  TEST_ASSERT_EQUAL_INT32(48952100, fix.lonE7);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, fix.altitudeM);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 48.2f, fix.speedKmh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 123.45f, fix.bearingDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.9f, fix.hdop);
  TEST_ASSERT_TRUE(parser.timeValid());
}

static void test_negative_latitude_and_longitude_are_signed() {
  PvtFields fields;
  fields.lat = -523713400;
  fields.lon = -48952100;
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_INT32(-523713400, parser.fix().latE7);
  TEST_ASSERT_EQUAL_INT32(-48952100, parser.fix().lonE7);
}

static void test_no_fix_reports_quality_zero_but_keeps_satellites() {
  PvtFields fields;
  fields.fixType = 0;
  fields.flags = 0x00;
  fields.numSV = 4;
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT8(0, parser.fix().fixQuality);
  // The count is what tells you the receiver is alive while you wait.
  TEST_ASSERT_EQUAL_UINT8(4, parser.fix().satellites);
}

static void test_two_d_fix_is_quality_one() {
  PvtFields fields;
  fields.fixType = 2;
  fields.flags = 0x01;
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT8(1, parser.fix().fixQuality);
  TEST_ASSERT_EQUAL_UINT8(2, parser.fix().fixType);
}

static void test_differential_fix_is_quality_two() {
  PvtFields fields;
  fields.fixType = 3;
  fields.flags = 0x03;  // gnssFixOK | diffSoln
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT8(2, parser.fix().fixQuality);
}

static void test_gnss_fix_ok_clear_overrides_a_good_fix_type() {
  PvtFields fields;
  fields.fixType = 3;
  fields.flags = 0x00;  // gnssFixOK clear
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT8(0, parser.fix().fixQuality);
}

static void test_time_valid_requires_both_date_and_time_flags() {
  UbxParser parser;

  PvtFields dateOnly;
  dateOnly.valid = 0x01;
  const Frame a = makePvtFrame(dateOnly);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, a.bytes, a.len));
  TEST_ASSERT_FALSE(parser.timeValid());

  PvtFields timeOnly;
  timeOnly.valid = 0x02;
  const Frame b = makePvtFrame(timeOnly);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, b.bytes, b.len));
  TEST_ASSERT_FALSE(parser.timeValid());

  PvtFields both;
  both.valid = 0x03;
  const Frame c = makePvtFrame(both);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, c.bytes, c.len));
  TEST_ASSERT_TRUE(parser.timeValid());
}

static void test_negative_nano_yields_zero_millis() {
  PvtFields fields;
  fields.nano = -250000000;  // the fix is 250 ms before the reported second
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT16(0, parser.fix().millis);
}

static void test_maximum_nano_clamps_to_999_millis() {
  PvtFields fields;
  fields.nano = 999999999;
  UbxParser parser;
  const Frame f = makePvtFrame(fields);
  TEST_ASSERT_EQUAL_INT(1, feedAll(parser, f.bytes, f.len));
  TEST_ASSERT_EQUAL_UINT16(999, parser.fix().millis);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_valid_frame_is_accepted_on_its_last_byte);
  RUN_TEST(test_a_flipped_payload_byte_is_rejected);
  RUN_TEST(test_leading_garbage_is_skipped);
  RUN_TEST(test_a_truncated_frame_costs_one_more_frame_then_recovers);
  RUN_TEST(test_a_payload_containing_a_sync_pair_still_frames);
  RUN_TEST(test_an_absurd_declared_length_does_not_swallow_the_stream);
  RUN_TEST(test_doubled_sync_byte_still_syncs);
  RUN_TEST(test_another_ubx_message_is_consumed_but_not_reported);
  RUN_TEST(test_a_message_longer_than_navpvt_does_not_overflow);
  RUN_TEST(test_a_navpvt_of_the_wrong_length_is_rejected);
  RUN_TEST(test_every_field_decodes);
  RUN_TEST(test_negative_latitude_and_longitude_are_signed);
  RUN_TEST(test_no_fix_reports_quality_zero_but_keeps_satellites);
  RUN_TEST(test_two_d_fix_is_quality_one);
  RUN_TEST(test_differential_fix_is_quality_two);
  RUN_TEST(test_gnss_fix_ok_clear_overrides_a_good_fix_type);
  RUN_TEST(test_time_valid_requires_both_date_and_time_flags);
  RUN_TEST(test_negative_nano_yields_zero_millis);
  RUN_TEST(test_maximum_nano_clamps_to_999_millis);
  return UNITY_END();
}
