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
  // LoRa lives on ALDO3 and this project does not use it. The AXP2101 brings
  // every rail up by itself -- the boot log showed ALDO1 through BLDO2 all
  // enabled before this function ever ran -- so the radio has been powered
  // since the board was first flashed. Off, on every boot, not just in sleep.
  g_pmu.disableALDO3();

  // Everything else the board powers up with, restored explicitly. This is not
  // redundant: prepareForSleep() switches these off, deep sleep does not
  // power-cycle the PMU, and a wake re-enters begin() with them still down. The
  // display's supply is among them -- a wake with them left off hung the board
  // in u8g2's init with a dark screen and no serial output, which is the same
  // failure the I2C address bug produced in phase 3 and just as hard to read.
  g_pmu.setALDO1Voltage(3300);
  g_pmu.enableALDO1();
  g_pmu.setALDO2Voltage(3300);
  g_pmu.enableALDO2();
  g_pmu.setBLDO1Voltage(3300);
  g_pmu.enableBLDO1();
  g_pmu.setBLDO2Voltage(3300);
  g_pmu.enableBLDO2();
  // Charges the AXP2101's backup cell. On T-Beam variants where that rail
  // feeds the GNSS receiver's V_BCKP it holds the almanac and ephemeris across
  // a power cycle, turning a cold start into a warm or hot one.
  //
  // UNVERIFIED ON THIS BOARD. LilyGO's own support for the S3 Supreme never
  // enables VBACKUP -- every reference to it is in other board branches -- so
  // whether it reaches this receiver's backup pin is unknown. Harmless either
  // way; do not rely on it for fix times until someone measures a wake.
  g_pmu.setButtonBatteryChargeVoltage(3300);
  g_pmu.enableButtonBatteryCharge();

  // Nothing below can be read back without these. isBatteryConnect() gates
  // both getBattVoltage() and getBatteryPercent(), and it reads false until
  // battery detection is on -- which is why the firmware could not see the
  // cell at all before now.
  g_pmu.enableBattDetection();
  g_pmu.enableBattVoltageMeasure();
  g_pmu.enableVbusVoltageMeasure();

  // How much the board may draw from USB in total: charging plus running.
  // Never set before, so it was whatever the chip powers up with, and the
  // 150-250 mA this board draws came out of the same unknown budget as the
  // charge current.
  g_pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);

  // 4.2 V is correct for a standard 18650; the part also accepts 4.35 and
  // 4.4 V, which suit high-voltage cells and would overcharge a normal one.
  g_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  // 1000 mA is the part's maximum and 0.5C even for a small 2000 mAh cell,
  // 0.33C for a 3000 mAh one -- safe across any 18650 likely to be fitted.
  // The previous 500 mA was about 0.17C and took most of a day from empty.
  g_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_1000MA);
  // Precharge revives a deeply discharged cell gently; termination decides
  // when the charger stops. Both were inherited defaults, and an inherited
  // value is not a chosen one.
  g_pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_100MA);
  g_pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_100MA);
  g_pmu.enableCellbatteryCharge();

  // Hands the onboard LED to the charger, so it reports charge state as a
  // hardware function -- true even if this firmware hangs.
  g_pmu.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);

  present_ = true;
  Log::info("pmu", "AXP2101 up, charging 1000mA to 4.2V, 1500mA from USB");
  return true;
}

void Pmu::tick(uint32_t nowMs) {
  if (!present_) {
    return;
  }
  // readOnce_ makes the first call read immediately rather than waiting a
  // second: at boot lastReadMs_ and nowMs are both near zero.
  if (readOnce_ && nowMs - lastReadMs_ < kRefreshIntervalMs) {
    return;
  }
  lastReadMs_ = nowMs;
  readOnce_ = true;

  BatteryState state;
  state.present = g_pmu.isBatteryConnect();
  state.usbPresent = g_pmu.isVbusIn();
  if (state.present) {
    const int percent = g_pmu.getBatteryPercent();
    state.percent = percent < 0 ? 0 : static_cast<uint8_t>(percent);
    state.milliVolts = g_pmu.getBattVoltage();
    // getChargerStatus() distinguishes done from merely not charging, which
    // isCharging() cannot. Trickle, pre, constant-current and constant-
    // voltage are all "charging" as far as anyone looking at the screen is
    // concerned.
    const uint8_t status = g_pmu.getChargerStatus();
    state.full = status == XPOWERS_AXP2101_CHG_DONE_STATE;
    state.charging = status == XPOWERS_AXP2101_CHG_TRI_STATE ||
                     status == XPOWERS_AXP2101_CHG_PRE_STATE ||
                     status == XPOWERS_AXP2101_CHG_CC_STATE ||
                     status == XPOWERS_AXP2101_CHG_CV_STATE;
  }
  battery_ = state;
}

void Pmu::prepareForSleep() {
  if (!present_) {
    return;
  }
  // ALDO3 is already off from begin(). ALDO4 stays on: cutting it is what
  // would cost a warm GPS start, which is the whole point of the phase.
  g_pmu.disableALDO1();  // sensors
  g_pmu.disableALDO2();  // SD card
  g_pmu.disableBLDO1();
  g_pmu.disableBLDO2();
  // DC3, DC4 and DC5 are left alone. The vendor calls them the M.2 interface
  // and nothing here knows what else hangs off them; switching them off saved
  // an unmeasured amount and is not worth guessing about on a board that has
  // already failed to come back once.
  Log::info("sleep", "rails down, GPS rail held");
}

#else

bool Pmu::begin() {
  present_ = false;
  return true;  // no PMU on this board; nothing to do and nothing failed
}

void Pmu::tick(uint32_t nowMs) {
  (void)nowMs;  // no PMU on this board; battery_ stays default-constructed
}

void Pmu::prepareForSleep() {}  // no PMU on this board

#endif
