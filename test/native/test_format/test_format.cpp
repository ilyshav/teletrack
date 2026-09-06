#include <unity.h>
#include <string.h>

#include "core/Format.h"
#include "core/LogLevel.h"

void setUp() {}
void tearDown() {}

static void test_level_names() {
  TEST_ASSERT_EQUAL_STRING("DBG", logLevelName(LogLevel::Debug));
  TEST_ASSERT_EQUAL_STRING("INF", logLevelName(LogLevel::Info));
  TEST_ASSERT_EQUAL_STRING("WRN", logLevelName(LogLevel::Warn));
  TEST_ASSERT_EQUAL_STRING("ERR", logLevelName(LogLevel::Error));
}

static void test_uptime_short_zero() {
  char buf[16];
  Format::uptimeShort(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00.0", buf);
}

static void test_uptime_short_subsecond() {
  char buf[16];
  Format::uptimeShort(342, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00.3", buf);
}

static void test_uptime_short_minute_rollover() {
  char buf[16];
  // 1 min 3.7 s
  Format::uptimeShort(63700, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("01:03.7", buf);
}

static void test_uptime_short_wraps_at_100_minutes() {
  char buf[16];
  // 100 minutes exactly wraps back to zero: the short form is a fixed-width
  // relative marker, not an absolute clock. uptimeLong() carries absolute time.
  Format::uptimeShort(100u * 60u * 1000u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00.0", buf);
}

static void test_uptime_long_hours() {
  char buf[16];
  // 2 h 3 m 4 s
  Format::uptimeLong((2u * 3600u + 3u * 60u + 4u) * 1000u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("02:03:04", buf);
}

static void test_uptime_long_saturates_at_99_hours() {
  char buf[16];
  Format::uptimeLong(4294967295u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("99:59:59", buf);
}

static void test_log_line_shape() {
  char buf[64];
  Format::logLine(42300, LogLevel::Info, "http", "GET /api/config", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:42.3 [INF] http: GET /api/config", buf);
}

static void test_log_line_truncates_with_tilde() {
  char buf[20];  // far too small
  Format::logLine(42300, LogLevel::Warn, "http", "a very long message indeed",
                  buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(19u, (unsigned)strlen(buf));
  TEST_ASSERT_EQUAL_CHAR('~', buf[18]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_level_names);
  RUN_TEST(test_uptime_short_zero);
  RUN_TEST(test_uptime_short_subsecond);
  RUN_TEST(test_uptime_short_minute_rollover);
  RUN_TEST(test_uptime_short_wraps_at_100_minutes);
  RUN_TEST(test_uptime_long_hours);
  RUN_TEST(test_uptime_long_saturates_at_99_hours);
  RUN_TEST(test_log_line_shape);
  RUN_TEST(test_log_line_truncates_with_tilde);
  return UNITY_END();
}
