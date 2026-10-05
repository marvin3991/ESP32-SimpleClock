#include "app.h"

#include <Arduino.h>
#include <string.h>
#include <sys/time.h>

#include "config.h"
#include "display.h"
#include "log.h"
#include "net.h"
#include "orient.h"
#include "power.h"
#include "settings.h"
#include "timekeep.h"

static const uint8_t DBV[BRIGHTNESS_LEVELS] = BRIGHTNESS_TABLE;
static const char* const QUADRANT_NAME[4] = {"USB DOWN", "USB LEFT", "USB UP", "USB RIGHT"};

// A state that lasts `len` ms from `start` (status page, toast, peek). The
// elapsed time is compared unsigned, which stays right across the millis()
// wrap, and an expired timer switches itself off. (A stored deadline compared
// by signed difference turns "active" again 2^31 ms, ~24.9 days, after it
// was set, or for any deadline of 0 once the uptime passes 2^31 ms.)
struct Timer {
    bool on = false;
    uint32_t start = 0, len = 0;
    void run(uint32_t ms) {
        on = true;
        start = millis();
        len = ms;
    }
    void stop() { on = false; }
    bool active() {
        if (on && millis() - start >= len) on = false;
        return on;
    }
};

static const uint32_t MS_PER_HOUR = 60UL * 60UL * 1000UL;
// The clock going back by at most this much while UTC runs on normally is the
// end of daylight saving time (shifts in use are 30 or 60 min).
static const int CLOCK_BACK_HOLD_MAX_MIN = 120;
// A UTC step larger than this between two ticks is a time change (NTP, manual).
static const long CLOCK_JUMP_S = 120;

static FaceState s_face;
static bool s_display_ok = false;
static bool s_screen_on = true;
static bool s_need_full = true;
static Timer s_status;                // status page shown
static Timer s_toast;
static Timer s_setup_bright;          // setup page at day brightness after an input
static uint8_t s_swallow = 0;         // buttons whose current press only woke the screen
static uint8_t s_quadrant = 0;
static uint8_t s_shift_idx = 0;
static uint32_t s_shift_at = 0;       // millis() of the last pixel-shift step
static int s_last_minute = -1;
static uint32_t s_settings_gen = 0;

// Local minute of the day the night and screen-off windows use; -1 while the
// time is not valid.
static int s_window_min = -1;
static time_t s_window_utc = 0;

// Screen-off schedule
static bool s_first_tick = true;
static bool s_in_sleep = false;       // inside the schedule window (last tick)
static Timer s_peek;                  // inside the window: screen on while active

// Brightness fade
static uint8_t s_dbv_now = 0, s_dbv_from = 0, s_dbv_target = 0;
static uint32_t s_ramp_start = 0;
static bool s_ramping = false;

// ------------------------------------------------------------------ helpers

// Closed loop over a SHIFT_GRID x SHIFT_GRID grid with 1 px steps: row 0
// left->right, then snake through columns N-1..1 (rows 1..N-1), then up
// column 0 back to the start. N must be even for the loop to close.
static void shift_offset(uint8_t idx, int8_t* dx, int8_t* dy) {
    const int n = SHIFT_GRID;
    int k = idx % (n * n), x, y;
    if (k < n) {
        x = k;
        y = 0;
    } else if ((k -= n) < (n - 1) * (n - 1)) {
        const int col = k / (n - 1), pos = k % (n - 1);
        x = n - 1 - col;
        y = (col % 2 == 0) ? 1 + pos : n - 1 - pos;
    } else {
        k -= (n - 1) * (n - 1);
        x = 0;
        y = n - 1 - k;
    }
    *dx = (int8_t)(x - n / 2);
    *dy = (int8_t)(y - n / 2);
}

// When the clock goes back (end of daylight saving time) the same local hour
// runs twice. The windows then keep the latest minute already seen until the
// clock catches up, so a window boundary inside that hour fires only once.
static void update_window_minute(time_t utc, const struct tm& lt, bool valid) {
    const long step = (long)(utc - s_window_utc);   // seconds since the last tick
    s_window_utc = utc;
    if (!valid) {
        s_window_min = -1;
        return;
    }
    const int m = lt.tm_hour * 60 + lt.tm_min;
    const int back = (s_window_min - m + 24 * 60) % (24 * 60);   // minutes the local clock went back
    const bool replay = s_window_min >= 0 && back > 0 && back <= CLOCK_BACK_HOLD_MAX_MIN && step >= 0 &&
                        step < CLOCK_JUMP_S;
    if (!replay) s_window_min = m;
}

// [start, end) in minutes after midnight; may wrap past midnight; start == end
// means "never". m < 0 (no valid time) is never inside.
static bool in_window(int m, uint16_t start, uint16_t end) {
    if (m < 0 || start == end) return false;
    return start < end ? (m >= start && m < end) : (m >= start || m < end);
}

static bool in_night() {
    return g_settings.night_enabled && in_window(s_window_min, g_settings.night_start, g_settings.night_end);
}

static bool in_sleep() {
    return g_settings.sleep_enabled && in_window(s_window_min, g_settings.sleep_start, g_settings.sleep_end);
}

static uint8_t current_level() { return in_night() ? g_settings.level_night : g_settings.level_day; }

static void step_shift() {
    s_shift_idx = (uint8_t)((s_shift_idx + 1) % (SHIFT_GRID * SHIFT_GRID));
    s_shift_at = millis();
}

static uint8_t wanted_quadrant() {
    if (g_settings.rotation != ROTATION_AUTO) return g_settings.rotation & 3;
    return orient_ok() ? orient_quadrant() : s_quadrant;
}

static void set_brightness_now(uint8_t dbv) {
    s_ramping = false;
    s_dbv_now = s_dbv_target = dbv;
    display_set_brightness(dbv);
}

static void ramp_to(uint8_t dbv) {
    if (dbv == s_dbv_target) return;
    s_dbv_from = s_dbv_now;
    s_dbv_target = dbv;
    s_ramp_start = millis();
    s_ramping = true;
}

static void ramp_tick() {
    if (!s_ramping) return;
    const uint32_t t = millis() - s_ramp_start;
    uint8_t v = s_dbv_target;
    if (t < BRIGHTNESS_RAMP_MS)
        v = (uint8_t)(s_dbv_from + ((int)s_dbv_target - (int)s_dbv_from) * (int)t / BRIGHTNESS_RAMP_MS);
    else
        s_ramping = false;
    if (v != s_dbv_now) {
        s_dbv_now = v;
        display_set_brightness(v);
    }
}

static void toast_text(const char* text) {
    s_face.toast = TOAST_TEXT;
    strncpy(s_face.toast_text, text, sizeof(s_face.toast_text) - 1);
    s_face.toast_text[sizeof(s_face.toast_text) - 1] = 0;
    s_toast.run(TOAST_MS);
}

// ------------------------------------------------------------------ actions

static void screen_off() {
    s_screen_on = false;
    set_brightness_now(0);
    display_set_on(false);
    s_face.toast = TOAST_NONE;
    s_status.stop();
    LOGI("app", "screen off");
}

static void screen_on() {
    s_screen_on = true;
    s_need_full = true;   // redraw first, then fade in (see app_tick)
    display_set_on(true);
    LOGI("app", "screen on");
}

static void step_level(int delta) {
    const bool night = in_night();
    uint8_t& level = night ? g_settings.level_night : g_settings.level_day;
    const int v = constrain((int)level + delta, 0, BRIGHTNESS_LEVELS - 1);
    level = (uint8_t)v;
    s_face.toast = TOAST_BRIGHTNESS;
    s_face.toast_level = (uint8_t)(v + 1);
    s_face.toast_night = night;
    s_toast.run(TOAST_MS);
    settings_save_brightness_later();
    LOGI("app", "%s brightness level %d/%d (DBV %u)", night ? "night" : "day", v + 1, BRIGHTNESS_LEVELS, DBV[v]);
}

void app_set_rotation_mode(uint8_t mode) {
    g_settings.rotation = mode > ROTATION_AUTO ? ROTATION_AUTO : mode;
    settings_save_brightness_later();   // same debounced write covers rotation
    toast_text(g_settings.rotation == ROTATION_AUTO ? "ROTATE AUTO" : QUADRANT_NAME[g_settings.rotation]);
    LOGI("app", "rotation mode %u", g_settings.rotation);
}

static void cycle_rotation() {
    const uint8_t m = g_settings.rotation;
    app_set_rotation_mode(m == ROTATION_AUTO ? 0 : (m >= 3 ? ROTATION_AUTO : m + 1));
}

// Leaving setup is always allowed: without Wi-Fi the clock still runs from
// the RTC (or shows "--" until the time is set).
static void toggle_setup() {
    if (net_in_setup()) net_stop_setup();
    else net_start_setup();
}

void app_set_level(uint8_t level) {
    step_level((int)level - (int)current_level());
}

void app_show_page(Page p) {
    if (p == PAGE_SETUP) {
        s_setup_bright.run(SETUP_DIM_AFTER_MS);
        if (!net_in_setup()) net_start_setup();
        return;
    }
    if (net_in_setup()) net_stop_setup();
    if (p == PAGE_STATUS) s_status.run(STATUS_PAGE_MS);
    else s_status.stop();
}

void app_handle(const InputEvent& ev) {
    static const char* const KIND[] = {"press", "short", "long"};
    LOGI("app", "input %s %s%s", input_button_name(ev.button), KIND[ev.kind], s_screen_on ? "" : " (wake)");
    const uint8_t bit = (uint8_t)(1u << ev.button);
    s_peek.run(SLEEP_PEEK_MS);   // inside the off-schedule: stay on a while
    s_setup_bright.run(SETUP_DIM_AFTER_MS);
    if (ev.kind == IN_PRESS) s_swallow &= (uint8_t)~bit;   // a new press starts clean
    if (!s_screen_on) {
        // Any button or tap wakes the screen and is not acted on otherwise.
        if (ev.kind == IN_PRESS) s_swallow |= bit;
        screen_on();
        return;
    }
    if (s_swallow & bit) return;
    switch (ev.button) {
        case BTN_KEY:
            if (ev.kind == IN_SHORT) step_level(+1);
            else if (ev.kind == IN_LONG) cycle_rotation();
            break;
        case BTN_BOOT:
            if (ev.kind == IN_SHORT) step_level(-1);
            else if (ev.kind == IN_LONG) toggle_setup();
            break;
        case BTN_PWR:
            if (ev.kind == IN_SHORT) screen_off();
            break;
        case BTN_TOUCH:
            if (ev.kind == IN_SHORT && !net_in_setup()) {
                if (s_status.active()) s_status.stop();
                else s_status.run(STATUS_PAGE_MS);
            }
            break;
        default:
            break;
    }
}

// ------------------------------------------------------------------ pages

static void fill_status(const struct tm& lt, bool valid) {
    enum { L_DATE, L_TIME, L_WIFI, L_IP, L_SYNC, L_RTC, L_BATT, L_ROT, L_FW };
    static_assert(L_FW < STATUS_LINES, "status lines");
    char (*l)[STATUS_LINE_LEN] = s_face.status;
    memset(s_face.status, 0, sizeof(s_face.status));
    s_face.status_hint[0] = 0;
    if (valid) {
        strftime(l[L_DATE], STATUS_LINE_LEN, "DATE\t%Y-%m-%d %a", &lt);
        strftime(l[L_TIME], STATUS_LINE_LEN, "TIME\t%H:%M:%S", &lt);
    } else {
        snprintf(l[L_DATE], STATUS_LINE_LEN, "DATE\t-");
        snprintf(l[L_TIME], STATUS_LINE_LEN, "TIME\tNOT SET");
    }
    if (net_connected())
        snprintf(l[L_WIFI], STATUS_LINE_LEN, "WIFI\t%d dBm  %s", net_rssi(), g_settings.ssid);
    else if (settings_has_wifi())
        snprintf(l[L_WIFI], STATUS_LINE_LEN, "WIFI\tOFFLINE, %s", net_last_error());
    else
        snprintf(l[L_WIFI], STATUS_LINE_LEN, "WIFI\tNOT SET");
    snprintf(l[L_IP], STATUS_LINE_LEN, "IP\t%s", net_connected() ? net_ip() : "-");
    const time_t ls = timekeep_last_sync();
    if (ls) {
        struct tm st;
        localtime_r(&ls, &st);
        char when[20];
        strftime(when, sizeof(when), "%m-%d %H:%M", &st);
        snprintf(l[L_SYNC], STATUS_LINE_LEN, "SYNC\t%s NTP x%lu", when, (unsigned long)timekeep_sync_count());
    } else {
        snprintf(l[L_SYNC], STATUS_LINE_LEN, "SYNC\tNEVER, FROM %s", timekeep_source_name());
    }
    snprintf(l[L_RTC], STATUS_LINE_LEN, "RTC\t%s", timekeep_rtc_ok() ? "OK" : "ERROR");
    if (power_has_battery())
        snprintf(l[L_BATT], STATUS_LINE_LEN, "BATT\t%d%%%s%s", power_battery_pct(),
                 power_charging() ? " CHARGING" : "", power_vbus() ? " + USB" : "");
    else
        snprintf(l[L_BATT], STATUS_LINE_LEN, "BATT\tNONE, USB");
    snprintf(l[L_ROT], STATUS_LINE_LEN, "ROT\t%s%s", g_settings.rotation == ROTATION_AUTO ? "AUTO, " : "",
             QUADRANT_NAME[s_quadrant]);
    snprintf(l[L_FW], STATUS_LINE_LEN, "FW\t%s", FW_VERSION);
    if (!settings_has_wifi()) snprintf(s_face.status_hint, STATUS_LINE_LEN, "HOLD BOOT 3 S FOR WI-FI");
    else if (net_connected()) snprintf(s_face.status_hint, STATUS_LINE_LEN, "http://%s.local", HOSTNAME);
}

static void fill_setup() {
    strncpy(s_face.ap_ssid, net_ap_ssid(), sizeof(s_face.ap_ssid) - 1);
    strncpy(s_face.ap_pass, net_ap_pass(), sizeof(s_face.ap_pass) - 1);
    strncpy(s_face.ap_ip, net_ap_ip(), sizeof(s_face.ap_ip) - 1);
    strncpy(s_face.setup_msg, net_setup_message(), sizeof(s_face.setup_msg) - 1);
    const SetupPhase p = net_setup_phase();
    s_face.setup_msg_color = p == SETUP_OK ? COLOR_OK : (p == SETUP_FAIL ? COLOR_WARN : 0);
}

// ------------------------------------------------------------------ main

void app_init() {
    face_init();
    s_quadrant = wanted_quadrant();
    s_display_ok = display_init(s_quadrant);   // panel starts at brightness 0
    s_settings_gen = settings_generation();
    s_need_full = true;
    s_setup_bright.run(SETUP_DIM_AFTER_MS);   // the setup page may open at boot
}

void app_tick() {
    if (settings_generation() != s_settings_gen) {
        s_settings_gen = settings_generation();
        s_need_full = true;   // 12/24 h, date, ... may have changed
        s_window_min = -1;    // a new time zone is not a daylight saving change
    }

    InputEvent ev;
    while (input_next(&ev)) app_handle(ev);

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm lt;
    localtime_r(&tv.tv_sec, &lt);
    const bool valid = timekeep_valid();
    update_window_minute(tv.tv_sec, lt, valid);

    // Pixel shift: one step per minute change (the minutes redraw then anyway).
    // Without a valid time the "--" would never move, so it steps once per
    // minute of uptime instead.
    if (valid) {
        if (s_last_minute >= 0 && lt.tm_min != s_last_minute) step_shift();
        s_last_minute = lt.tm_min;
    } else if (millis() - s_shift_at >= SHIFT_NO_TIME_STEP_MS) {
        step_shift();
    }

    // Screen-off schedule. Only the window edges switch the panel, so a manual
    // PWR press in between is respected; inside the window a press or tap
    // shows the clock for SLEEP_PEEK_MS. Never during Wi-Fi setup.
    const bool sleeping = in_sleep() && !net_in_setup();
    if (s_first_tick) {
        s_first_tick = false;
        s_in_sleep = sleeping;
        s_peek.run(SLEEP_PEEK_MS);   // powered on inside the window: show it briefly
    } else if (sleeping != s_in_sleep) {
        s_in_sleep = sleeping;
        LOGI("app", "off-schedule %s (%02u:%02u-%02u:%02u)", sleeping ? "starts" : "ends",
             g_settings.sleep_start / 60, g_settings.sleep_start % 60, g_settings.sleep_end / 60,
             g_settings.sleep_end % 60);
        if (sleeping && s_screen_on) screen_off();
        else if (!sleeping && !s_screen_on) screen_on();
        s_peek.stop();
    }
    const bool peeking = s_peek.active();
    if (sleeping && s_screen_on && !peeking) {
        LOGI("app", "off-schedule: peek over");
        screen_off();
    }

    // Rotation: blank, switch the panel scan direction, redraw, fade back in.
    const uint8_t q = wanted_quadrant();
    if (q != s_quadrant) {
        s_quadrant = q;
        set_brightness_now(0);
        display_set_quadrant(q);
        s_need_full = true;
        LOGI("app", "rotate to %s", QUADRANT_NAME[q]);
    }

    if (!s_toast.active()) s_face.toast = TOAST_NONE;

    s_face.page = net_in_setup() ? PAGE_SETUP : (s_status.active() ? PAGE_STATUS : PAGE_CLOCK);
    s_face.time_valid = valid;
    s_face.local = lt;
    s_face.h12 = g_settings.h12;
    s_face.show_date = g_settings.show_date;
    s_face.sync_stale = valid && timekeep_stale(tv.tv_sec);
    s_face.battery_low = power_has_battery() && !power_vbus() && power_battery_pct() >= 0 &&
                         power_battery_pct() <= BATTERY_LOW_PCT;
    shift_offset(s_shift_idx, &s_face.shift_dx, &s_face.shift_dy);
    // Odd hours put the side column left; without a valid time, odd hours of uptime.
    const bool odd_hour = valid ? lt.tm_hour % 2 == 1 : (millis() / MS_PER_HOUR) % 2 == 1;
    s_face.side_left = g_settings.swap_hourly && odd_hour;
    if (s_face.page == PAGE_STATUS) fill_status(lt, valid);
    if (s_face.page == PAGE_SETUP) fill_setup();
    // The setup page can stay up for days (no Wi-Fi and no time to show): when
    // nobody has touched the clock for a while it drops to the night level.
    const bool setup_idle = !s_setup_bright.active() && s_face.page == PAGE_SETUP;

    if (s_screen_on && s_display_ok) {
        face_update(s_face, s_need_full);
        s_need_full = false;
        ramp_to(DBV[in_night() || setup_idle ? g_settings.level_night : g_settings.level_day]);
        ramp_tick();
    }
}

void app_screenshot() { face_screenshot(); }

void app_print_status() {
    const time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    Serial.printf("time     %s (%s, valid=%d, tz=%s)\n", buf, timekeep_source_name(), timekeep_valid(),
                  g_settings.tz);
    Serial.printf("sync     count=%lu last=%ld stale=%d servers=%s %s %s\n", (unsigned long)timekeep_sync_count(),
                  (long)timekeep_last_sync(), timekeep_stale(now), timekeep_server(0), timekeep_server(1),
                  timekeep_server(2));
    Serial.printf("wifi     ssid=\"%s\" connected=%d ip=%s rssi=%d err=%s setup=%d phase=%u drops=%lu\n",
                  g_settings.ssid, net_connected(), net_ip(), net_rssi(), net_last_error(), net_in_setup(),
                  net_setup_phase(), (unsigned long)net_disconnects());
    Serial.printf("screen   on=%d page=%u dbv=%u level=%u/%u night=%d quadrant=%u rotmode=%u shift=%d,%d side=%s\n",
                  s_screen_on, s_face.page, s_dbv_now, current_level() + 1, BRIGHTNESS_LEVELS, in_night(),
                  s_quadrant, g_settings.rotation, s_face.shift_dx, s_face.shift_dy,
                  s_face.side_left ? "left" : "right");
    Serial.printf("schedule off=%d %02u:%02u-%02u:%02u inside=%d swap=%d\n", g_settings.sleep_enabled,
                  g_settings.sleep_start / 60, g_settings.sleep_start % 60, g_settings.sleep_end / 60,
                  g_settings.sleep_end % 60, s_in_sleep, g_settings.swap_hourly);
    Serial.printf("power    vbus=%d battery=%d pct=%d charging=%d pmu=%d\n", power_vbus(), power_has_battery(),
                  power_battery_pct(), power_charging(), power_ok());
    Serial.printf("system   fw=%s heap=%lu min_heap=%lu uptime=%lus\n", FW_VERSION,
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
                  (unsigned long)(millis() / 1000));
}
