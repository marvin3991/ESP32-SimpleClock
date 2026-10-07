#include "timekeep.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>

#include "config.h"
#include "log.h"
#include "net.h"
#include "ntp.h"
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

// RTC drift. The RTC's crystal on this board runs about 85 ppm slow (some 7 s
// a day; with Offset -21 it measured +2.4 ppm against NTP over an hour, +3.1
// ppm against a Mac's clock over 58 min). At every NTP sync the RTC's error
// against the fresh system clock is measured; its change since the RTC was
// last set in phase gives the rate error, which the Offset register corrects
// (fast mode, see rtc_set_offset()). The setting is kept in NVS so it
// survives a power loss of the RTC.
static const double RTC_PPM_PER_STEP = 4.069;
static const int RTC_STEP_MIN = -64, RTC_STEP_MAX = 63;
// What the two measurements of a rate can be off by: one round of correction
// pulses (1/1024 s per step, every 4 minutes, so the error swings by up to
// |steps| / 1024 s around the average rate), the error bound of each sync
// (ntp_error_bound_us(): half the round trip to the server and on to its
// reference) and the tick reads (each RTC read moves 11 bytes over I2C at
// 100 kHz, about 1 ms). The Offset only changes when the rate is off by more
// than half a step plus that.
static const double RTC_PULSE_S = 1.0 / 1024;
static const double RTC_TICK_READ_ERR_S = 0.001;
// A sync vouches for the system clock only this long: the ESP32's own clock
// drifts (here 14.3 and 13.8 ms an hour between syncs, -4 ppm), and the RTC
// is measured within seconds of a sync.
static const uint32_t SYNC_FRESH_MS = 10000;
// The rate is judged over at least this long and only within what the Offset
// register can correct (beyond that the time was changed, e.g. by hand).
static const double RTC_RATE_MIN_SPAN_S = 600;
static const double RTC_RATE_MAX_PPM = 260;
// The RTC is set in phase again when it is this far off.
static const double RTC_REWRITE_ERR_S = 0.100;
// Tick search: the first pass reads the RTC once per loop until its second
// changes; then the next tick is awaited from this long before it.
static const int64_t RTC_TICK_EARLY_US = 40000;
static const uint32_t RTC_TICK_SEARCH_MS = 6000;   // gives up after this long
static const char* const NVS_NAMESPACE = "clock";  // as settings.cpp
static const char* const NVS_RTC_OFFSET = "rtcoff";

enum TickSearch : uint8_t { TICK_IDLE, TICK_COARSE, TICK_FINE };
static TickSearch s_tick = TICK_IDLE;
static bool s_tick_for_base = false;       // measuring right after an in-phase write
static time_t s_tick_second = 0;           // RTC second before the awaited tick
static int64_t s_tick_before_us = 0;       // system time of the last read before a tick
static int64_t s_tick_after_us = 0;        // ... and of the first read after it
static uint32_t s_tick_started_ms = 0;
static bool s_check_pending = false;       // measure after the NTP sync once idle
static bool s_base_ok = false;             // RTC error s_base_err measured at s_base_us
static int64_t s_base_us = 0;
static double s_base_err = 0;
static double s_base_bound = 0;            // error bound of the sync behind the base, s
static int8_t s_rtc_steps = 0;             // Offset register
static uint32_t s_rtc_checks = 0;          // drift measurements after NTP syncs
static double s_rtc_last_ppm = 0, s_rtc_last_span_s = 0;   // the last one with a base

static TimeSource s_source = TIME_NONE;
static time_t s_last_sync = 0;
static uint32_t s_sync_count = 0;
static uint32_t s_last_sync_ms = 0;        // millis() of the last sync
static int64_t s_sync_bound_us = 0;        // its error bound (ntp_error_bound_us())
static int64_t s_last_offset_us = 0, s_last_delay_us = 0;
static int8_t s_last_server = -1;
static uint32_t s_clock_gen = 0;           // bumped whenever the system clock is set
static bool s_ntp_on = false;              // the network is up: bursts are scheduled
static bool s_ntp_busy = false;            // a burst runs in the NTP task
static uint32_t s_ntp_due_ms = 0;          // millis() of the next burst
static uint32_t s_ntp_retry_ms = NTP_RETRY_MIN_MS;
static bool s_rtc_ok = false;
static bool s_rtc_pending = false;   // the system time is to be written to the RTC
static bool s_grace_over = false;    // FIRST_SYNC_GRACE_MS has passed (latched: millis() wraps)
static char s_tz[64] = TZ_DEFAULT;
static char s_servers[3][64] = {NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3};

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

static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Every change of the system clock goes through here, so that an NTP result
// measured against the old clock is recognised as stale (s_clock_gen).
static void set_system_us(int64_t us) {
    struct timeval tv = {(time_t)(us / 1000000), (suseconds_t)(us % 1000000)};
    settimeofday(&tv, nullptr);
    ++s_clock_gen;
}

// Error bound of the system clock in s while a sync still vouches for it,
// else a negative value (manual time, RTC time, or the sync is too old).
static double sync_bound_s() {
    if (s_source != TIME_NTP || millis() - s_last_sync_ms > SYNC_FRESH_MS) return -1;
    return (double)s_sync_bound_us / 1e6;
}

// Puts the calibrated Offset back after the RTC lost power (it then resets to
// 0); without a stored value the RTC starts uncalibrated.
static void rtc_offset_restore() {
    int8_t stored = 0, current = 0;
    bool fast = false;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, true)) {
        if (p.isKey(NVS_RTC_OFFSET)) stored = p.getChar(NVS_RTC_OFFSET, 0);
        p.end();
    }
    if (!rtc_get_offset(&current, &fast) || current != stored || !fast) {
        if (rtc_set_offset(stored)) LOGI("time", "RTC offset set to %d", stored);
        else LOGE("time", "RTC offset write failed");
    }
    s_rtc_steps = stored;
}

static void rtc_offset_change(int8_t steps) {
    if (!rtc_set_offset(steps)) {
        LOGE("time", "RTC offset write failed");
        return;
    }
    s_rtc_steps = steps;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        p.putChar(NVS_RTC_OFFSET, steps);
        p.end();
    }
}

static void rtc_tick_start(bool for_base) {
    time_t utc;
    if (rtc_read(&utc) != RTC_OK) {
        if (!for_base) s_rtc_pending = true;   // cannot measure: just set it again
        return;
    }
    s_tick = TICK_COARSE;
    s_tick_for_base = for_base;
    s_tick_second = utc;
    s_tick_before_us = now_us();
    s_tick_started_ms = millis();
}

// Called with the RTC error (RTC ahead of the system clock, in s) at a tick:
// right after an in-phase write it becomes the base, provided a fresh NTP
// sync vouches for the system clock; after an NTP sync it is compared with
// the base, the Offset is corrected and the RTC set again when that is due.
static void rtc_tick_measured(double err, int64_t at_us) {
    const double bound = sync_bound_s();
    if (s_tick_for_base) {
        s_base_ok = bound >= 0;   // a manually set clock gives no base
        s_base_us = at_us;
        s_base_err = err;
        s_base_bound = bound;
        return;
    }
    if (bound < 0) {   // the check started too long after its sync
        LOGI("time", "RTC check skipped: the sync is too old");
        return;
    }
    ++s_rtc_checks;
    // Rewrite when the RTC is surely RTC_REWRITE_ERR_S off, not just within
    // the uncertainty of this sync.
    bool rewrite = !s_base_ok || fabs(err) - bound >= RTC_REWRITE_ERR_S;
    if (!s_base_ok) {
        LOGI("time", "RTC %+.1f ms (+-%.1f, no drift base yet)", err * 1e3, bound * 1e3);
    } else {
        const double span = (double)(at_us - s_base_us) / 1e6;
        const double ppm = (err - s_base_err) / span * 1e6;   // > 0: the RTC runs fast
        s_rtc_last_ppm = ppm;
        s_rtc_last_span_s = span;
        const double noise_s = abs(s_rtc_steps) * RTC_PULSE_S + s_base_bound + bound + 2 * RTC_TICK_READ_ERR_S;
        const double noise_ppm = noise_s / span * 1e6;
        const bool judged = span >= RTC_RATE_MIN_SPAN_S && fabs(ppm) <= RTC_RATE_MAX_PPM;
        int target = s_rtc_steps;
        if (judged && fabs(ppm) >= RTC_PPM_PER_STEP / 2 + noise_ppm)
            target = constrain(s_rtc_steps + (int)lround(ppm / RTC_PPM_PER_STEP), RTC_STEP_MIN, RTC_STEP_MAX);
        LOGI("time", "RTC %+.1f ms after %.2f h: %+.1f ppm (+-%.1f), offset %d -> %d", err * 1e3, span / 3600, ppm,
             noise_ppm, s_rtc_steps, target);
        const bool changed = target != s_rtc_steps;
        if (changed) rtc_offset_change((int8_t)target);
        // A new rate needs a fresh start; so does a time change (a rate beyond
        // what the Offset register could cause, also allowing for the noise).
        const bool implausible = span >= RTC_RATE_MIN_SPAN_S && fabs(ppm) - noise_ppm > RTC_RATE_MAX_PPM;
        if (changed || implausible) rewrite = true;
    }
    if (rewrite) s_rtc_pending = true;
}

// One step of the tick search per loop: the coarse pass brackets the tick
// between two reads, then the next tick (one RTC second later, the same within
// a fraction of a millisecond) is awaited with back-to-back reads, which keeps
// the loop busy for some 50 ms instead of up to a second.
static void rtc_tick_step() {
    if (millis() - s_tick_started_ms > RTC_TICK_SEARCH_MS) {
        s_tick = TICK_IDLE;
        LOGE("time", "RTC tick not found");
        if (!s_tick_for_base) s_rtc_pending = true;
        return;
    }
    time_t utc;
    if (s_tick == TICK_COARSE) {
        if (rtc_read(&utc) != RTC_OK) return;   // retried next loop until the timeout
        const int64_t t = now_us();
        if (utc == s_tick_second) {
            s_tick_before_us = t;
            return;
        }
        s_tick_second = utc;
        s_tick_after_us = t;
        s_tick = TICK_FINE;
        return;
    }
    const int64_t t = now_us(), expected = s_tick_before_us + 1000000;
    if (t < expected - RTC_TICK_EARLY_US) return;
    if (t >= expected) {   // the loop came too late for this tick: take the next one
        s_tick_before_us += 1000000;
        s_tick_after_us += 1000000;
        ++s_tick_second;
        return;
    }
    const int64_t until = s_tick_after_us + 1000000 + RTC_TICK_EARLY_US;
    int64_t at = t;
    utc = s_tick_second;
    while (utc == s_tick_second && at < until) {
        if (rtc_read(&utc) != RTC_OK) return;
        at = now_us();
    }
    if (utc == s_tick_second) return;   // not seen: retried until the timeout
    s_tick = TICK_IDLE;
    rtc_tick_measured((double)utc - (double)at / 1e6, at);
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
        rtc_offset_restore();
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
            set_system_us((int64_t)utc * 1000000);
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
        set_system_us(0);
    }
    if (!ntp_begin()) LOGE("time", "NTP task did not start");
}

void timekeep_set_servers(const char* const servers[3]) {
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
    const char* const names[3] = {s_servers[0], s_servers[1], s_servers[2]};
    ntp_set_servers(names);
    s_ntp_due_ms = millis();   // try the new servers right away (once the network is up)
}

const char* timekeep_server(int i) { return (i >= 0 && i < 3) ? s_servers[i] : ""; }

void timekeep_start_ntp() {
    if (!s_ntp_on) LOGI("time", "NTP on (%s, %s, %s)", s_servers[0], s_servers[1][0] ? s_servers[1] : "-",
                        s_servers[2][0] ? s_servers[2] : "-");
    s_ntp_on = true;
    s_ntp_due_ms = millis();   // after a reconnect, sync right away
    s_ntp_retry_ms = NTP_RETRY_MIN_MS;
}

// Writes the system time to the RTC once the current second reaches the
// window before RTC_RELEASE_US (see there).
static void rtc_write_in_phase() {
    if (!s_rtc_pending || !s_rtc_ok || s_tick != TICK_IDLE) return;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_usec < RTC_RELEASE_US - RTC_WRITE_WINDOW_US || tv.tv_usec >= RTC_RELEASE_US) return;
    s_rtc_pending = false;
    s_base_ok = false;
    const bool written = rtc_write_stopped(tv.tv_sec);
    while (written && tv.tv_usec < RTC_RELEASE_US) gettimeofday(&tv, nullptr);
    const bool released = rtc_release();   // also after a failed write: never leave it stopped
    if (written && released) {
        LOGI("time", "RTC set (utc %ld, in phase)", (long)tv.tv_sec);
        rtc_tick_start(true);   // the base for the next drift measurement
    } else {
        LOGE("time", "RTC write failed");
    }
}

// Sets the system clock from a finished burst and schedules the next one.
static void ntp_apply(const NtpResult& r, uint32_t now_ms) {
    if (r.ok && r.gen != s_clock_gen) {   // measured against a clock that was set since
        LOGI("time", "NTP result dropped: the clock was set meanwhile");
        s_ntp_due_ms = now_ms;
        return;
    }
    const char* name = r.server >= 0 ? s_servers[r.server] : "-";
    const int64_t target = r.ok ? now_us() + r.best.offset_us : 0;
    const bool in_range =
        target >= (int64_t)min_valid_epoch() * 1000000 && target < (int64_t)RTC_END_EPOCH * 1000000;
    if (!r.ok || !in_range) {
        if (r.ok) LOGE("time", "NTP time from %s out of range (%lld s)", name, (long long)(target / 1000000));
        else LOGI("time", "NTP failed (%s), retry in %lu s", r.error, (unsigned long)(s_ntp_retry_ms / 1000));
        s_ntp_due_ms = now_ms + s_ntp_retry_ms;
        s_ntp_retry_ms = min(s_ntp_retry_ms * 2, NTP_RETRY_MAX_MS);
        return;
    }
    set_system_us(target);
    s_last_sync = (time_t)(target / 1000000);
    ++s_sync_count;
    s_source = TIME_NTP;
    s_last_sync_ms = millis();
    s_sync_bound_us = ntp_error_bound_us(r.best);
    s_last_offset_us = r.best.offset_us;
    s_last_delay_us = r.best.delay_us;
    s_last_server = r.server;
    s_check_pending = s_rtc_ok;   // measure the RTC against the fresh time, then decide
    s_ntp_due_ms = now_ms + NTP_INTERVAL_MS;
    s_ntp_retry_ms = NTP_RETRY_MIN_MS;
    struct tm lt;
    localtime_r(&s_last_sync, &lt);
    LOGI("time", "NTP sync #%lu: %04d-%02d-%02d %02d:%02d:%02d %s (%s, offset %+.1f ms, delay %.1f ms, %u of %u)",
         (unsigned long)s_sync_count, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min,
         lt.tm_sec, s_tz, name, (double)r.best.offset_us / 1000, (double)r.best.delay_us / 1000, r.answers,
         r.queries);
}

static void ntp_poll() {
    const uint32_t now = millis();
    NtpResult r;
    // A result moves the system clock: never in the middle of an RTC tick measurement.
    if (s_ntp_busy && s_tick == TICK_IDLE && ntp_take_result(&r)) {
        s_ntp_busy = false;
        ntp_apply(r, now);
    }
    if (!s_ntp_on || s_ntp_busy || (int32_t)(now - s_ntp_due_ms) < 0 || !net_connected()) return;
    if (ntp_start_burst(s_clock_gen)) s_ntp_busy = true;
}

void timekeep_tick() {
    if (!s_grace_over && millis() > FIRST_SYNC_GRACE_MS) s_grace_over = true;
    if (s_tick != TICK_IDLE) {
        rtc_tick_step();
    } else if (s_check_pending && !s_rtc_pending) {
        s_check_pending = false;
        rtc_tick_start(false);
    }
    rtc_write_in_phase();
    ntp_poll();
}

bool timekeep_valid() { return s_source != TIME_NONE && time(nullptr) >= min_valid_epoch(); }

bool timekeep_set_manual(time_t utc) {
    if (utc < min_valid_epoch() || utc >= RTC_END_EPOCH) return false;
    set_system_us((int64_t)utc * 1000000);
    s_source = TIME_MANUAL;
    s_rtc_pending = s_rtc_ok;
    return true;
}

TimeSource timekeep_source() { return s_source; }
time_t timekeep_last_sync() { return s_last_sync; }
uint32_t timekeep_sync_count() { return s_sync_count; }
bool timekeep_rtc_ok() { return s_rtc_ok; }

void timekeep_last_ntp(double* offset_ms, double* delay_ms, const char** server) {
    *offset_ms = (double)s_last_offset_us / 1000;
    *delay_ms = (double)s_last_delay_us / 1000;
    *server = s_last_server >= 0 ? s_servers[s_last_server] : "-";
}

void timekeep_rtc_drift(int8_t* steps, uint32_t* checks, float* last_ppm, float* last_hours) {
    *steps = s_rtc_steps;
    *checks = s_rtc_checks;
    *last_ppm = (float)s_rtc_last_ppm;
    *last_hours = (float)(s_rtc_last_span_s / 3600);
}

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
