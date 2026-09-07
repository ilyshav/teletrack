#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "core/LogRing.h"

void setUp() {}
void tearDown() {}

static void test_fresh_ring_is_all_empty_rows() {
  LogRing ring;
  for (size_t i = 0; i < LogRing::kRows; ++i) {
    TEST_ASSERT_EQUAL_STRING("", ring.row(i));
  }
  TEST_ASSERT_EQUAL_UINT32(0u, ring.revision());
}

static void test_appends_fill_from_the_top() {
  LogRing ring;
  ring.append("one");
  ring.append("two");
  ring.append("three");

  TEST_ASSERT_EQUAL_STRING("one", ring.row(0));
  TEST_ASSERT_EQUAL_STRING("two", ring.row(1));
  TEST_ASSERT_EQUAL_STRING("three", ring.row(2));
  TEST_ASSERT_EQUAL_STRING("", ring.row(3));
}

static void test_exactly_full_keeps_every_line() {
  LogRing ring;
  char line[16];
  for (size_t i = 0; i < LogRing::kRows; ++i) {
    snprintf(line, sizeof(line), "line%u", (unsigned)i);
    ring.append(line);
  }
  TEST_ASSERT_EQUAL_STRING("line0", ring.row(0));
  snprintf(line, sizeof(line), "line%u", (unsigned)(LogRing::kRows - 1));
  TEST_ASSERT_EQUAL_STRING(line, ring.row(LogRing::kRows - 1));
}

static void test_wrap_around_drops_the_oldest_and_keeps_order() {
  LogRing ring;
  char line[16];
  // One more than capacity: line0 falls off the top.
  for (size_t i = 0; i <= LogRing::kRows; ++i) {
    snprintf(line, sizeof(line), "line%u", (unsigned)i);
    ring.append(line);
  }
  TEST_ASSERT_EQUAL_STRING("line1", ring.row(0));
  snprintf(line, sizeof(line), "line%u", (unsigned)LogRing::kRows);
  TEST_ASSERT_EQUAL_STRING(line, ring.row(LogRing::kRows - 1));

  // And the rows in between are still in order.
  for (size_t i = 0; i < LogRing::kRows; ++i) {
    snprintf(line, sizeof(line), "line%u", (unsigned)(i + 1));
    TEST_ASSERT_EQUAL_STRING(line, ring.row(i));
  }
}

static void test_wrap_around_many_times() {
  LogRing ring;
  char line[16];
  for (size_t i = 0; i < LogRing::kRows * 7 + 3; ++i) {
    snprintf(line, sizeof(line), "line%u", (unsigned)i);
    ring.append(line);
  }
  const size_t newest = LogRing::kRows * 7 + 2;
  snprintf(line, sizeof(line), "line%u", (unsigned)newest);
  TEST_ASSERT_EQUAL_STRING(line, ring.row(LogRing::kRows - 1));
  snprintf(line, sizeof(line), "line%u", (unsigned)(newest - LogRing::kRows + 1));
  TEST_ASSERT_EQUAL_STRING(line, ring.row(0));
}

static void test_line_of_exactly_kcols_is_not_truncated() {
  LogRing ring;
  char line[LogRing::kLineSize];
  memset(line, 'x', LogRing::kCols);
  line[LogRing::kCols] = '\0';

  ring.append(line);
  TEST_ASSERT_EQUAL_UINT((unsigned)LogRing::kCols, (unsigned)strlen(ring.row(0)));
  TEST_ASSERT_EQUAL_CHAR('x', ring.row(0)[LogRing::kCols - 1]);
}

static void test_over_long_line_is_truncated_with_a_tilde() {
  LogRing ring;
  char line[LogRing::kCols + 20];
  memset(line, 'x', sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  ring.append(line);
  TEST_ASSERT_EQUAL_UINT((unsigned)LogRing::kCols, (unsigned)strlen(ring.row(0)));
  TEST_ASSERT_EQUAL_CHAR('~', ring.row(0)[LogRing::kCols - 1]);
}

static void test_null_line_appends_an_empty_row() {
  LogRing ring;
  ring.append(nullptr);
  TEST_ASSERT_EQUAL_STRING("", ring.row(0));
  TEST_ASSERT_EQUAL_UINT32(1u, ring.revision());
}

static void test_revision_advances_only_on_append() {
  LogRing ring;
  TEST_ASSERT_EQUAL_UINT32(0u, ring.revision());
  ring.append("a");
  TEST_ASSERT_EQUAL_UINT32(1u, ring.revision());
  ring.append("b");
  TEST_ASSERT_EQUAL_UINT32(2u, ring.revision());

  (void)ring.row(0);
  (void)ring.row(LogRing::kRows - 1);
  TEST_ASSERT_EQUAL_UINT32(2u, ring.revision());
}

static void test_row_out_of_range_is_empty() {
  LogRing ring;
  ring.append("a");
  TEST_ASSERT_EQUAL_STRING("", ring.row(LogRing::kRows));
  TEST_ASSERT_EQUAL_STRING("", ring.row(9999));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fresh_ring_is_all_empty_rows);
  RUN_TEST(test_appends_fill_from_the_top);
  RUN_TEST(test_exactly_full_keeps_every_line);
  RUN_TEST(test_wrap_around_drops_the_oldest_and_keeps_order);
  RUN_TEST(test_wrap_around_many_times);
  RUN_TEST(test_line_of_exactly_kcols_is_not_truncated);
  RUN_TEST(test_over_long_line_is_truncated_with_a_tilde);
  RUN_TEST(test_null_line_appends_an_empty_row);
  RUN_TEST(test_revision_advances_only_on_append);
  RUN_TEST(test_row_out_of_range_is_empty);
  return UNITY_END();
}
