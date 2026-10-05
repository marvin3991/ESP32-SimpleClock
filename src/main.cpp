// ESP32 AMOLED Clock - Waveshare ESP32-C6-Touch-AMOLED-2.16
#include <Arduino.h>
#include <Wire.h>

#include "app.h"
#include "board.h"
#include "config.h"
#include "console.h"
#include "input.h"
#include "log.h"
#include "net.h"
#include "orient.h"
#include "power.h"
#include "settings.h"
#include "timekeep.h"

volatile bool g_log_mute = false;

void setup() {
    // HWCDC drops output by itself when no USB host is attached (wall
    // charger), so logging never blocks the clock. Do not set the TX timeout
    // to 0: in core 3.3.8 that marks the port disconnected after one write.
    Serial.begin(115200);
    delay(200);
    LOGI("main", "ESP32 AMOLED Clock %s", FW_VERSION);

    settings_load();
    Wire.begin(I2C_SDA, I2C_SCL);
    power_init();          // powers and resets the panel: must precede the display
    orient_init();         // boot orientation for the first frame
    timekeep_init(g_settings.tz);
    const char* const servers[3] = {g_settings.ntp[0], g_settings.ntp[1], g_settings.ntp[2]};
    timekeep_set_servers(servers);
    app_init();            // display + first frame (drawn in app_tick)
    input_init();
    net_init();
    LOGI("main", "ready, free heap %lu", (unsigned long)ESP.getFreeHeap());
}

void loop() {
    power_tick();
    input_tick();
    orient_tick();
    net_loop();
    timekeep_tick();
    app_tick();
    settings_tick();
    console_tick();
    delay(LOOP_IDLE_MS);
}
