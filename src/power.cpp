#include "power.h"

#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

#include "board.h"
#include "config.h"
#include "log.h"

static XPowersPMU s_pmu;
static bool s_ok = false;
static bool s_pwr_flag = false;
static bool s_vbus = false, s_battery = false, s_charging = false;
static int s_pct = -1;
static uint32_t s_last_irq_poll = 0, s_last_batt_poll = 0;

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
    s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    s_pmu.clearIrqStatus();
    s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    poll_battery();
    s_ok = true;
    LOGI("pmu", "rails up; vbus=%d battery=%d pct=%d charging=%d", s_vbus, s_battery, s_pct, s_charging);
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
        for (int i = 0; i < XPOWERS_AXP2101_INTSTS_CNT; ++i) {
            const uint8_t bits = (uint8_t)(status >> (8 * (XPOWERS_AXP2101_INTSTS_CNT - 1 - i)));
            if (bits) s_pmu.writeRegister(XPOWERS_AXP2101_INTSTS1 + i, bits);
        }
    }
    if (now - s_last_batt_poll >= BATTERY_POLL_MS) {
        s_last_batt_poll = now;
        poll_battery();
    }
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
