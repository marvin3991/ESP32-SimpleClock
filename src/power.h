#pragma once
#include <stdint.h>

// AXP2101 PMU: panel/touch power rails, PWR key, battery.
bool power_init();          // must run before display_init(): powers the panel
void power_tick();
bool power_pwr_pressed();   // one-shot: PWR short press since last call
bool power_ok();
bool power_vbus();          // USB power present
bool power_has_battery();
bool power_charging();
int power_battery_pct();    // -1 if unknown / no battery

// What the supply did since start-up (diag.h). Voltages in mV; 0 = no reading
// (no USB power, no battery).
struct PowerDiag {
    uint16_t vbus_mv, vbus_min_mv, vbus_max_mv;   // USB input now, lowest and highest while present
    uint16_t vsys_mv, vsys_min_mv;                // system rail now, lowest
    uint16_t vbat_mv;
    uint32_t usb_lost, usb_back;                  // USB power removed / back (PMU flags: short dips count)
    uint32_t battery_s;                           // time without USB power
    uint16_t limit_ma, limit_mv;                  // PMU input current limit, input voltage limit
};
void power_diag(PowerDiag* out);
