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
