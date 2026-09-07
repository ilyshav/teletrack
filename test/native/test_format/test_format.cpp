#include <unity.h>
#include <string.h>

#include "core/Format.h"
#include "core/LogLevel.h"

void setUp() {}
void tearDown() {}

static void test_level_names() {
  TEST_ASSERT_EQUAL_STRING("INF", logLevelName(LogLevel::Info));
  TEST_ASSERT_EQUAL_STRING("WRN", logLevelName(LogLevel::Warn));
  TEST_ASSERT_EQUAL_STRING("ERR", logLevelName(LogLevel::Error));
}

static void test_uptime_zero() {
  char buf[16];
  Format::uptime(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00:00", buf);
}

static void test_uptime_counts_hours_minutes_seconds() {
  char buf[16];
  Format::uptime((2u * 3600u + 3u * 60u + 4u) * 1000u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("02:03:04", buf);
}

static void test_uptime_saturates_at_99_hours() {
  char buf[16];
  Format::uptime(4294967295u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("99:59:59", buf);
}

static void test_log_line_shape() {
  char buf[64];
  Format::logLine(42300, LogLevel::Info, "http", "GET /api/config", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00:42 [INF] http: GET /api/config", buf);
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
  RUN_TEST(test_uptime_zero);
  RUN_TEST(test_uptime_counts_hours_minutes_seconds);
  RUN_TEST(test_uptime_saturates_at_99_hours);
  RUN_TEST(test_log_line_shape);
  RUN_TEST(test_log_line_truncates_with_tilde);
  return UNITY_END();
}
