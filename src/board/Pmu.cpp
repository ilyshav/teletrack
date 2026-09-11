#include "board/Pmu.h"

#include "core/Log.h"

#if defined(BOARD_TBEAM)

#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

namespace {
XPowersPMU g_pmu;
}  // namespace

bool Pmu::begin() {
  // This board splits I2C across two buses. Wire1 (SDA 42 / SCL 41) carries
  // the AXP2101 at 0x34 and the RTC; Wire carries the display, magnetometer
  // and BME280, and OledDisplay owns it.
  Wire1.begin(BoardConfig::kPmuSda, BoardConfig::kPmuScl);

  if (!g_pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, BoardConfig::kPmuSda,
                   BoardConfig::kPmuScl)) {
    Log::error("pmu", "AXP2101 not responding");
    present_ = false;
    return false;
  }

  // Rail map for the T-Beam S3 Supreme, from LilyGO's own board support:
  //   ALDO1 sensors   ALDO2 SD card   ALDO3 LoRa   ALDO4 GPS
  //   DCDC1 ESP32 VDD (protected, never disable)
  // The earlier labels here said ALDO2 was the display and ALDO3 the GPS.
  // Both were wrong. The display is on none of them, which is why the dark
  // panel turned out to be an I2C address problem rather than a power one.
  g_pmu.setALDO4Voltage(3300);
  g_pmu.enableALDO4();
  // LoRa lives on ALDO3 and this project does not use it. The AXP2101 brings
  // every rail up by itself -- the boot log showed ALDO1 through BLDO2 all
  // enabled before this function ever ran -- so the radio has been powered
  // since the board was first flashed. Off, on every boot.
  g_pmu.disableALDO3();
  // Read back rather than assume. That ALDO3 is the LoRa rail comes from
  // LilyGO's board support, and that the disable took comes from nothing at
  // all until this line prints. Worth one log line before anyone unplugs an
  // antenna on the strength of it.
  Log::info("pmu", "LoRa rail ALDO3: %s",
            g_pmu.isEnableALDO3() ? "STILL ON" : "off");

  // Everything else the board powers up with, enabled explicitly rather than
  // trusted to the AXP2101's power-up state. That state only applies to a real
  // power-on, and the display's supply is among these rails: bringing the board
  // up with them off hangs u8g2's init on an I2C device that is not there, with
  // a dark screen and no serial output at all.
  g_pmu.setALDO1Voltage(3300);
  g_pmu.enableALDO1();
  g_pmu.setALDO2Voltage(3300);
  g_pmu.enableALDO2();
  g_pmu.setBLDO1Voltage(3300);
  g_pmu.enableBLDO1();
  g_pmu.setBLDO2Voltage(3300);
  g_pmu.enableBLDO2();
  // How much the board may draw from USB in total. Nothing is charging any
  // more, but this still governs the 150-250 mA the board itself pulls, and
  // leaving it at whatever the chip powers up with was never deliberate.
  g_pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);

  present_ = true;
  Log::info("pmu", "AXP2101 up, rails set, 1500mA from USB");
  return true;
}

#else

bool Pmu::begin() {
  present_ = false;
  return true;  // no PMU on this board; nothing to do and nothing failed
}

#endif
