#ifndef PIO_UNIT_TESTING

#include <Arduino.h>

#include "ble/BleLink.h"
#include "ble/TelemetryRing.h"
#include "config/ConfigPortal.h"
#include "config/Settings.h"
#include "core/DeviceStatus.h"
#include "core/Log.h"
#include "radio/ModeButton.h"
#include "radio/RadioMode.h"
#include "ui/Display.h"

namespace {

// Synthetic telemetry until GPS and IMU exist: same cadence and packet size as
// the real thing, so the throughput measured here is the throughput Phase 3 gets.
constexpr uint32_t kSampleHz = 200;
constexpr uint32_t kSampleIntervalMs = 1000 / kSampleHz;  // 5 ms

// The one global. Everything else is reached through it by reference.
struct App {
  Settings settings = Settings::defaults();
  ConfigPortal portal{settings};
  TelemetryRing ring;
  BleLink ble;
  ModeButton button;
  ModeController modes;
  Display display;
};

App app;

uint32_t g_lastSampleMs = 0;
uint32_t g_rateWindowMs = 0;
uint32_t g_rateWindowBytes = 0;
uint32_t g_kbPerSec = 0;

void startCurrentMode() {
  if (app.modes.mode() == RadioMode::Wifi) {
    if (!app.portal.begin()) {
      Log::error("boot", "config portal failed to start");
    }
  } else {
    if (!app.ble.begin("teletrack", app.ring)) {
      Log::error("ble", "failed to start");
    }
  }
  app.modes.switchComplete();
}

void stopCurrentMode(RadioMode leaving) {
  if (leaving == RadioMode::Wifi) {
    app.portal.end();
  } else {
    app.ble.end();
  }
}

void produceSample(uint32_t nowMs) {
  if (nowMs - g_lastSampleMs < kSampleIntervalMs) {
    return;
  }
  g_lastSampleMs = nowMs;

  // Phase 3 replaces this with real GPS and IMU fields. The pattern is
  // deliberate: a receiver can spot corruption as well as loss.
  uint8_t payload[TelemetrySample::kPayloadBytes];
  for (size_t i = 0; i < sizeof(payload); ++i) {
    payload[i] = static_cast<uint8_t>(nowMs + i);
  }
  app.ring.push(nowMs, payload);
}

DeviceStatus buildStatus(uint32_t nowMs) {
  DeviceStatus s = app.portal.status();
  s.mode = app.modes.mode();
  s.bleConnected = app.ble.connected();
  s.kbPerSec = g_kbPerSec;
  s.dropped = app.ring.dropped();
  s.holdMs = app.button.heldMs(nowMs);
  s.uptimeMs = nowMs;
  return s;
}

void updateRate(uint32_t nowMs) {
  if (nowMs - g_rateWindowMs < 1000) {
    return;
  }
  const uint32_t sent = app.ble.sentBytes();
  g_kbPerSec = (sent - g_rateWindowBytes) / 1024;
  g_rateWindowBytes = sent;
  g_rateWindowMs = nowMs;
}

}  // namespace

void setup() {
  Log::begin(115200);
  if (!app.display.begin()) {
    Log::error("tft", "display init failed, serial only");
  }
  Log::info("boot", "teletrack");

  app.button.begin();
  startCurrentMode();
}

void loop() {
  const uint32_t now = millis();

  if (app.button.tick(now)) {
    const RadioMode leaving = app.modes.mode();
    if (app.modes.handle(ModeEvent::ButtonHeld)) {
      Log::info("mode", "switching");
      // Down before up: both radios share one front end.
      stopCurrentMode(leaving);
      startCurrentMode();
    }
  }

  if (app.modes.mode() == RadioMode::Wifi) {
    app.portal.tick(now);
  } else {
    produceSample(now);
    app.ble.tick(now);
  }

  updateRate(now);
  app.display.tick(now, buildStatus(now));
  delay(1);  // yield to the WiFi and BLE tasks
}

#endif  // PIO_UNIT_TESTING
