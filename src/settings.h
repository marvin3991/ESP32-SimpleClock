#pragma once
#include <stdint.h>

static const uint8_t ROTATION_AUTO = 4;   // 0..3 = fixed quadrant, 4 = IMU
static const int NTP_SERVERS = 3;         // what the SNTP client supports (CONFIG_LWIP_SNTP_MAX_SERVERS)

struct Settings {
    char ssid[33];
    char pass[65];
    char tz[64];             // POSIX TZ string, e.g. "CST-8"
    bool h12;                // 12-hour display
    bool show_date;          // weekday + day next to the hours
    bool night_enabled;
    uint16_t night_start;    // minutes after midnight
    uint16_t night_end;
    uint8_t level_day;       // brightness index 0..BRIGHTNESS_LEVELS-1
    uint8_t level_night;
    uint8_t rotation;        // 0..3 or ROTATION_AUTO
    bool sleep_enabled;      // scheduled screen-off window
    uint16_t sleep_start;    // minutes after midnight
    uint16_t sleep_end;
    bool swap_hourly;        // side column changes sides every hour
    char ntp[NTP_SERVERS][64];  // time servers, tried in order; "" = unused
};

extern Settings g_settings;

void settings_load();
bool settings_save();                 // everything except brightness
void settings_save_brightness_later(); // debounced brightness write
void settings_tick();                 // performs the debounced write
void settings_factory_reset();
bool settings_has_wifi();
// Bumped whenever settings change at runtime, so the app can re-apply them.
uint32_t settings_generation();
void settings_touch();
