#include "core/Log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "core/Format.h"

namespace Log {
namespace {

LogRing g_ring;
SemaphoreHandle_t g_mutex = nullptr;

void emit(LogLevel level, const char* tag, const char* fmt, va_list args) {
  // Stack-local: concurrent callers never share these buffers, so the only
  // thing needing a lock is the ring append below.
  char message[kMaxMessageBytes];
  vsnprintf(message, sizeof(message), fmt, args);

  // Wider than the screen on purpose: serial gets the full line, and
  // LogRing::append truncates its own copy to the console width.
  char line[240];
  // TODO(phase2): millis() is uptime and resets every boot. Replace with
  // GPS-derived wall clock once the M10N module exists. See "Follow-ups".
  Format::logLine(millis(), level, tag, message, line, sizeof(line));

  Serial.println(line);

  if (g_mutex != nullptr && xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    g_ring.append(line);
    xSemaphoreGive(g_mutex);
  }
}

}  // namespace

void begin(unsigned long baud) {
  g_mutex = xSemaphoreCreateMutex();
  Serial.begin(baud);
  // USB CDC blocks for tx_timeout_ms (250 by default) when the host is not
  // draining the buffer, and that stall lands on whichever task happened to
  // log. A log line is never worth stalling a task for a quarter second --
  // least of all the BLE host task, which has a connection to service. Zero
  // makes a write that cannot fit drop instead of wait.
  Serial.setTxTimeoutMs(0);
}

void info(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  emit(LogLevel::Info, tag, fmt, args);
  va_end(args);
}

void warn(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  emit(LogLevel::Warn, tag, fmt, args);
  va_end(args);
}

void error(const char* tag, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  emit(LogLevel::Error, tag, fmt, args);
  va_end(args);
}

void snapshot(LogRing& dest) {
  if (g_mutex != nullptr && xSemaphoreTake(g_mutex, portMAX_DELAY) == pdTRUE) {
    dest = g_ring;
    xSemaphoreGive(g_mutex);
  }
}

// A 32-bit aligned read is atomic on this core, so this needs no lock. A caller
// racing an append just redraws on the next frame.
uint32_t revision() { return g_ring.revision(); }

}  // namespace Log
