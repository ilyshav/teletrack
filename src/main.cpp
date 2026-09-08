#ifndef PIO_UNIT_TESTING

#include <math.h>

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "ble/BleLink.h"
#include "ble/RaceChronoGps.h"
#include "ble/TelemetryRing.h"
#include "board/BoardConfig.h"
#include "board/Pmu.h"
#include "can/CanBus.h"
#include "can/CanFilter.h"
#include "can/CanFrame.h"
#include "config/ConfigPortal.h"
#include "config/Settings.h"
#include "core/DeviceStatus.h"
#include "core/Log.h"
#include "gps/GpsReceiver.h"
#include "radio/ModeButton.h"
#include "radio/RadioMode.h"
#include "ui/Display.h"

#if defined(BOARD_TBEAM)
#include "ui/OledDisplay.h"
using BoardDisplay = OledDisplay;
#else
#include "ui/TftDisplay.h"
using BoardDisplay = TftDisplay;
#endif

namespace {

#if !defined(BOARD_TBEAM)
// Synthetic GPS until Phase 3 wires up a real receiver: a fix at roughly a
// real GPS update rate, so the transport is exercised the way Phase 3 will
// use it.
constexpr uint32_t kSampleHz = 5;
constexpr uint32_t kSampleIntervalMs = 1000 / kSampleHz;  // 200 ms
#endif

// Delay between a save that changes deviceName and the radio restart that
// applies it. Returning from the POST handler only means the async web
// server queued the response, not that it left the socket -- this gives it a
// moment before the AP that carried it (in WiFi mode) disappears.
constexpr uint32_t kRenameFlushDelayMs = 250;

#if !defined(BOARD_TBEAM)
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
#endif

// The one global. Everything else is reached through it by reference.
struct App {
  Settings settings = Settings::defaults();
  ConfigPortal portal{settings};
  TelemetryRing ring;
  BleLink ble;
  RaceChronoGps gps;
  GpsReceiver gpsRx;
  CanBus can;
  CanFilter canFilter;
  ModeButton button;
  ModeController modes;
  Pmu pmu;
  BoardDisplay display;
};

App app;

#if !defined(BOARD_TBEAM)
uint32_t g_lastSampleMs = 0;
#endif
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

  // A rate change arrives through the same restart path as a rename.
  //
  // Compared against the rate the receiver will actually adopt, not the raw
  // setting. Comparing against a value that gets adjusted makes this fire on
  // every mode switch and every rename, and each begin() re-runs the baud
  // probe, blocking loop() for up to 750 ms.
  const uint8_t wanted = GpsReceiver::effectiveRate(app.settings.sampleHz);
  if (app.gpsRx.present() && app.gpsRx.rateHz() != wanted) {
    app.gpsRx.begin(app.settings.sampleHz);
  }
}

void stopCurrentMode(RadioMode leaving) {
  if (leaving == RadioMode::Wifi) {
    app.portal.end();
  } else {
    app.ble.end();
  }
}

#if defined(BOARD_TBEAM)

void enterSleep() {
  // Before anything is torn down. A triple click can be recognised while the
  // button is still DOWN: the third click's release may go unsampled, and the
  // press that reveals it is the one still in progress. Arming a wake on LOW
  // while GPIO0 is already LOW satisfies the wake condition the instant deep
  // sleep begins, so the board wakes straight back up -- SLEEPING flashes and
  // it reboots, looking exactly like a crash.
  //
  // This has to happen first. Abandoning the sleep after the radio is down and
  // the GPS is in backup would leave the board half torn down with no way back
  // except a reboot.
  const uint32_t deadline = millis() + 5000;
  while (digitalRead(BoardConfig::kModeButtonPin) == LOW && millis() < deadline) {
    delay(10);
  }
  if (digitalRead(BoardConfig::kModeButtonPin) == LOW) {
    Log::warn("sleep", "button still held, not sleeping");
    return;
  }
  delay(50);  // let the contact settle before arming on its level

  Log::info("sleep", "going down");
  // The radio comes down the way a mode switch brings it down, so a connected
  // client sees a clean disconnect rather than a link that simply stops.
  stopCurrentMode(app.modes.mode());
  // Before its neighbours lose power: the receiver has to be told to hold its
  // own almanac while it still has a supply to be told over.
  app.gpsRx.sleep();
  app.display.sleep();
  app.pmu.prepareForSleep();

  // GPIO0 going low. It is a strapping pin, but a deep-sleep wake is not a
  // power-on reset: the ROM takes its fast path through the wake stub and
  // never re-reads the boot-mode straps, so waking on it cannot drop the
  // board into download mode.
  // The pad's pull is not configured by enabling the wake source, and in deep
  // sleep the digital-domain pull is gone. Without this the wake pin can float
  // and either wake the board at random or never wake it at all.
  const gpio_num_t wakePin = static_cast<gpio_num_t>(BoardConfig::kModeButtonPin);
  rtc_gpio_pullup_en(wakePin);
  rtc_gpio_pulldown_dis(wakePin);
  esp_sleep_enable_ext0_wakeup(wakePin, 0);
  esp_deep_sleep_start();  // does not return; a wake restarts setup()
}

#else

void enterSleep() {
  // This board's mode button is GPIO39, and the ESP32-S3's RTC GPIOs stop at
  // 21, so nothing could wake it again. A board asleep with no wake source
  // needs a power cycle to recover, which is worse than not sleeping.
  Log::warn("sleep", "not supported on this board");
}

#endif

#if !defined(BOARD_TBEAM)
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

#endif  // !defined(BOARD_TBEAM)

// Sends one RaceChrono packet per fix. Nothing is buffered ahead of a client:
// the reference implementation notifies the fix it just parsed and keeps no
// history (gpsLoop() in docs/reference/racechrono/canbus-gps-device-main.ino),
// and producing while disconnected once filled the ring with a minute of stale
// fixes that flooded out at 25x real time the moment RaceChrono connected.
void publishFix(const GpsFix& fix) {
  if (!app.ble.connected()) {
    return;
  }

  const uint8_t syncBits = app.gps.updateSyncBits(fix);

  uint8_t mainPacket[TelemetrySample::kSize];
  RaceChronoGps::encodeMain(fix, syncBits, mainPacket);
  app.ring.push(mainPacket);

  uint8_t timePacket[3];
  RaceChronoGps::encodeTime(fix, syncBits, timePacket);
  app.ble.publishTime(timePacket);
}

#if defined(BOARD_TBEAM)

// The receiver's own rate is the sample rate: one packet per NAV-PVT, no
// timer and no resampling. Nothing goes out until the receiver says its clock
// is valid -- a wrong timestamp is the axis every sample would be placed on.
void produceSample(uint32_t nowMs, bool newFix) {
  (void)nowMs;
  if (!newFix) {
    return;
  }
  // Sent whether or not the receiver's clock is valid yet. Withholding until
  // it was cost us the connection outright: indoors the receiver never
  // resolves time, so the device fell silent and RaceChrono dropped the link
  // after about two seconds of nothing, over and over. The reference sends
  // whatever it last parsed, and a fix carrying quality 0 is how a client is
  // told "still acquiring" -- silence says nothing at all.
  //
  // The sync bits stay coherent through this: before the clock resolves,
  // dateAndHour is 0 and updateSyncBits leaves the counter alone, so both
  // characteristics read 0. The first real timestamp bumps it exactly once.
  publishFix(app.gpsRx.fix());
}

#else

void produceSample(uint32_t nowMs, bool newFix) {
  (void)newFix;
  if (!app.ble.connected()) {
    return;
  }
  if (nowMs - g_lastSampleMs < kSampleIntervalMs) {
    return;
  }
  g_lastSampleMs = nowMs;
  publishFix(buildSyntheticFix(nowMs));
}

#endif

// Frames go straight out or are dropped, never stored. A ring of stale
// telemetry has already broken this project once: it filled while nothing was
// connected and then flooded a minute of history at RaceChrono the moment it
// appeared. A CAN frame is worth even less once it is old.
void pumpCanBus(uint32_t nowMs) {
  uint8_t command[8];
  size_t commandLen = 0;
  while (app.ble.takeFilterCommand(command, commandLen)) {
    if (app.canFilter.applyCommand(command, commandLen)) {
      Log::info("can", "filter command %u applied, %u ids",
                static_cast<unsigned>(command[0]),
                static_cast<unsigned>(app.canFilter.trackedIds()));
    } else {
      Log::warn("can", "filter command %u rejected, %u bytes",
                static_cast<unsigned>(command[0]),
                static_cast<unsigned>(commandLen));
    }
  }

  // Bounded so a busy bus cannot monopolise a pass of loop().
  CanFrame frame;
  for (int i = 0; i < 32 && app.can.read(frame); ++i) {
    if (!app.ble.connected() || !app.canFilter.shouldNotify(frame.id, nowMs)) {
      continue;
    }
    uint8_t packet[CanFrame::kMaxPacketBytes];
    const size_t len = encodeCanPacket(frame, packet);
    app.ble.publishCan(packet, len);
  }
}

DeviceStatus buildStatus(uint32_t nowMs) {
  DeviceStatus s = app.portal.status();
  s.mode = app.modes.mode();
  s.bleConnected = app.ble.connected();
  s.kbPerSec = g_kbPerSec;
  s.dropped = app.ring.dropped();
  s.holdMs = app.button.heldMs(nowMs);
  s.uptimeMs = nowMs;
  s.gpsPresent = app.gpsRx.present();
  s.gpsTimeValid = app.gpsRx.timeValid();
  s.gpsFix = app.gpsRx.fix();

  const BatteryState battery = app.pmu.battery();
  s.batteryPresent = battery.present;
  s.batteryUsbPresent = battery.usbPresent;
  s.batteryCharging = battery.charging;
  s.batteryFull = battery.full;
  s.batteryPercent = battery.percent;
  s.batteryMilliVolts = battery.milliVolts;
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
  Log::info("boot", "teletrack on %s", BoardConfig::kBoardName);
  // Says outright whether this boot is a wake. Without it a board that wakes
  // and then hangs looks exactly like one that never woke at all, and the two
  // need completely different investigations.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    Log::info("boot", "woke from sleep on the mode button");
  }

  // Before the display: on the T-Beam the panel sits on a rail this switches.
  if (!app.pmu.begin()) {
    Log::error("pmu", "power management failed to start");
  }

  // The panel's charge pump needs a moment after its rail comes up before it
  // will accept initialisation.
  delay(100);
  if (!app.display.begin()) {
    Log::error("display", "init failed, serial only");
  } else {
    Log::info("display", "init ok");
  }

  // Before any radio starts: the boot mode is BLE, so the portal -- which
  // used to be the only thing that read NVS -- may never run at all.
  app.portal.loadSettings();

  app.button.begin();

  // Rate comes from the saved setting, which Task 7's loadSettings() call
  // just above has already read. On the DevKitC this returns false and the
  // synthetic fix takes over; on the T-Beam a false means no receiver
  // answered, and BLE carries on without it.
  app.gpsRx.begin(app.settings.sampleHz);

  // A bus that is not there must not stop anything else: begin() reports and
  // returns, and read() then yields nothing for the rest of the run.
  app.can.begin();

  startCurrentMode();
}

void loop() {
  const uint32_t now = millis();

  // Re-reads the battery at most once a second; cheap on every other pass.
  app.pmu.tick(now);

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

  switch (app.button.tick(now)) {
    case ButtonEvent::Hold: {
      const RadioMode leaving = app.modes.mode();
      if (app.modes.handle(ModeEvent::ButtonHeld)) {
        Log::info("mode", "switching");
        // Down before up: both radios share one front end.
        stopCurrentMode(leaving);
        startCurrentMode();
      }
      break;
    }
    case ButtonEvent::TripleClick:
      enterSleep();
      break;
    case ButtonEvent::None:
      break;
  }

  // Drained in both modes: the status screen shows satellites while the
  // portal is up, and an undrained UART buffer would overflow either way.
  const bool newFix = app.gpsRx.tick();
  pumpCanBus(now);

  if (app.modes.mode() == RadioMode::Wifi) {
    app.portal.tick(now);
  } else {
    produceSample(now, newFix);
    app.ble.tick(now);
  }

  updateRate(now);
  app.display.tick(now, buildStatus(now));
  delay(1);  // yield to the WiFi and BLE tasks
}

#endif  // PIO_UNIT_TESTING
