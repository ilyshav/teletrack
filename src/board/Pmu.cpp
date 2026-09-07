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
  //   VBACKUP GNSS RTC   DCDC1 ESP32 VDD (protected, never disable)
  // The earlier labels here said ALDO2 was the display and ALDO3 the GPS.
  // Both were wrong. The display is on none of them, which is why the dark
  // panel turned out to be an I2C address problem rather than a power one.
  g_pmu.setALDO4Voltage(3300);
  g_pmu.enableALDO4();
  // Keeps the GNSS RTC and its almanac alive across power cycles. Without it
  // every start is a cold start: minutes to first fix instead of seconds.
  g_pmu.setButtonBatteryChargeVoltage(3300);
  g_pmu.enableButtonBatteryCharge();

  // Charging is a hardware function of the AXP2101 and works without any of
  // this, but the current and termination voltage would then come from
  // whatever the chip powers up with. 4.2 V is correct for a standard 18650;
  // the part can also be told 4.35 or 4.4 V, which suits some high-voltage
  // cells and would overcharge a normal one. Set it rather than inherit it.
  g_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  g_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
  g_pmu.enableCellbatteryCharge();

  present_ = true;
  Log::info("pmu", "AXP2101 up, charging 500mA to 4.2V");
  return true;
}

#else

bool Pmu::begin() {
  present_ = false;
  return true;  // no PMU on this board; nothing to do and nothing failed
}

#endif
