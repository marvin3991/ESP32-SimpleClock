#include "settings.h"

#include <Preferences.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "wifi_default.h"   // generated from .env by scripts/pio_env.py

Settings g_settings;

static const char* NS = "clock";
static const uint8_t SCHEMA = 1;   // bump when the stored layout changes

static uint32_t s_generation = 1;
static bool s_brightness_dirty = false;
static uint32_t s_brightness_changed_ms = 0;

// Fingerprint of the Wi-Fi defaults compiled in from .env (0 = none). Stored
// in NVS so that a re-flash with a *changed* .env wins over credentials saved
// earlier from the web page, while an unchanged .env does not undo web edits.
static uint32_t env_signature() {
    if (!WIFI_DEFAULT_SSID[0]) return 0;
    uint32_t h = 2166136261u;   // FNV-1a
    for (const char* p = WIFI_DEFAULT_SSID; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ 0xFFu) * 16777619u;   // separator
    for (const char* p = WIFI_DEFAULT_PASSWORD; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    return h ? h : 1;
}

static void set_default_ntp(Settings& s) {
    const char* const def[NTP_SERVERS] = {NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3};
    for (int i = 0; i < NTP_SERVERS; ++i) {
        strncpy(s.ntp[i], def[i], sizeof(s.ntp[i]) - 1);
        s.ntp[i][sizeof(s.ntp[i]) - 1] = 0;
    }
}

static void defaults(Settings& s) {
    memset(&s, 0, sizeof(s));
    strncpy(s.ssid, WIFI_DEFAULT_SSID, sizeof(s.ssid) - 1);
    strncpy(s.pass, WIFI_DEFAULT_PASSWORD, sizeof(s.pass) - 1);
    strncpy(s.tz, TZ_DEFAULT, sizeof(s.tz) - 1);
    s.h12 = false;
    s.show_date = true;
    s.night_enabled = NIGHT_DEFAULT_ENABLED;
    s.night_start = NIGHT_DEFAULT_START_MIN;
    s.night_end = NIGHT_DEFAULT_END_MIN;
    s.level_day = BRIGHTNESS_DAY_DEFAULT;
    s.level_night = BRIGHTNESS_NIGHT_DEFAULT;
    s.rotation = ROTATION_AUTO;
    s.sleep_enabled = SLEEP_DEFAULT_ENABLED;
    s.sleep_start = SLEEP_DEFAULT_START_MIN;
    s.sleep_end = SLEEP_DEFAULT_END_MIN;
    s.swap_hourly = SWAP_DEFAULT_ENABLED;
    set_default_ntp(s);
}

// Clamp anything out of range (corrupt NVS, older firmware) back to sane values.
static void sanitize(Settings& s) {
    s.ssid[sizeof(s.ssid) - 1] = 0;
    s.pass[sizeof(s.pass) - 1] = 0;
    s.tz[sizeof(s.tz) - 1] = 0;
    if (!s.tz[0]) strncpy(s.tz, TZ_DEFAULT, sizeof(s.tz) - 1);
    if (s.night_start >= 24 * 60) s.night_start = NIGHT_DEFAULT_START_MIN;
    if (s.night_end >= 24 * 60) s.night_end = NIGHT_DEFAULT_END_MIN;
    if (s.level_day >= BRIGHTNESS_LEVELS) s.level_day = BRIGHTNESS_DAY_DEFAULT;
    if (s.level_night >= BRIGHTNESS_LEVELS) s.level_night = BRIGHTNESS_NIGHT_DEFAULT;
    if (s.rotation > ROTATION_AUTO) s.rotation = ROTATION_AUTO;
    if (s.sleep_start >= 24 * 60) s.sleep_start = SLEEP_DEFAULT_START_MIN;
    if (s.sleep_end >= 24 * 60) s.sleep_end = SLEEP_DEFAULT_END_MIN;
    bool any_ntp = false;
    for (auto& host : s.ntp) {
        host[sizeof(host) - 1] = 0;
        any_ntp |= host[0] != 0;
    }
    if (!any_ntp) set_default_ntp(s);   // never end up without a time source
}

void settings_load() {
    defaults(g_settings);
    Preferences p;
    if (!p.begin(NS, false)) {   // read-write: creates the namespace on first boot
        LOGE("cfg", "NVS unavailable, using defaults");
        return;
    }
    if (p.getUChar("schema", 0) == SCHEMA) {
        // isKey() first: getString() on a missing key logs a core error.
        if (p.isKey("ssid")) {
            String v = p.getString("ssid", "");
            if (v.length()) {
                strncpy(g_settings.ssid, v.c_str(), sizeof(g_settings.ssid) - 1);
                strncpy(g_settings.pass, p.getString("pass", "").c_str(), sizeof(g_settings.pass) - 1);
            }
        }
        if (p.isKey("tz"))
            strncpy(g_settings.tz, p.getString("tz", TZ_DEFAULT).c_str(), sizeof(g_settings.tz) - 1);
        g_settings.h12 = p.getBool("h12", g_settings.h12);
        g_settings.show_date = p.getBool("date", g_settings.show_date);
        g_settings.night_enabled = p.getBool("night", g_settings.night_enabled);
        g_settings.night_start = p.getUShort("nstart", g_settings.night_start);
        g_settings.night_end = p.getUShort("nend", g_settings.night_end);
        g_settings.level_day = p.getUChar("lday", g_settings.level_day);
        g_settings.level_night = p.getUChar("lnight", g_settings.level_night);
        g_settings.rotation = p.getUChar("rot", g_settings.rotation);
        g_settings.sleep_enabled = p.getBool("sleep", g_settings.sleep_enabled);
        g_settings.sleep_start = p.getUShort("sstart", g_settings.sleep_start);
        g_settings.sleep_end = p.getUShort("send", g_settings.sleep_end);
        g_settings.swap_hourly = p.getBool("swap", g_settings.swap_hourly);
        for (int i = 0; i < NTP_SERVERS; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "ntp%d", i + 1);
            if (p.isKey(key))
                strncpy(g_settings.ntp[i], p.getString(key, "").c_str(), sizeof(g_settings.ntp[i]) - 1);
        }
    } else {
        LOGI("cfg", "settings schema mismatch, using defaults");
    }
    const uint32_t stored_sig = p.getULong("envsig", 0);
    p.end();

    const uint32_t sig = env_signature();
    if (sig && sig != stored_sig) {
        // New .env since the last boot: it is the most recent change, use it.
        strncpy(g_settings.ssid, WIFI_DEFAULT_SSID, sizeof(g_settings.ssid) - 1);
        strncpy(g_settings.pass, WIFI_DEFAULT_PASSWORD, sizeof(g_settings.pass) - 1);
        Preferences w;
        if (w.begin(NS, false)) {
            w.putUChar("schema", SCHEMA);
            w.putString("ssid", g_settings.ssid);
            w.putString("pass", g_settings.pass);
            w.putULong("envsig", sig);
            w.end();
        }
        LOGI("cfg", "Wi-Fi taken from the new .env build");
    }
    sanitize(g_settings);
    LOGI("cfg",
         "loaded: wifi=%s tz=%s h12=%d date=%d night=%d %02u:%02u-%02u:%02u lvl=%u/%u rot=%u "
         "sleep=%d %02u:%02u-%02u:%02u swap=%d",
         g_settings.ssid[0] ? "set" : "none", g_settings.tz, g_settings.h12, g_settings.show_date,
         g_settings.night_enabled, g_settings.night_start / 60, g_settings.night_start % 60,
         g_settings.night_end / 60, g_settings.night_end % 60, g_settings.level_day,
         g_settings.level_night, g_settings.rotation, g_settings.sleep_enabled, g_settings.sleep_start / 60,
         g_settings.sleep_start % 60, g_settings.sleep_end / 60, g_settings.sleep_end % 60,
         g_settings.swap_hourly);
    LOGI("cfg", "ntp: \"%s\" \"%s\" \"%s\"", g_settings.ntp[0], g_settings.ntp[1], g_settings.ntp[2]);
}

bool settings_save() {
    sanitize(g_settings);
    Preferences p;
    if (!p.begin(NS, false)) {
        LOGE("cfg", "NVS open failed");
        return false;
    }
    bool ok = true;
    ok &= p.putUChar("schema", SCHEMA) == 1;
    ok &= p.putString("ssid", g_settings.ssid) == strlen(g_settings.ssid);
    ok &= p.putString("pass", g_settings.pass) == strlen(g_settings.pass);
    ok &= p.putString("tz", g_settings.tz) == strlen(g_settings.tz);
    ok &= p.putBool("h12", g_settings.h12) == 1;
    ok &= p.putBool("date", g_settings.show_date) == 1;
    ok &= p.putBool("night", g_settings.night_enabled) == 1;
    ok &= p.putUShort("nstart", g_settings.night_start) == 2;
    ok &= p.putUShort("nend", g_settings.night_end) == 2;
    ok &= p.putUChar("lday", g_settings.level_day) == 1;
    ok &= p.putUChar("lnight", g_settings.level_night) == 1;
    ok &= p.putUChar("rot", g_settings.rotation) == 1;
    ok &= p.putBool("sleep", g_settings.sleep_enabled) == 1;
    ok &= p.putUShort("sstart", g_settings.sleep_start) == 2;
    ok &= p.putUShort("send", g_settings.sleep_end) == 2;
    ok &= p.putBool("swap", g_settings.swap_hourly) == 1;
    for (int i = 0; i < NTP_SERVERS; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "ntp%d", i + 1);
        ok &= p.putString(key, g_settings.ntp[i]) == strlen(g_settings.ntp[i]);
    }
    p.end();
    s_brightness_dirty = false;
    settings_touch();
    if (!ok) LOGE("cfg", "NVS write incomplete");
    else LOGI("cfg", "saved");
    return ok;
}

void settings_save_brightness_later() {
    s_brightness_dirty = true;
    s_brightness_changed_ms = millis();
}

void settings_tick() {
    if (!s_brightness_dirty || millis() - s_brightness_changed_ms < BRIGHTNESS_SAVE_DELAY_MS) return;
    s_brightness_dirty = false;
    Preferences p;
    if (!p.begin(NS, false)) {
        LOGE("cfg", "NVS open failed (brightness)");
        return;
    }
    // Writing the schema too makes a first-boot brightness change persist.
    p.putUChar("schema", SCHEMA);
    p.putUChar("lday", g_settings.level_day);
    p.putUChar("lnight", g_settings.level_night);
    p.putUChar("rot", g_settings.rotation);
    p.end();
    LOGI("cfg", "brightness/rotation saved (day %u, night %u, rot %u)", g_settings.level_day,
         g_settings.level_night, g_settings.rotation);
}

void settings_factory_reset() {
    Preferences p;
    if (p.begin(NS, false)) {
        p.clear();
        p.end();
    }
    defaults(g_settings);
    settings_touch();
    LOGI("cfg", "factory reset");
}

bool settings_has_wifi() { return g_settings.ssid[0] != 0; }
uint32_t settings_generation() { return s_generation; }
void settings_touch() { ++s_generation; }
