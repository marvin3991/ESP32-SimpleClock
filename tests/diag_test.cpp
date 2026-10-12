// Unit tests for src/diag.cpp: the offline time accounting, the event log
// (order, wrap-around, the power state stamped on each event), the failure
// code counts, the loop timing and the uptime across the millis() wrap.
// Built and run by tests/run.sh; exits non-zero on a failure.
#include <map>
#include <string>

#include "Arduino.h"
#include "diag.h"
#include "net.h"
#include "power.h"
#include "timekeep.h"

static int checks = 0, fails = 0;
#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        ++checks;                                                                          \
        const long long a_ = (long long)(a), b_ = (long long)(b);                          \
        if (a_ != b_) {                                                                    \
            ++fails;                                                                       \
            printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); \
        }                                                                                  \
    } while (0)

// What diag.cpp reads from the rest of the firmware, under test control.
static uint32_t g_ms = 0;
static PowerDiag g_power = {};
static bool g_power_save = false;
static std::string g_out;
FakeSerial Serial;
uint32_t millis() { return g_ms; }
void delay(uint32_t ms) { g_ms += ms; }
int FakeSerial::printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_out += buf;
    return n;
}
void power_diag(PowerDiag* out) { *out = g_power; }
bool net_power_save() { return g_power_save; }
const char* net_reason_name(uint8_t code) { return code == DIAG_REASON_OTHER ? "OTHER" : "NAME"; }
bool timekeep_valid() { return false; }

static DiagSummary summary() {
    DiagSummary s;
    diag_summary(&s);
    return s;
}

int main() {
    g_ms = 1000;
    diag_init("POWER ON");
    CHECK_EQ(strcmp(summary().reset, "POWER ON"), 0);

    // Offline from start-up until the first connection.
    g_ms = 6000;
    g_power.vbus_mv = 5012;
    g_power.vsys_mv = 4310;
    diag_event(DIAG_WIFI_UP, 0, -45);
    DiagSummary s = summary();
    CHECK_EQ(s.offline_s, 5);
    CHECK_EQ(s.longest_offline_s, 5);
    CHECK_EQ(s.offline_now_s, 0);

    // Lost at 10 s; failures meanwhile do not restart the stretch.
    g_ms = 10000;
    diag_event(DIAG_WIFI_LOST, 200, -52);
    g_ms = 20000;
    g_power_save = true;
    FakeSerial::plugged = false;
    diag_event(DIAG_WIFI_FAILED, 15, 0);
    g_ms = 30000;
    diag_event(DIAG_WIFI_FAILED, 15, 0);
    g_ms = 40000;
    s = summary();
    CHECK_EQ(s.wifi_lost, 1);
    CHECK_EQ(s.wifi_failed, 2);
    CHECK_EQ(s.offline_now_s, 30);
    CHECK_EQ(s.offline_s, 35);          // 5 + the 30 so far
    CHECK_EQ(s.longest_offline_s, 30);  // the current stretch counts
    g_ms = 70000;
    diag_event(DIAG_WIFI_UP, 0, -47);
    s = summary();
    CHECK_EQ(s.offline_s, 65);
    CHECK_EQ(s.longest_offline_s, 60);
    CHECK_EQ(s.offline_now_s, 0);
    // A second "up" while online adds nothing.
    g_ms = 80000;
    diag_event(DIAG_WIFI_UP, 0, -47);
    CHECK_EQ(summary().offline_s, 65);

    // The event log: oldest first, with the state at each event (the second
    // "up" is logged too).
    CHECK_EQ(diag_count(), 6);
    const DiagEntry* e = diag_entry(1);
    CHECK_EQ(e->kind, DIAG_WIFI_LOST);
    CHECK_EQ(e->reason, 200);
    CHECK_EQ(e->rssi, -52);
    CHECK_EQ(e->uptime_s, 10);
    CHECK_EQ(e->vbus_mv, 5012);
    CHECK_EQ(e->power_save, false);
    CHECK_EQ(e->usb_host, true);
    e = diag_entry(2);
    CHECK_EQ(e->kind, DIAG_WIFI_FAILED);
    CHECK_EQ(e->power_save, true);
    CHECK_EQ(e->usb_host, false);
    CHECK_EQ(e->utc, 0);   // no valid time yet
    CHECK_EQ(diag_entry(6) == nullptr, true);
    CHECK_EQ(diag_entry(-1) == nullptr, true);

    // Failure codes, most frequent first.
    uint8_t codes[8];
    uint16_t counts[8];
    int n = diag_reasons(codes, counts, 8);
    CHECK_EQ(n, 2);
    CHECK_EQ(codes[0], 15);
    CHECK_EQ(counts[0], 2);
    CHECK_EQ(codes[1], 200);
    CHECK_EQ(counts[1], 1);

    // More distinct codes than slots: the eighth and later share "other".
    for (int code = 1; code <= 10; ++code) diag_event(DIAG_WIFI_FAILED, (uint8_t)code, 0);
    n = diag_reasons(codes, counts, 8);
    CHECK_EQ(n, 8);
    int other = 0, total = 0;
    for (int i = 0; i < n; ++i) {
        total += counts[i];
        if (codes[i] == DIAG_REASON_OTHER) other = counts[i];
    }
    CHECK_EQ(total, 13);   // 3 earlier + 10 now
    CHECK_EQ(other, 5);    // slots: 200, 15, 1..5; codes 6..10 are "other"
    CHECK_EQ(diag_reasons(codes, counts, 3), 3);   // limited by the caller

    // The log keeps the newest DIAG_LOG_LEN events.
    for (int i = 0; i < 40; ++i) {
        g_ms += 1000;
        diag_event(DIAG_USB_OFF, 0, 0);
    }
    CHECK_EQ(diag_count(), DIAG_LOG_LEN);
    CHECK_EQ(diag_entry(DIAG_LOG_LEN - 1)->uptime_s, g_ms / 1000);
    CHECK_EQ(diag_entry(0)->uptime_s, g_ms / 1000 - (DIAG_LOG_LEN - 1));

    // Loop timing.
    diag_loop_ms(150);
    diag_loop_ms(250);
    diag_loop_ms(90);
    s = summary();
    CHECK_EQ(s.loop_max_ms, 250);
    CHECK_EQ(s.slow_loops, 1);

    // Uptime keeps counting across the millis() wrap (49.7 days).
    const uint32_t before = summary().uptime_s;
    g_ms = 0xFFFFFC18u;   // 1 s before the wrap... from the last reading, far ahead
    diag_loop_ms(10);
    const uint32_t at_wrap = summary().uptime_s;
    CHECK_EQ(at_wrap > before, true);
    g_ms = 2000;          // 3 s later, after the wrap
    diag_loop_ms(10);
    CHECK_EQ(summary().uptime_s, at_wrap + 3);

    // The console report names the events and codes.
    g_out.clear();
    diag_print();
    CHECK_EQ(g_out.find("reasons") != std::string::npos, true);
    CHECK_EQ(g_out.find("USB POWER OFF") != std::string::npos, true);
    CHECK_EQ(g_out.find("OTHER x5") != std::string::npos, true);

    printf("diag: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
