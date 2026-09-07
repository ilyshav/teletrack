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
  Wire.begin(BoardConfig::kI2cSda, BoardConfig::kI2cScl);

  if (!g_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, BoardConfig::kI2cSda,
                   BoardConfig::kI2cScl)) {
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

  present_ = true;
  Log::info("pmu", "AXP2101 up, display rail on");
  Log::info("pmu", "charging 500mA to 4.2V");
  return true;
}

#else

bool Pmu::begin() {
  present_ = false;
  return true;  // no PMU on this board; nothing to do and nothing failed
}

#endif
