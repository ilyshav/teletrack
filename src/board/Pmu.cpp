#include "board/Pmu.h"

#include "core/Log.h"

#if defined(BOARD_TBEAM)

#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

namespace {
XPowersPMU g_pmu;
}  // namespace

namespace {

// Six I2C devices live on this board and every board-specific value in this
// branch came from documentation rather than measurement. Scanning once at
// boot turns "guess which rail and which address" into something readable on
// serial. Expected: 0x34 AXP2101, 0x3C or 0x3D OLED, 0x1C magnetometer,
// 0x6A/0x6B IMU, 0x76/0x77 BME280, 0x51 RTC.
uint8_t scanBus(TwoWire& bus, const char* label, uint8_t sda, uint8_t scl) {
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; ++addr) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() == 0) {
      Log::info("i2c", "%s: device at 0x%02X", label, addr);
      ++found;
    }
  }
  if (found == 0) {
    Log::info("i2c", "%s: nothing on SDA=%u SCL=%u", label, (unsigned)sda,
              (unsigned)scl);
  }
  return found;
}

}  // namespace

bool Pmu::begin() {
  Wire.begin(BoardConfig::kI2cSda, BoardConfig::kI2cScl);
  scanBus(Wire, "bus0", BoardConfig::kI2cSda, BoardConfig::kI2cScl);

  // The first scan found the display and the BME280 but no AXP2101, IMU, RTC
  // or magnetometer, which says this board splits its I2C across two buses.
  // These pins are the next guess, and the scan reports rather than assumes.
  Wire1.begin(BoardConfig::kPmuSda, BoardConfig::kPmuScl);
  scanBus(Wire1, "bus1", BoardConfig::kPmuSda, BoardConfig::kPmuScl);

  // Wire1, not Wire: the boot scan found the AXP2101 at 0x34 on bus 1
  // (SDA 42 / SCL 41), alongside the RTC. Bus 0 carries the display and the
  // BME280 only.
  if (!g_pmu.begin(Wire1, AXP2101_SLAVE_ADDRESS, BoardConfig::kPmuSda,
                   BoardConfig::kPmuScl)) {
    Log::error("pmu", "AXP2101 not responding");
    present_ = false;
    return false;
  }

  // Which rail feeds what varies across T-Beam revisions. If the display stays
  // dark on a board that is otherwise booting, this block is the first thing to
  // check -- not the display driver.
  g_pmu.setALDO2Voltage(3300);  // display
  g_pmu.enableALDO2();
  g_pmu.setALDO3Voltage(3300);  // GPS -- unused in this branch, powered anyway
  g_pmu.enableALDO3();

  // Charging is a hardware function of the AXP2101 and works without any of
  // this, but the current and termination voltage would then come from
  // whatever the chip powers up with. 4.2 V is correct for a standard 18650;
  // the part can also be told 4.35 or 4.4 V, which suits some high-voltage
  // cells and would overcharge a normal one. Set it rather than inherit it.
  g_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  g_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
  g_pmu.enableCellbatteryCharge();

  // getXVoltage() reports the CONFIGURED voltage, not whether the rail is
  // switched on -- every rail reading 3300 mV told us nothing. Log the enable
  // state, which is the thing that matters.
  Log::info("pmu", "before: ALDO1=%d ALDO2=%d ALDO3=%d ALDO4=%d BLDO1=%d BLDO2=%d",
            g_pmu.isEnableALDO1(), g_pmu.isEnableALDO2(), g_pmu.isEnableALDO3(),
            g_pmu.isEnableALDO4(), g_pmu.isEnableBLDO1(), g_pmu.isEnableBLDO2());

  // No rail is enabled here. The boot log showed all six already on before we
  // touched anything, so the dark panel was never a power problem -- it was
  // the display talking to the magnetometer at 0x3C. Left as a report only.

  present_ = true;
  Log::info("pmu", "AXP2101 up");
  Log::info("pmu", "charging 500mA to 4.2V");
  return true;
}

#else

bool Pmu::begin() {
  present_ = false;
  return true;  // no PMU on this board; nothing to do and nothing failed
}

#endif
