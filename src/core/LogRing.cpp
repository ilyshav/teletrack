#include "core/LogRing.h"

#include <string.h>

void LogRing::append(const char* line) {
  const char* src = (line != nullptr) ? line : "";

  size_t slot;
  if (count_ < kRows) {
    slot = count_;
    ++count_;
  } else {
    slot = head_;
    head_ = (head_ + 1) % kRows;
  }

  const size_t len = strlen(src);
  if (len > kCols) {
    memcpy(lines_[slot], src, kCols - 1);
    lines_[slot][kCols - 1] = '~';
    lines_[slot][kCols] = '\0';
  } else {
    memcpy(lines_[slot], src, len);
    lines_[slot][len] = '\0';
  }

  ++revision_;
}

const char* LogRing::row(size_t index) const {
  if (index >= kRows) {
    return "";
  }
  if (count_ < kRows) {
    return (index < count_) ? lines_[index] : "";
  }
  return lines_[(head_ + index) % kRows];
}

uint32_t LogRing::revision() const { return revision_; }
