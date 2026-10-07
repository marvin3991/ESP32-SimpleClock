// Runs src/timekeep.cpp through days of simulated time, in about a second: a
// PCF85063 model (crystal error, Offset correction pulses, STOP and release),
// the ESP32's system clock with its own drift, and an NTP client whose
// replies travel uneven paths. Each scenario checks the Offset the firmware
// ends up with, how often it changed it, and how far the RTC strays.
//
// Built and run by tests/run.sh: timekeep.cpp is compiled with gettimeofday,
// settimeofday and time renamed to sim_clock.c's, the Arduino and NVS APIs
// come from stubs/, and rtc.h, ntp.h and net.h are implemented here.
// "-v" prints the firmware's log.
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <random>
#include <string>
#include <vector>

#include "Arduino.h"
#include "net.h"
#include "ntp.h"
#include "rtc.h"
#include "timekeep.h"

std::map<std::string, int> g_nvs;
FakeSerial Serial;
volatile bool g_log_mute = false;
static bool g_verbose = false;
static std::vector<std::string> g_log;

// ------------------------------------------------------------ simulated time
static int64_t g_true_us = 0;        // UTC
static int64_t g_boot_true_us = 0;
static double g_sys_ppm = -20;       // the ESP32's crystal (this board measured -4; -20 is harder)
static int64_t g_sys_base_us = 0, g_sys_true_base_us = 0;

static double hours() { return (double)(g_true_us - g_boot_true_us) / 3.6e9; }
static int64_t sys_at(int64_t true_us) {
    return g_sys_base_us + (int64_t)llround((double)(true_us - g_sys_true_base_us) * (1 + g_sys_ppm * 1e-6));
}

extern "C" int64_t sim_clock_now_us(void) {
    g_true_us += 2;   // so that busy-wait loops make progress
    return sys_at(g_true_us);
}
extern "C" void sim_clock_set_us(int64_t us) {
    g_sys_base_us = us;
    g_sys_true_base_us = g_true_us;
}

uint32_t millis() { return (uint32_t)((double)(g_true_us - g_boot_true_us) * (1 + g_sys_ppm * 1e-6) / 1000); }
void delay(uint32_t ms) { g_true_us += (int64_t)ms * 1000; }

int FakeSerial::printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char line[600];
    snprintf(line, sizeof line, "%7.3f h  %s", hours(), buf);
    g_log.push_back(line);
    if (g_verbose) fputs(line, stdout);
    return n;
}

// ------------------------------------------------------------ PCF85063 model
static const double PULSE_US = 1e6 / 1024;
struct Rtc {
    bool valid = true, stopped = false;
    int64_t sec = 0;       // the time registers
    double frac_us = 0;    // progress into the current second
    int64_t true_at = 0;   // the model has been advanced up to this true time
    double ppm = -85;      // crystal
    int steps = 0;         // Offset register, fast mode
    int writes = 0;
    std::vector<std::pair<double, int>> offsets;   // (hour, steps) of every Offset write
} R;

// Fast mode (datasheet Rev. 7.3, section 7.2.3; checked on the board): one
// 1/1024 s pulse a second at seconds 0..n-1 of every 4th minute, above 60
// pulses the rest in second 59. Negative steps add time.
static void rtc_new_second() {
    const int n = abs(R.steps);
    if (n == 0 || (R.sec / 60) % 4 != 0) return;
    const int64_t second = R.sec % 60;
    int pulses = second < std::min(n, 60) ? 1 : 0;
    if (second == 59 && n > 60) pulses += n - 60;
    R.frac_us += (R.steps < 0 ? 1 : -1) * pulses * PULSE_US;
}

static void rtc_advance() {
    const int64_t d = g_true_us - R.true_at;
    R.true_at = g_true_us;
    if (R.stopped || d <= 0) return;
    R.frac_us += (double)d * (1 + R.ppm * 1e-6);
    while (R.frac_us >= 1e6) {
        R.frac_us -= 1e6;
        ++R.sec;
        rtc_new_second();
    }
}

static double rtc_err_ms() {   // the RTC minus true time
    rtc_advance();
    return ((double)(R.sec - g_true_us / 1000000) * 1e6 + R.frac_us - (double)(g_true_us % 1000000)) / 1000;
}

bool rtc_init() {
    rtc_advance();
    return true;
}
RtcResult rtc_read(time_t* utc) {
    rtc_advance();
    *utc = (time_t)R.sec;
    g_true_us += 1000;   // 11 bytes over I2C at 100 kHz
    return R.valid ? RTC_OK : RTC_INVALID;
}
bool rtc_write_stopped(time_t utc) {
    rtc_advance();
    g_true_us += 1000;
    R.true_at = g_true_us;
    R.stopped = true;
    R.valid = true;
    R.sec = utc;
    R.frac_us = 0;
    ++R.writes;
    return true;
}
bool rtc_release() {
    g_true_us += 300;
    R.true_at = g_true_us;
    R.stopped = false;
    R.frac_us = 500000;   // the first tick follows 0.500 s after the release (this board)
    return true;
}
bool rtc_set_offset(int8_t steps) {
    rtc_advance();
    R.steps = steps;
    R.offsets.push_back({hours(), steps});
    return true;
}
bool rtc_get_offset(int8_t* steps, bool* fast_mode) {
    *steps = (int8_t)R.steps;
    *fast_mode = true;
    return true;
}
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
time_t utc_from_fields(int year, int month, int day, int hour, int minute, int second) {
    return (time_t)(days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second);
}

// ------------------------------------------------------------ network and NTP
struct Window {
    double from_h, to_h;
};
struct Net {
    double up_ms = 4, down_ms = 4, jitter_ms = 3;   // base one-way delay, plus uniform jitter per path
    double spike_p = 0, spike_ms = 0;               // chance and size of extra delay on the way back
    std::vector<Window> no_reply, wifi_down;
    double bad_time_h = -1;                          // the first burst after this hour answers year 2000
} N;
static std::mt19937_64 g_rng;   // fully specified by the standard: the same numbers everywhere
static double uniform(double a, double b) { return a + (b - a) * (double)(g_rng() >> 11) * 0x1.0p-53; }
static bool in(const std::vector<Window>& windows, double h) {
    for (const Window& w : windows)
        if (h >= w.from_h && h < w.to_h) return true;
    return false;
}

static bool g_busy = false, g_ready = false;
static int64_t g_burst_start = 0;
static uint32_t g_burst_gen = 0;
static NtpResult g_result;
static int g_bursts = 0, g_failed = 0;

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put_ts(uint8_t* p, int64_t unix_us) {
    put32(p, (uint32_t)((uint64_t)(unix_us / 1000000 + 2208988800LL) & 0xFFFFFFFFu));
    put32(p + 4, (uint32_t)(((uint64_t)(unix_us % 1000000) << 32) / 1000000));
}

// Four queries 2 s apart like ntp.cpp; each reply goes through ntp_parse_reply().
static void finish_burst() {
    NtpResult r = {};
    r.gen = g_burst_gen;
    r.server = 0;
    r.queries = 4;
    const bool bad_time = N.bad_time_h >= 0 && hours() >= N.bad_time_h;
    if (bad_time) N.bad_time_h = -1;
    NtpSample samples[4];
    int n = 0;
    for (int q = 0; q < 4; ++q) {
        const int64_t tq = g_burst_start + q * 2000000LL;
        if (in(N.no_reply, (double)(tq - g_boot_true_us) / 3.6e9)) continue;
        const int64_t up = (int64_t)((N.up_ms + uniform(0, N.jitter_ms)) * 1000);
        double down_ms = N.down_ms + uniform(0, N.jitter_ms);
        if (uniform(0, 1) < N.spike_p) down_ms += uniform(0, N.spike_ms);
        const int64_t down = (int64_t)(down_ms * 1000), hold = 100;
        const int64_t t2 = bad_time ? 946684800LL * 1000000 : tq + up;   // 2000-01-01
        uint8_t pkt[48] = {};
        pkt[0] = 0x24;   // version 4, server
        pkt[1] = 2;      // stratum
        const uint64_t nonce = g_rng();
        for (int i = 0; i < 8; ++i) pkt[24 + i] = (uint8_t)(nonce >> (56 - 8 * i));
        put32(pkt + 4, (uint32_t)(0.0002 * 65536));   // root delay 0.2 ms
        put32(pkt + 8, (uint32_t)(0.035 * 65536));    // root dispersion 35 ms, like tock
        put_ts(pkt + 32, t2);
        put_ts(pkt + 40, t2 + hold);
        char kiss[5];
        if (ntp_parse_reply(pkt, 48, nonce, sys_at(tq), sys_at(tq + up + hold + down), &samples[n], kiss) ==
            NTP_REPLY_OK)
            ++n;
    }
    r.answers = (uint8_t)n;
    const int best = ntp_best_sample(samples, n);
    if (best >= 0) {
        r.ok = true;
        r.best = samples[best];
    } else {
        snprintf(r.error, sizeof r.error, "sim: no reply");
        ++g_failed;
    }
    g_result = r;
    g_ready = true;
    g_busy = false;
}

bool ntp_begin() { return true; }
void ntp_set_servers(const char* const servers[3]) { (void)servers; }
bool ntp_start_burst(uint32_t gen) {
    if (g_busy) return false;
    g_busy = true;
    g_ready = false;
    g_burst_gen = gen;
    g_burst_start = g_true_us;
    ++g_bursts;
    return true;
}
bool ntp_take_result(NtpResult* out) {
    // Answers are in after ~6.1 s; without answers ntp.cpp tries all three
    // servers (4 queries, 1 s timeout each).
    if (g_busy && g_true_us - g_burst_start >= (in(N.no_reply, hours()) ? 21000000 : 6100000)) finish_burst();
    if (!g_ready) return false;
    *out = g_result;
    g_ready = false;
    return true;
}
bool net_connected() { return !in(N.wifi_down, hours()); }

// ------------------------------------------------------------ scenarios
struct Scenario {
    const char* name;
    double crystal_ppm = -85, daily_ppm = 0;   // daily_ppm: +- swing over 24 h (temperature)
    int stored = 0;                            // Offset in NVS at start-up
    bool rtc_valid = true;                     // false: the RTC lost power (Offset back to 0)
    double hours = 72;
    double manual_h = -1;                      // timekeep_set_manual() at this hour, up to 1 s off
    double manual_in_burst_h = -1;             // ... while a burst runs
    int expect_final = 999;                    // 999: not checked
    int max_changes = 99;
    double max_rtc_err_ms = 1e9;               // after the first 6 hours
    const char* expect_log = nullptr;
};

static int run_here(const Scenario& sc, const Net& net) {
    N = net;
    g_rng.seed(12345);
    const int64_t start = 1791417600LL * 1000000 + 123456;   // 2026-10-08 00:00:00.123456 UTC
    g_true_us = g_boot_true_us = start;
    g_sys_base_us = 0;   // whatever the SoC counts at boot; timekeep sets it
    g_sys_true_base_us = start;
    R.ppm = sc.crystal_ppm;
    R.valid = sc.rtc_valid;
    R.sec = start / 1000000 - 2;   // 2.3 s behind
    R.frac_us = 300000;
    R.true_at = start;
    g_nvs["clock/rtcoff"] = sc.stored;
    R.steps = sc.rtc_valid ? sc.stored : 0;

    timekeep_init("CST-8");
    const char* const servers[3] = {"tock.stdtime.gov.tw", "time.stdtime.gov.tw", "pool.ntp.org"};
    timekeep_set_servers(servers);
    bool net_up = false, manual_done = false, manual_in_burst_done = false;
    double max_rtc = 0, max_sys = 0;
    int writes_at_6h = -1;
    int64_t next_sample = start;
    const int64_t end = start + (int64_t)(sc.hours * 3.6e9);
    while (g_true_us < end) {
        const double h = hours();
        if (sc.daily_ppm != 0) R.ppm = sc.crystal_ppm + sc.daily_ppm * sin(2 * M_PI * h / 24);
        const bool up = net_connected() && h > 5.0 / 3600;
        if (up && !net_up) timekeep_start_ntp();   // net.cpp calls it on every (re)connect
        net_up = up;
        if (sc.manual_h >= 0 && !manual_done && h >= sc.manual_h) {
            manual_done = true;
            timekeep_set_manual((time_t)(g_true_us / 1000000 + 1));
        }
        if (sc.manual_in_burst_h >= 0 && !manual_in_burst_done && h >= sc.manual_in_burst_h && g_busy) {
            manual_in_burst_done = true;
            timekeep_set_manual((time_t)(g_true_us / 1000000));
        }
        timekeep_tick();
        g_true_us += 10000;   // LOOP_IDLE_MS
        if (g_true_us >= next_sample) {
            next_sample += 60000000;
            if (h >= 6) {
                if (writes_at_6h < 0) writes_at_6h = R.writes;
                max_rtc = std::max(max_rtc, fabs(rtc_err_ms()));
                max_sys = std::max(max_sys, fabs((double)(sys_at(g_true_us) - g_true_us) / 1000));
            }
        }
    }

    std::string changes_text;
    int changes = 0;   // the restore after a power loss, at start-up, is not a change
    for (const auto& o : R.offsets) {
        if (o.first < 0.01) continue;
        ++changes;
        char b[32];
        snprintf(b, sizeof b, " %+d@%.1fh", o.second, o.first);
        changes_text += b;
    }
    bool log_ok = !sc.expect_log;
    for (const std::string& l : g_log)
        if (sc.expect_log && l.find(sc.expect_log) != std::string::npos) log_ok = true;
    const bool ok = (sc.expect_final == 999 || R.steps == sc.expect_final) && changes <= sc.max_changes &&
                    max_rtc <= sc.max_rtc_err_ms && log_ok;
    printf("%s %-36s offset %+d, changes:%s | syncs %u, bursts %d (%d failed), RTC writes %d | after 6 h: "
           "RTC within %.0f ms, system within %.0f ms\n",
           ok ? "PASS" : "FAIL", sc.name, R.steps, changes ? changes_text.c_str() : " none", timekeep_sync_count(),
           g_bursts, g_failed, R.writes, max_rtc, max_sys);
    if (!ok) {
        if (!log_ok) printf("     log never said: %s\n", sc.expect_log);
        for (const std::string& l : g_log)   // the drift decisions ("offset a -> b") and errors
            if (l.find(" -> ") != std::string::npos || l.find("ERROR") != std::string::npos)
                printf("     %s", l.c_str());
    }
    return ok ? 0 : 1;
}

static int g_scenarios = 0;

// timekeep.cpp keeps its state in statics: every scenario gets a fresh process.
static int run(const Scenario& sc, const Net& net) {
    ++g_scenarios;
    fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        const int rc = run_here(sc, net);
        fflush(stdout);
        _exit(rc);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

int main(int argc, char** argv) {
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    Net lan;                    // on USB power: a few ms each way
    Net battery = lan;          // Wi-Fi power save: 30 % of the replies held up to 300 ms
    battery.spike_p = 0.3;
    battery.spike_ms = 300;
    Net slow = lan;             // every reply held 0..300 ms
    slow.spike_p = 1;
    slow.spike_ms = 300;
    Net outage = lan;
    outage.no_reply.push_back({10, 16});
    Net wifi = lan;
    wifi.wifi_down.push_back({20, 23});
    Net bad_time = lan;
    bad_time.bad_time_h = 30;
    const int k = (int)lround(-85 / 4.069);   // the Offset for a -85 ppm crystal: -21
    int failed = 0;
    Scenario s;
    s = {"crystal -85 ppm"};
    s.expect_final = k, s.max_changes = 1, s.max_rtc_err_ms = 150;
    failed += run(s, lan);
    s = {"crystal -85, Offset -19 stored"};
    s.stored = -19, s.expect_final = k, s.max_changes = 1, s.max_rtc_err_ms = 150;
    failed += run(s, lan);
    s = {"crystal -85, on battery"};
    s.expect_final = k, s.max_changes = 1, s.max_rtc_err_ms = 150;
    failed += run(s, battery);
    s = {"crystal -85, -19 stored, on battery"};
    s.stored = -19, s.expect_final = k, s.max_changes = 1, s.max_rtc_err_ms = 150;
    failed += run(s, battery);
    s = {"crystal -85, all replies held"};
    s.max_changes = 3;
    failed += run(s, slow);
    s = {"crystal -85 +-3 ppm a day"};
    s.daily_ppm = 3, s.max_changes = 6, s.max_rtc_err_ms = 200;
    failed += run(s, lan);
    s = {"crystal -3.5 ppm"};
    s.crystal_ppm = -3.5, s.expect_final = -1, s.max_changes = 1;
    failed += run(s, lan);
    s = {"crystal -1.5 ppm (under half a step)"};
    s.crystal_ppm = -1.5, s.expect_final = 0, s.max_changes = 0;
    failed += run(s, lan);
    s = {"crystal +60 ppm"};
    s.crystal_ppm = 60, s.expect_final = 15, s.max_changes = 1;
    failed += run(s, lan);
    s = {"crystal -250 ppm"};
    s.crystal_ppm = -250, s.expect_final = -61, s.max_changes = 1;
    failed += run(s, lan);
    s = {"crystal -300 ppm (beyond the register)"};
    s.crystal_ppm = -300, s.expect_final = 0, s.max_changes = 0;
    failed += run(s, lan);
    s = {"time set by hand at 10 h"};
    s.stored = k, s.manual_h = 10, s.expect_final = k, s.max_changes = 0;
    failed += run(s, lan);
    s = {"time set by hand during a burst"};
    s.stored = k, s.manual_in_burst_h = 10, s.expect_final = k, s.max_changes = 0;
    s.expect_log = "NTP result dropped";
    failed += run(s, lan);
    s = {"no replies 10-16 h"};
    s.stored = k, s.expect_final = k, s.max_changes = 0, s.expect_log = "NTP failed";
    failed += run(s, outage);
    s = {"Wi-Fi down 20-23 h"};
    s.stored = k, s.expect_final = k, s.max_changes = 0;
    failed += run(s, wifi);
    s = {"a server answers year 2000 once"};
    s.stored = k, s.expect_final = k, s.max_changes = 0, s.expect_log = "out of range";
    failed += run(s, bad_time);
    s = {"RTC lost power, -21 stored"};
    s.rtc_valid = false, s.stored = k, s.expect_final = k, s.max_changes = 0;
    failed += run(s, lan);
    printf("timekeep: %d scenarios, %d failed\n", g_scenarios, failed);
    return failed ? 1 : 0;
}
