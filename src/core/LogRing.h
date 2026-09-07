#pragma once

#include <stddef.h>
#include <stdint.h>

// Fixed-size scrolling console buffer for the on-screen log.
//
// Not thread-safe on purpose: Display owns the critical section, LogRing owns
// the data. Keeping the locking out of here is what lets it be tested on the
// host with no FreeRTOS.
class LogRing {
 public:
  static constexpr size_t kRows = 20;
  static constexpr size_t kCols = 53;
  static constexpr size_t kLineSize = kCols + 1;

  // Appends a line, dropping the oldest when full. Lines longer than kCols are
  // truncated with a trailing '~'. A null line appends an empty row.
  void append(const char* line);

  // Index 0 is the oldest visible row, kRows-1 the newest. Rows never written,
  // and out-of-range indices, read as "".
  const char* row(size_t index) const;

  // Monotonic; bumped by append(). Display redraws when this differs from
  // the revision it last drew.
  uint32_t revision() const;

 private:
  char lines_[kRows][kLineSize] = {};
  size_t count_ = 0;  // rows written so far, saturating at kRows
  size_t head_ = 0;   // index into lines_ of the oldest row, once full
  uint32_t revision_ = 0;
};
