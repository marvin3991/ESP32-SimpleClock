// ESP32 AMOLED Clock - Waveshare ESP32-C6-Touch-AMOLED-2.16
#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>

#include "app.h"
#include "board.h"
#include "config.h"
#include "console.h"
#include "diag.h"
#include "input.h"
#include "log.h"
#include "net.h"
#include "orient.h"
#include "power.h"
#include "settings.h"
#include "timekeep.h"

volatile bool g_log_mute = false;

static const char* reset_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "POWER ON";
        case ESP_RST_EXT: return "RESET PIN";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "CRASH";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return "WATCHDOG";
        case ESP_RST_DEEPSLEEP: return "DEEP SLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_PWR_GLITCH: return "POWER GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU LOCKUP";
        default: return "OTHER";
    }
}

void setup() {
    // HWCDC drops output by itself when no USB host is attached (wall
    // charger). A host that is attached but not reading would block every
    // log line for up to 2 s with the default timeout (see config.h).
    Serial.setTxTimeoutMs(SERIAL_TX_TIMEOUT_MS);
    Serial.begin(115200);
    delay(200);
    LOGI("main", "ESP32 AMOLED Clock %s", FW_VERSION);
    diag_init(reset_name(esp_reset_reason()));
    LOGI("main", "last reset: %s", reset_name(esp_reset_reason()));

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
    const uint32_t start = millis();
    power_tick();
    input_tick();
    orient_tick();
    net_loop();
    timekeep_tick();
    app_tick();
    settings_tick();
    console_tick();
    diag_loop_ms(millis() - start);
    delay(LOOP_IDLE_MS);
}
