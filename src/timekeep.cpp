#include "timekeep.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <string.h>
#include <sys/time.h>

#include "config.h"
#include "log.h"
#include "rtc.h"

// Before the first NTP sync the RTC time is trusted for this long before the
// "SYNC" warning appears (Wi-Fi + NTP normally take a few seconds).
static const uint32_t FIRST_SYNC_GRACE_MS = 5UL * 60UL * 1000UL;
// Boot-time alignment to the RTC's seconds tick.
static const uint32_t RTC_ALIGN_TIMEOUT_MS = 1100;
static const uint32_t RTC_ALIGN_POLL_MS = 5;
// The RTC is set in phase with the system clock, so that after a restart the
// system clock (aligned to the RTC tick above) is off by milliseconds, not by
// up to a second. STOP is released this long before the next second. The
// datasheet (Rev. 7.3, section 7.2.1.2) puts the first tick 0.507813..0.507935 s
// after the release, but the RTC on this board ticks 0.500 s after it: with
// the datasheet value the console "rtc" command measured the tick 6 ms early,
// 1/128 s (7.8 ms) minus the read time.
static const long RTC_FIRST_TICK_US = 500000L;
static const long RTC_RELEASE_US = 1000000L - RTC_FIRST_TICK_US;
// The write starts inside this window before the release and busy-waits the
// rest; the loop runs every ~10 ms, so it hits the window within seconds.
static const long RTC_WRITE_WINDOW_US = 30000L;
// The RTC stores the years 2000..2099.
static const time_t RTC_END_EPOCH = utc_from_fields(2100, 1, 1, 0, 0, 0);

static volatile bool s_sync_pending = false;   // set from the lwIP task
static TimeSource s_source = TIME_NONE;
static time_t s_last_sync = 0;
static uint32_t s_sync_count = 0;
static bool s_rtc_ok = false;
static bool s_rtc_pending = false;   // the system time is to be written to the RTC
static bool s_grace_over = false;    // FIRST_SYNC_GRACE_MS has passed (latched: millis() wraps)
static bool s_ntp_started = false;
static char s_tz[64] = TZ_DEFAULT;
// lwIP's SNTP keeps pointers to these names (it does not copy them), so they
// live here and are only rewritten while SNTP is stopped.
static char s_servers[3][64] = {NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3};

static void on_sntp_sync(struct timeval*) { s_sync_pending = true; }

// Earliest plausible time: the firmware build date minus one day (the build
// machine's time zone is unknown). Falls back to TIME_VALID_EPOCH.
static time_t min_valid_epoch() {
    static time_t cached = 0;
    if (cached) return cached;
    static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {};
    int day = 0, year = 0;
    time_t t = TIME_VALID_EPOCH;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) == 3) {
        const char* p = strstr(MONTHS, mon);
        if (p) {
            const time_t built = utc_from_fields(year, (int)(p - MONTHS) / 3 + 1, day, 0, 0, 0) - 86400;
            if (built > t) t = built;
        }
    }
    cached = t;
    return t;
}

void timekeep_set_tz(const char* tz) {
    strncpy(s_tz, tz, sizeof(s_tz) - 1);
    s_tz[sizeof(s_tz) - 1] = 0;
    setenv("TZ", s_tz, 1);
    tzset();
}

void timekeep_init(const char* tz) {
    timekeep_set_tz(tz);
    s_rtc_ok = rtc_init();
    if (s_rtc_ok) {
        time_t utc = 0;
        RtcResult r = rtc_read(&utc);
        if (r == RTC_OK) {
            // The RTC only has whole seconds: wait for the next tick so the
            // system clock starts in phase (error ~poll period, not up to 1 s).
            // A failed read meanwhile keeps the first, unaligned reading.
            const time_t first = utc;
            const uint32_t start = millis();
            while (utc == first && millis() - start < RTC_ALIGN_TIMEOUT_MS) {
                delay(RTC_ALIGN_POLL_MS);
                time_t next;
                if (rtc_read(&next) != RTC_OK) break;
                utc = next;
            }
        }
        if (r == RTC_OK && utc >= min_valid_epoch()) {
            struct timeval tv = {utc, 0};
            settimeofday(&tv, nullptr);
            s_source = TIME_RTC;
            LOGI("time", "loaded from RTC (utc %ld)", (long)utc);
        } else if (r == RTC_OK) {
            LOGI("time", "RTC time %ld is before the firmware build, ignored", (long)utc);
        } else if (r == RTC_I2C_ERROR) {
            s_rtc_ok = false;
            LOGE("time", "RTC read failed");
        } else {
            LOGI("time", "RTC holds no valid time (power was lost or never set)");
        }
    }
    if (s_source == TIME_NONE) {
        // The SoC keeps counting across software/USB resets, so time() may
        // still hold whatever a previous firmware set. Start from zero.
        struct timeval zero = {0, 0};
        settimeofday(&zero, nullptr);
    }
    sntp_set_time_sync_notification_cb(on_sntp_sync);
    sntp_set_sync_interval(NTP_INTERVAL_MS);
}

static const char* server_or_null(int i) { return s_servers[i][0] ? s_servers[i] : nullptr; }

static void start_sntp() {
    configTzTime(s_tz, server_or_null(0), server_or_null(1), server_or_null(2));
    LOGI("time", "SNTP started (%s, %s, %s)", s_servers[0], s_servers[1][0] ? s_servers[1] : "-",
         s_servers[2][0] ? s_servers[2] : "-");
}

void timekeep_set_servers(const char* const servers[3]) {
    if (s_ntp_started) esp_sntp_stop();
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        if (!servers[i] || !servers[i][0]) continue;   // compact: first slot is never empty
        strncpy(s_servers[n], servers[i], sizeof(s_servers[n]) - 1);
        s_servers[n][sizeof(s_servers[n]) - 1] = 0;
        ++n;
    }
    if (n == 0) {   // settings_sanitize() prevents this; keep a working default anyway
        strncpy(s_servers[0], NTP_SERVER_1, sizeof(s_servers[0]) - 1);
        n = 1;
    }
    for (int i = n; i < 3; ++i) s_servers[i][0] = 0;
    if (s_ntp_started) start_sntp();
}

const char* timekeep_server(int i) { return (i >= 0 && i < 3) ? s_servers[i] : ""; }

void timekeep_start_ntp() {
    if (!s_ntp_started) {
        start_sntp();
        s_ntp_started = true;
    } else {
        sntp_restart();   // poll right away after a reconnect
        LOGI("time", "SNTP restarted");
    }
}

// Writes the system time to the RTC once the current second reaches the
// window before RTC_RELEASE_US (see there).
static void rtc_write_in_phase() {
    if (!s_rtc_pending || !s_rtc_ok) return;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_usec < RTC_RELEASE_US - RTC_WRITE_WINDOW_US || tv.tv_usec >= RTC_RELEASE_US) return;
    s_rtc_pending = false;
    const bool written = rtc_write_stopped(tv.tv_sec);
    while (written && tv.tv_usec < RTC_RELEASE_US) gettimeofday(&tv, nullptr);
    const bool released = rtc_release();   // also after a failed write: never leave it stopped
    if (written && released) LOGI("time", "RTC set (utc %ld, in phase)", (long)tv.tv_sec);
    else LOGE("time", "RTC write failed");
}

void timekeep_tick() {
    if (!s_grace_over && millis() > FIRST_SYNC_GRACE_MS) s_grace_over = true;
    rtc_write_in_phase();
    if (!s_sync_pending) return;
    s_sync_pending = false;
    const time_t now = time(nullptr);
    s_last_sync = now;
    ++s_sync_count;
    s_source = TIME_NTP;
    s_rtc_pending = s_rtc_ok;
    struct tm lt;
    localtime_r(&now, &lt);
    LOGI("time", "NTP sync #%lu: %04d-%02d-%02d %02d:%02d:%02d %s", (unsigned long)s_sync_count,
         lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, s_tz);
}

bool timekeep_valid() { return s_source != TIME_NONE && time(nullptr) >= min_valid_epoch(); }

bool timekeep_set_manual(time_t utc) {
    if (utc < min_valid_epoch() || utc >= RTC_END_EPOCH) return false;
    struct timeval tv = {utc, 0};
    settimeofday(&tv, nullptr);
    s_source = TIME_MANUAL;
    s_rtc_pending = s_rtc_ok;
    return true;
}

TimeSource timekeep_source() { return s_source; }
time_t timekeep_last_sync() { return s_last_sync; }
uint32_t timekeep_sync_count() { return s_sync_count; }
bool timekeep_rtc_ok() { return s_rtc_ok; }

bool timekeep_stale(time_t now) {
    if (s_last_sync == 0) return s_grace_over;
    return now - s_last_sync > SYNC_STALE_S;
}

const char* timekeep_source_name() {
    switch (s_source) {
        case TIME_RTC: return "RTC";
        case TIME_NTP: return "NTP";
        case TIME_MANUAL: return "MANUAL";
        default: return "NONE";
    }
}
