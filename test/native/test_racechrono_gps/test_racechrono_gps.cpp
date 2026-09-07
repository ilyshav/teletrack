#include <unity.h>

#include "ble/RaceChronoGps.h"

void setUp() {}
void tearDown() {}

namespace {

GpsFix makeFix() {
  GpsFix fix;
  fix.latE7 = 0x12345678;       // ~30.5 N, distinct bytes to catch endian bugs
  fix.lonE7 = -0x12345678;      // ~-30.5, exercises two's complement
  fix.altitudeM = 100.0f;
  fix.speedKmh = 50.0f;
  fix.bearingDeg = 123.45f;
  fix.hdop = 1.2f;
  fix.fixQuality = 2;
  fix.satellites = 50;
  fix.year = 2026;
  fix.month = 9;
  fix.day = 7;
  fix.hour = 14;
  fix.minute = 23;
  fix.seconds = 45;
  fix.millis = 500;
  return fix;
}

}  // namespace

// --- dateAndHour ---

static void test_date_and_hour_formula() {
  GpsFix fix = makeFix();
  // (2026-2000)*8928 + (9-1)*744 + (7-1)*24 + 14
  // = 232128 + 5952 + 144 + 14 = 238238
  TEST_ASSERT_EQUAL_UINT32(238238u, RaceChronoGps::dateAndHour(fix));
}

static void test_date_and_hour_at_epoch_is_zero() {
  GpsFix fix;  // year 2000, month 1, day 1, hour 0 (defaults)
  TEST_ASSERT_EQUAL_UINT32(0u, RaceChronoGps::dateAndHour(fix));
}

// --- encodeMain: big-endian, sync bits, and field layout ---

static void test_encode_main_full_field_layout_is_big_endian() {
  GpsFix fix = makeFix();
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, /*syncBits=*/5, out);

  // Time-from-hour-start = 23*30000 + 45*500 + 500/2 = 712750 = 0xAE02E.
  // top5 = 0x0A, mid byte = 0xE0, low byte = 0x2E.
  TEST_ASSERT_EQUAL_HEX8(0xAA, out[0]);  // (5 & 0x7) << 5 | 0x0A = 0xA0 | 0x0A
  TEST_ASSERT_EQUAL_HEX8(0xE0, out[1]);
  TEST_ASSERT_EQUAL_HEX8(0x2E, out[2]);

  // Fix quality (2 bits, clamped to 3) << 6 | satellites (6 bits): (2<<6)|50
  TEST_ASSERT_EQUAL_HEX8(0xB2, out[3]);

  // Latitude 0x12345678, big-endian.
  TEST_ASSERT_EQUAL_HEX8(0x12, out[4]);
  TEST_ASSERT_EQUAL_HEX8(0x34, out[5]);
  TEST_ASSERT_EQUAL_HEX8(0x56, out[6]);
  TEST_ASSERT_EQUAL_HEX8(0x78, out[7]);

  // Longitude -0x12345678 as two's complement 0xEDCBA988, big-endian.
  TEST_ASSERT_EQUAL_HEX8(0xED, out[8]);
  TEST_ASSERT_EQUAL_HEX8(0xCB, out[9]);
  TEST_ASSERT_EQUAL_HEX8(0xA9, out[10]);
  TEST_ASSERT_EQUAL_HEX8(0x88, out[11]);

  // Altitude 100 m, fine: round((100+500)*10) = 6000 = 0x1770.
  TEST_ASSERT_EQUAL_HEX8(0x17, out[12]);
  TEST_ASSERT_EQUAL_HEX8(0x70, out[13]);

  // Speed 50 km/h, fine: round(50*100) = 5000 = 0x1388.
  TEST_ASSERT_EQUAL_HEX8(0x13, out[14]);
  TEST_ASSERT_EQUAL_HEX8(0x88, out[15]);

  // Bearing 123.45 deg: round(123.45*100) = 12345 = 0x3039.
  TEST_ASSERT_EQUAL_HEX8(0x30, out[16]);
  TEST_ASSERT_EQUAL_HEX8(0x39, out[17]);

  // HDOP 1.2: round(1.2*10) = 12.
  TEST_ASSERT_EQUAL_HEX8(0x0C, out[18]);

  // VDOP: unimplemented, always the documented invalid value.
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[19]);
}

static void test_sync_bits_occupy_top_3_bits_of_byte_0() {
  GpsFix fix = makeFix();
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, /*syncBits=*/0x7, out);
  TEST_ASSERT_EQUAL_HEX8(0x7, (out[0] >> 5) & 0x7);

  RaceChronoGps::encodeMain(fix, /*syncBits=*/0x3, out);
  TEST_ASSERT_EQUAL_HEX8(0x3, (out[0] >> 5) & 0x7);

  // Only the low 3 bits of the argument are used, per the 3-bit field width.
  RaceChronoGps::encodeMain(fix, /*syncBits=*/0xFF, out);
  TEST_ASSERT_EQUAL_HEX8(0x7, (out[0] >> 5) & 0x7);
}

// --- altitude fine/coarse switchover ---

static void test_altitude_fine_just_below_threshold() {
  GpsFix fix = makeFix();
  fix.altitudeM = 2776.6f;  // below the 2776.7 m switchover
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, 0, out);
  // Fine: round((2776.6+500)*10) = round(32766.0) = 32766 = 0x7FFE.
  // High bit clear -> fine encoding, not the 0x8000-tagged coarse one.
  TEST_ASSERT_EQUAL_HEX8(0x7F, out[12]);
  TEST_ASSERT_EQUAL_HEX8(0xFE, out[13]);
}

static void test_altitude_coarse_just_above_threshold() {
  GpsFix fix = makeFix();
  fix.altitudeM = 2776.8f;  // above the 2776.7 m switchover
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, 0, out);
  // Coarse: round(2776.8+500) = round(3276.8) = 3277 = 0xCCD, tagged 0x8000.
  TEST_ASSERT_EQUAL_HEX8(0x8C, out[12]);
  TEST_ASSERT_EQUAL_HEX8(0xCD, out[13]);
}

static void test_altitude_coarse_for_a_clearly_high_value() {
  GpsFix fix = makeFix();
  fix.altitudeM = 3000.0f;
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, 0, out);
  // round(3000+500) = 3500 = 0xDAC, tagged 0x8000 -> 0x8DAC.
  TEST_ASSERT_EQUAL_HEX8(0x8D, out[12]);
  TEST_ASSERT_EQUAL_HEX8(0xAC, out[13]);
}

// --- speed fine/coarse switchover ---

static void test_speed_fine_just_below_threshold() {
  GpsFix fix = makeFix();
  fix.speedKmh = 327.66f;  // below the 327.67 km/h switchover
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, 0, out);
  // Fine: round(327.66*100) = 32766 = 0x7FFE.
  TEST_ASSERT_EQUAL_HEX8(0x7F, out[14]);
  TEST_ASSERT_EQUAL_HEX8(0xFE, out[15]);
}

static void test_speed_coarse_just_above_threshold() {
  GpsFix fix = makeFix();
  fix.speedKmh = 327.68f;  // above the 327.67 km/h switchover
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, 0, out);
  // Coarse: round(327.68*10) = round(3276.8) = 3277 = 0xCCD, tagged 0x8000.
  TEST_ASSERT_EQUAL_HEX8(0x8C, out[14]);
  TEST_ASSERT_EQUAL_HEX8(0xCD, out[15]);
}

// --- documented invalid values, never zero ---

static void test_unknown_lat_lon_send_invalid_sentinel_not_zero() {
  GpsFix fix = makeFix();
  fix.latE7 = 0x7FFFFFFF;
  fix.lonE7 = 0x7FFFFFFF;
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, 0, out);
  TEST_ASSERT_EQUAL_HEX8(0x7F, out[4]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[5]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[6]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[7]);
  TEST_ASSERT_EQUAL_HEX8(0x7F, out[8]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[9]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[10]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, out[11]);
}

static void test_unknown_satellites_send_0x3f_not_zero() {
  GpsFix fix = makeFix();
  fix.fixQuality = 0;
  fix.satellites = 0xFF;  // "don't know" sentinel: clamps to the invalid 0x3F
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, 0, out);
  TEST_ASSERT_EQUAL_HEX8(0x3F, out[3]);
  TEST_ASSERT_NOT_EQUAL(0, out[3]);
}

static void test_out_of_range_hdop_sends_the_invalid_value() {
  // A receiver with no fix reports pDOP 99.99. Encoded naively that is 1000,
  // which truncates to 232 and reads as a precise 23.2.
  GpsFix fix = makeFix();
  fix.hdop = 99.99f;
  uint8_t out[20];
  RaceChronoGps::encodeMain(fix, 0, out);
  TEST_ASSERT_EQUAL_UINT8(0xFF, out[18]);

  // The top of the representable range still encodes as itself.
  fix.hdop = 25.4f;
  RaceChronoGps::encodeMain(fix, 0, out);
  TEST_ASSERT_EQUAL_UINT8(254, out[18]);
}

static void test_vdop_is_always_the_invalid_value() {
  // The reference has no VDOP source and always sends 0xFF; ported as-is.
  GpsFix a = makeFix();
  GpsFix b = makeFix();
  b.hdop = 9.9f;
  uint8_t outA[20];
  uint8_t outB[20];
  RaceChronoGps::encodeMain(a, 0, outA);
  RaceChronoGps::encodeMain(b, 0, outB);
  TEST_ASSERT_EQUAL_HEX8(0xFF, outA[19]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, outB[19]);
}

// --- encodeTime ---

static void test_encode_time_is_big_endian_and_matches_date_and_hour() {
  GpsFix fix = makeFix();
  uint8_t out[3];
  RaceChronoGps::encodeTime(fix, /*syncBits=*/5, out);
  // dateAndHour = 238238 = 0x3A29E -> top5 = 0x03, mid = 0xA2, low = 0x9E.
  TEST_ASSERT_EQUAL_HEX8(0xA3, out[0]);  // (5 & 0x7) << 5 | 0x03 = 0xA0 | 0x03
  TEST_ASSERT_EQUAL_HEX8(0xA2, out[1]);
  TEST_ASSERT_EQUAL_HEX8(0x9E, out[2]);
}

static void test_sync_bits_are_identical_between_main_and_time() {
  GpsFix fix = makeFix();
  uint8_t mainOut[20];
  uint8_t timeOut[3];
  for (uint8_t sync = 0; sync < 8; ++sync) {
    RaceChronoGps::encodeMain(fix, sync, mainOut);
    RaceChronoGps::encodeTime(fix, sync, timeOut);
    TEST_ASSERT_EQUAL_HEX8((mainOut[0] >> 5) & 0x7, (timeOut[0] >> 5) & 0x7);
  }
}

// --- sync bit counter driven by dateAndHour changes ---

static void test_sync_bits_do_not_increment_on_repeated_identical_fix() {
  RaceChronoGps gps;
  GpsFix fix = makeFix();
  uint8_t first = gps.updateSyncBits(fix);
  uint8_t second = gps.updateSyncBits(fix);
  uint8_t third = gps.updateSyncBits(fix);
  TEST_ASSERT_EQUAL_UINT8(first, second);
  TEST_ASSERT_EQUAL_UINT8(first, third);
}

static void test_sync_bits_increment_only_when_date_and_hour_changes() {
  RaceChronoGps gps;
  GpsFix fix = makeFix();
  uint8_t before = gps.updateSyncBits(fix);

  // Same hour, minute/seconds/millis advance: dateAndHour is unchanged, so no
  // increment even though this is a materially different fix.
  fix.minute = 24;
  fix.seconds = 1;
  fix.millis = 0;
  TEST_ASSERT_EQUAL_UINT8(before, gps.updateSyncBits(fix));

  // Hour rolls over: dateAndHour changes, so the counter bumps by exactly 1.
  fix.hour = 15;
  uint8_t after = gps.updateSyncBits(fix);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)((before + 1) & 0x7), after);
}

static void test_sync_bits_wrap_at_3_bits() {
  RaceChronoGps gps;
  GpsFix fix = makeFix();
  fix.year = 2000;
  fix.month = 1;
  fix.day = 1;
  fix.hour = 0;

  // hour=0 has dateAndHour(fix) == 0, the tracker's initial "unset" sentinel,
  // so the very first call does not count as a change (see the epoch test
  // above) -- the sequence below starts at 0 and climbs by exactly 1 per
  // distinct hour, wrapping from 7 back to 0 on the 9th distinct value.
  static const uint8_t expected[] = {0, 1, 2, 3, 4, 5, 6, 7, 0};
  for (uint8_t hour = 0; hour <= 8; ++hour) {
    fix.hour = hour;
    TEST_ASSERT_EQUAL_UINT8(expected[hour], gps.updateSyncBits(fix));
  }
}

static void test_sync_bits_prime_without_incrementing_at_zero_epoch() {
  // Faithful-port detail: the reference's previous-value sentinel is 0,
  // which is also dateAndHour's value at year 2000/month 1/day 1/hour 0. A
  // fresh tracker's very first call with that exact fix is indistinguishable
  // from "nothing changed yet", so it must not bump the counter.
  RaceChronoGps gps;
  GpsFix fix;  // defaults to the epoch
  TEST_ASSERT_EQUAL_UINT8(0, gps.updateSyncBits(fix));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_date_and_hour_formula);
  RUN_TEST(test_date_and_hour_at_epoch_is_zero);
  RUN_TEST(test_encode_main_full_field_layout_is_big_endian);
  RUN_TEST(test_sync_bits_occupy_top_3_bits_of_byte_0);
  RUN_TEST(test_altitude_fine_just_below_threshold);
  RUN_TEST(test_altitude_coarse_just_above_threshold);
  RUN_TEST(test_altitude_coarse_for_a_clearly_high_value);
  RUN_TEST(test_speed_fine_just_below_threshold);
  RUN_TEST(test_speed_coarse_just_above_threshold);
  RUN_TEST(test_unknown_lat_lon_send_invalid_sentinel_not_zero);
  RUN_TEST(test_unknown_satellites_send_0x3f_not_zero);
  RUN_TEST(test_out_of_range_hdop_sends_the_invalid_value);
  RUN_TEST(test_vdop_is_always_the_invalid_value);
  RUN_TEST(test_encode_time_is_big_endian_and_matches_date_and_hour);
  RUN_TEST(test_sync_bits_are_identical_between_main_and_time);
  RUN_TEST(test_sync_bits_do_not_increment_on_repeated_identical_fix);
  RUN_TEST(test_sync_bits_increment_only_when_date_and_hour_changes);
  RUN_TEST(test_sync_bits_wrap_at_3_bits);
  RUN_TEST(test_sync_bits_prime_without_incrementing_at_zero_epoch);
  return UNITY_END();
}
