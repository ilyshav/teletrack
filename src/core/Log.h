#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/LogLevel.h"
#include "core/LogRing.h"

// Logging for the whole firmware. Formats a line once, prints it to serial,
// and appends it to the on-screen console.
//
// Callable from any task: HTTP handlers run on the AsyncTCP task, everything
// else on loop().
namespace Log {

inline constexpr size_t kMaxMessageBytes = 160;

void begin(unsigned long baud);

void info(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void warn(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void error(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Copies the console into `dest` under the lock. Call from loop() only.
void snapshot(LogRing& dest);

// Bumped on every line. Display redraws when it differs from what it drew.
uint32_t revision();

}  // namespace Log
