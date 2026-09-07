#ifndef PIO_UNIT_TESTING

#include <math.h>

#include <Arduino.h>

#include "ble/BleLink.h"
#include "ble/RaceChronoGps.h"
#include "ble/TelemetryRing.h"
#include "config/ConfigPortal.h"
#include "config/Settings.h"
#include "core/DeviceStatus.h"
#include "core/Log.h"
#include "radio/ModeButton.h"
#include "radio/RadioMode.h"
#include "ui/TftDisplay.h"

namespace {

// Synthetic GPS until Phase 3 wires up a real receiver: a fix at roughly a
// real GPS update rate, so the transport is exercised the way Phase 3 will
// use it.
constexpr uint32_t kSampleHz = 5;
constexpr uint32_t kSampleIntervalMs = 1000 / kSampleHz;  // 200 ms

// Delay between a save that changes deviceName and the radio restart that
// applies it. Returning from the POST handler only means the async web
// server queued the response, not that it left the socket -- this gives it a
// moment before the AP that carried it (in WiFi mode) disappears.
constexpr uint32_t kRenameFlushDelayMs = 250;

// A frozen point would prove the encoding parses but not that RaceChrono
// tracks updates, which is the whole question Phase 2 exists to answer. So
// instead: a slow circle at walking pace, centered on an arbitrary point,
// with a wall clock that advances from an arbitrary start time.
constexpr double kCircleCenterLatDeg = 52.0;   // arbitrary demo location
constexpr double kCircleCenterLonDeg = 4.0;
constexpr float kCircleRadiusM = 20.0f;
constexpr float kWalkSpeedKmh = 5.0f;                    // brisk walking pace
constexpr float kWalkSpeedMps = kWalkSpeedKmh / 3.6f;
constexpr float kAngularSpeedRadPerS = kWalkSpeedMps / kCircleRadiusM;
constexpr double kEarthRadiusM = 6371000.0;

constexpr uint16_t kStartYear = 2026;
constexpr uint8_t kStartMonth = 9;
constexpr uint8_t kStartDay = 7;
constexpr uint32_t kStartHour = 12;

// The one global. Everything else is reached through it by reference.
struct App {
  Settings settings = Settings::defaults();
  ConfigPortal portal{settings};
  TelemetryRing ring;
  BleLink ble;
  RaceChronoGps gps;
  ModeButton button;
  ModeController modes;
  TftDisplay display;
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
    if (!app.ble.begin(app.settings.deviceName, app.ring)) {
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

// Builds a synthetic fix walking a slow circle, at the wall-clock time
// kStartYear/Month/Day/Hour advanced by nowMs. Day/month/year rollover past
// the 24-hour mark just increments day-of-month without regard for month
// length -- fine for a demo session that runs for minutes, not months.
GpsFix buildSyntheticFix(uint32_t nowMs) {
  const float theta = kAngularSpeedRadPerS * (static_cast<float>(nowMs) / 1000.0f);

  // Tangent-plane offset from the center, in meters: north/east components of
  // a point going around the circle, and of its velocity (the derivative).
  const float northM = kCircleRadiusM * sinf(theta);
  const float eastM = kCircleRadiusM * cosf(theta);
  const float velNorthMps = kWalkSpeedMps * cosf(theta);
  const float velEastMps = -kWalkSpeedMps * sinf(theta);

  const double latRad = kCircleCenterLatDeg * M_PI / 180.0;
  const double dLatDeg = (northM / kEarthRadiusM) * (180.0 / M_PI);
  const double dLonDeg =
      (eastM / (kEarthRadiusM * cos(latRad))) * (180.0 / M_PI);

  float bearingDeg = atan2f(velEastMps, velNorthMps) * (180.0f / static_cast<float>(M_PI));
  if (bearingDeg < 0.0f) {
    bearingDeg += 360.0f;
  }

  const uint32_t totalSeconds = nowMs / 1000;
  const uint32_t millisPart = nowMs % 1000;
  const uint32_t totalMinutes = totalSeconds / 60;
  const uint32_t totalHours = kStartHour + totalMinutes / 60;

  GpsFix fix;
  fix.latE7 = static_cast<int32_t>((kCircleCenterLatDeg + dLatDeg) * 1e7);
  fix.lonE7 = static_cast<int32_t>((kCircleCenterLonDeg + dLonDeg) * 1e7);
  fix.altitudeM = 10.0f;
  fix.speedKmh = kWalkSpeedKmh;
  fix.bearingDeg = bearingDeg;
  fix.hdop = 1.0f;
  fix.fixQuality = 1;
  fix.satellites = 8;
  fix.year = kStartYear;
  fix.month = kStartMonth;
  fix.day = static_cast<uint8_t>(kStartDay + totalHours / 24);
  fix.hour = static_cast<uint8_t>(totalHours % 24);
  fix.minute = static_cast<uint8_t>(totalMinutes % 60);
  fix.seconds = static_cast<uint8_t>(totalSeconds % 60);
  fix.millis = static_cast<uint16_t>(millisPart);
  return fix;
}

void produceSample(uint32_t nowMs) {
  if (nowMs - g_lastSampleMs < kSampleIntervalMs) {
    return;
  }
  g_lastSampleMs = nowMs;

  const GpsFix fix = buildSyntheticFix(nowMs);
  const uint8_t syncBits = app.gps.updateSyncBits(fix);

  uint8_t mainPacket[TelemetrySample::kSize];
  RaceChronoGps::encodeMain(fix, syncBits, mainPacket);
  app.ring.push(mainPacket);

  uint8_t timePacket[3];
  RaceChronoGps::encodeTime(fix, syncBits, timePacket);
  app.ble.publishTime(timePacket);
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

  // A device-name save flags this; acted on here, never inside the request
  // handler, and only once the response has had a moment to leave the async
  // task. The rename can only be requested through the WiFi config page, so
  // in practice this always restarts the portal -- but it restarts whichever
  // radio is actually active, same as a mode switch.
  if (app.portal.renamePending() &&
      now - app.portal.renameFlaggedAtMs() >= kRenameFlushDelayMs) {
    app.portal.clearRenamePending();
    Log::info("cfg", "restarting radio with new name");
    const RadioMode current = app.modes.mode();
    stopCurrentMode(current);
    startCurrentMode();
  }

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
