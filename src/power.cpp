#include "power.h"

#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

#include "board.h"
#include "config.h"
#include "diag.h"
#include "log.h"

static XPowersPMU s_pmu;
static bool s_ok = false;
static bool s_pwr_flag = false;
static bool s_vbus = false, s_battery = false, s_charging = false;
static int s_pct = -1;
static uint32_t s_last_irq_poll = 0, s_last_batt_poll = 0;

// Diagnostics: voltages read every POWER_MEASURE_MS; the PMU latches USB
// removal and insertion in its IRQ flags, so even dips between two polls count.
static const uint32_t POWER_MEASURE_MS = 1000;
static const uint16_t CUR_LIMIT_MA[] = {100, 500, 900, 1000, 1500, 2000};   // xpowers_axp2101_vbus_cur_limit_t
static const uint16_t VOL_LIMIT_BASE_MV = 3880, VOL_LIMIT_STEP_MV = 80;      // xpower_apx2101_vbus_vol_limit_t
static PowerDiag s_diag;
static uint32_t s_last_measure = 0, s_battery_ms = 0;

static void measure() {
    const uint32_t now = millis();
    const uint32_t dt = now - s_last_measure;
    s_last_measure = now;
    const bool vbus = s_pmu.isVbusIn();
    s_diag.vbus_mv = vbus ? s_pmu.getVbusVoltage() : 0;
    s_diag.vsys_mv = s_pmu.getSystemVoltage();
    s_diag.vbat_mv = s_battery ? s_pmu.getBattVoltage() : 0;
    if (vbus && s_diag.vbus_mv) {
        if (s_diag.vbus_mv < s_diag.vbus_min_mv || !s_diag.vbus_min_mv) s_diag.vbus_min_mv = s_diag.vbus_mv;
        if (s_diag.vbus_mv > s_diag.vbus_max_mv) s_diag.vbus_max_mv = s_diag.vbus_mv;
    }
    if (!vbus) s_battery_ms += dt;
    if (s_diag.vsys_mv && (s_diag.vsys_mv < s_diag.vsys_min_mv || !s_diag.vsys_min_mv))
        s_diag.vsys_min_mv = s_diag.vsys_mv;
}

static void poll_battery() {
    s_vbus = s_pmu.isVbusIn();
    s_battery = s_pmu.isBatteryConnect();
    s_charging = s_battery && s_pmu.isCharging();
    s_pct = s_battery ? s_pmu.getBatteryPercent() : -1;
}

bool power_init() {
    if (!s_pmu.begin(Wire, AXP2101_ADDR, I2C_SDA, I2C_SCL)) {
        LOGE("pmu", "AXP2101 not responding - panel may stay dark");
        return false;
    }
    // Rails as in Waveshare's BSP for this board: ALDO1..4 = 3.3 V
    // (panel, touch, sensors, audio).
    s_pmu.setALDO1Voltage(3300);
    s_pmu.setALDO2Voltage(3300);
    s_pmu.setALDO3Voltage(3300);
    s_pmu.setALDO4Voltage(3300);
    s_pmu.enableALDO1();
    s_pmu.enableALDO2();
    s_pmu.enableALDO4();
    // ALDO3 doubles as the panel reset (RST is not wired to a GPIO): pulse it
    // on -> off -> on with 100 ms holds, exactly like the vendor BSP.
    s_pmu.enableALDO3();
    delay(100);
    s_pmu.disableALDO3();
    delay(100);
    s_pmu.enableALDO3();
    delay(100);

    // Holding PWR powers the board off (AXP2101 hardware). 6 s instead of a
    // shorter level so a slightly long "screen off" press cannot kill it.
    s_pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_6S);

    s_pmu.enableBattDetection();
    s_pmu.enableBattVoltageMeasure();
    s_pmu.enableVbusVoltageMeasure();
    s_pmu.enableSystemVoltageMeasure();
    s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    s_pmu.clearIrqStatus();
    s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_VBUS_INSERT_IRQ | XPOWERS_AXP2101_VBUS_REMOVE_IRQ);
    const uint8_t cur = s_pmu.getVbusCurrentLimit(), vol = s_pmu.getVbusVoltageLimit();
    s_diag.limit_ma = cur < sizeof(CUR_LIMIT_MA) / sizeof(CUR_LIMIT_MA[0]) ? CUR_LIMIT_MA[cur] : 0;
    s_diag.limit_mv = VOL_LIMIT_BASE_MV + VOL_LIMIT_STEP_MV * vol;
    poll_battery();
    s_last_measure = millis();
    measure();
    s_ok = true;
    // The voltages come with the next measure(): the ADC reads 0 right after it is enabled.
    LOGI("pmu", "rails up; vbus=%d battery=%d pct=%d charging=%d; input limit %u mA, %u mV", s_vbus, s_battery, s_pct,
         s_charging, s_diag.limit_ma, s_diag.limit_mv);
    return true;
}

void power_tick() {
    if (!s_ok) return;
    const uint32_t now = millis();
    // The PMU IRQ line is not routed to the MCU, so poll the latched flags.
    // Only the flags that were read are cleared (write 1 to clear):
    // clearIrqStatus() writes 0xFF and would drop a key press latched between
    // the read and the clear.
    if (now - s_last_irq_poll >= POWER_POLL_MS) {
        s_last_irq_poll = now;
        const uint32_t status = (uint32_t)s_pmu.getIrqStatus();   // INTSTS1..3 = bits 23..0
        if (s_pmu.isPekeyShortPressIrq()) s_pwr_flag = true;
        // Removal first: a dip shorter than the poll latches both.
        if (s_pmu.isVbusRemoveIrq()) {
            ++s_diag.usb_lost;
            diag_event(DIAG_USB_OFF, 0, 0);
        }
        if (s_pmu.isVbusInsertIrq()) {
            ++s_diag.usb_back;
            diag_event(DIAG_USB_ON, 0, 0);
        }
        for (int i = 0; i < XPOWERS_AXP2101_INTSTS_CNT; ++i) {
            const uint8_t bits = (uint8_t)(status >> (8 * (XPOWERS_AXP2101_INTSTS_CNT - 1 - i)));
            if (bits) s_pmu.writeRegister(XPOWERS_AXP2101_INTSTS1 + i, bits);
        }
    }
    if (now - s_last_batt_poll >= BATTERY_POLL_MS) {
        s_last_batt_poll = now;
        poll_battery();
    }
    if (now - s_last_measure >= POWER_MEASURE_MS) measure();
}

bool power_pwr_pressed() {
    const bool f = s_pwr_flag;
    s_pwr_flag = false;
    return f;
}

bool power_ok() { return s_ok; }
bool power_vbus() { return s_vbus; }
bool power_has_battery() { return s_battery; }
bool power_charging() { return s_charging; }
int power_battery_pct() { return s_pct; }

void power_diag(PowerDiag* out) {
    *out = s_diag;
    out->battery_s = s_battery_ms / 1000;
}
