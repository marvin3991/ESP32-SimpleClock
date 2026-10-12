#include "diag.h"

#include <Arduino.h>
#include <string.h>

#include "net.h"
#include "power.h"
#include "timekeep.h"

// A pass of the main loop that takes longer than this is counted: a blocked
// USB serial write (no reader on a USB host: up to 100 ms per write) shows up
// here.
static const uint32_t SLOW_LOOP_MS = 200;
static const int REASON_SLOTS = 8;   // distinct failure codes counted, the last slot is "other"

static DiagEntry s_log[DIAG_LOG_LEN];
static int s_next = 0, s_count = 0;
static uint8_t s_reason[REASON_SLOTS];
static uint16_t s_reason_n[REASON_SLOTS];
static uint32_t s_lost = 0, s_failed = 0;
static bool s_online = false;
static uint32_t s_offline_since_ms = 0;    // millis() when the current offline stretch began
static uint32_t s_offline_s = 0, s_longest_s = 0;   // finished stretches
static uint32_t s_loop_max_ms = 0, s_slow_loops = 0;
static uint64_t s_up_ms = 0;               // uptime, safe across the millis() wrap
static uint32_t s_up_last_ms = 0;
static const char* s_reset = "?";

static void tick_uptime() {
    const uint32_t now = millis();
    s_up_ms += (uint32_t)(now - s_up_last_ms);
    s_up_last_ms = now;
}

static void count_reason(uint8_t code) {
    int i = 0;   // slots fill in order and never empty: a code is found before a free slot
    while (i < REASON_SLOTS - 1 && s_reason_n[i] && s_reason[i] != code) ++i;
    if (i == REASON_SLOTS - 1) code = DIAG_REASON_OTHER;
    s_reason[i] = code;
    if (s_reason_n[i] < UINT16_MAX) ++s_reason_n[i];
}

void diag_init(const char* reset_reason) {
    s_reset = reset_reason;
    s_up_last_ms = millis();
    s_up_ms = s_up_last_ms;
    s_offline_since_ms = s_up_last_ms;   // offline until the first connection
}

void diag_event(DiagKind kind, uint8_t reason, int8_t rssi) {
    tick_uptime();
    const uint32_t now = millis();
    PowerDiag p;
    power_diag(&p);
    DiagEntry& e = s_log[s_next];
    e.utc = timekeep_valid() ? time(nullptr) : 0;
    e.uptime_s = (uint32_t)(s_up_ms / 1000);
    e.kind = kind;
    e.reason = reason;
    e.rssi = rssi;
    e.power_save = net_power_save();
    e.usb_host = Serial.isPlugged();
    e.vbus_mv = p.vbus_mv;
    e.vsys_mv = p.vsys_mv;
    s_next = (s_next + 1) % DIAG_LOG_LEN;
    if (s_count < DIAG_LOG_LEN) ++s_count;
    switch (kind) {
        case DIAG_WIFI_UP:
            if (!s_online) {
                const uint32_t span_s = (now - s_offline_since_ms) / 1000;
                s_offline_s += span_s;
                if (span_s > s_longest_s) s_longest_s = span_s;
            }
            s_online = true;
            break;
        case DIAG_WIFI_LOST:
            ++s_lost;
            count_reason(reason);
            if (s_online) s_offline_since_ms = now;
            s_online = false;
            break;
        case DIAG_WIFI_FAILED:
            ++s_failed;
            count_reason(reason);
            break;
        default:
            break;
    }
}

void diag_loop_ms(uint32_t ms) {
    tick_uptime();
    if (ms > s_loop_max_ms) s_loop_max_ms = ms;
    if (ms >= SLOW_LOOP_MS) ++s_slow_loops;
}

void diag_summary(DiagSummary* out) {
    tick_uptime();
    const uint32_t now_s = s_online ? 0 : (millis() - s_offline_since_ms) / 1000;
    out->uptime_s = (uint32_t)(s_up_ms / 1000);
    out->wifi_lost = s_lost;
    out->wifi_failed = s_failed;
    out->offline_s = s_offline_s + now_s;
    out->longest_offline_s = now_s > s_longest_s ? now_s : s_longest_s;
    out->offline_now_s = now_s;
    out->loop_max_ms = s_loop_max_ms;
    out->slow_loops = s_slow_loops;
    out->reset = s_reset;
}

int diag_count() { return s_count; }

const DiagEntry* diag_entry(int i) {
    if (i < 0 || i >= s_count) return nullptr;
    return &s_log[(s_next - s_count + i + DIAG_LOG_LEN) % DIAG_LOG_LEN];
}

int diag_reasons(uint8_t* codes, uint16_t* counts, int max) {
    int n = 0;
    bool used[REASON_SLOTS] = {};
    while (n < max) {   // selection by count, most frequent first
        int best = -1;
        for (int i = 0; i < REASON_SLOTS; ++i)
            if (s_reason_n[i] && !used[i] && (best < 0 || s_reason_n[i] > s_reason_n[best])) best = i;
        if (best < 0) break;
        used[best] = true;
        codes[n] = s_reason[best];
        counts[n++] = s_reason_n[best];
    }
    return n;
}

const char* diag_kind_name(DiagKind k) {
    switch (k) {
        case DIAG_WIFI_UP: return "WIFI UP";
        case DIAG_WIFI_LOST: return "WIFI LOST";
        case DIAG_WIFI_FAILED: return "WIFI FAILED";
        case DIAG_USB_OFF: return "USB POWER OFF";
        case DIAG_USB_ON: return "USB POWER ON";
    }
    return "?";
}

static void print_duration(const char* label, uint32_t s) {
    Serial.printf("%s %lud %02lu:%02lu:%02lu", label, (unsigned long)(s / 86400), (unsigned long)(s / 3600 % 24),
                  (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
}

void diag_print() {
    DiagSummary s;
    diag_summary(&s);
    PowerDiag p;
    power_diag(&p);
    print_duration("diag     up", s.uptime_s);
    Serial.printf(", reset %s, loop max %lu ms, %lu over %lu ms\n", s.reset, (unsigned long)s.loop_max_ms,
                  (unsigned long)s.slow_loops, (unsigned long)SLOW_LOOP_MS);
    Serial.printf("power    USB %u mV now, %u..%u mV, VSYS %u mV now, min %u mV, battery %u mV\n", p.vbus_mv,
                  p.vbus_min_mv, p.vbus_max_mv, p.vsys_mv, p.vsys_min_mv, p.vbat_mv);
    Serial.printf("         USB power lost %lu, back %lu, on battery %lu s, input limit %u mA / %u mV\n",
                  (unsigned long)p.usb_lost, (unsigned long)p.usb_back, (unsigned long)p.battery_s, p.limit_ma,
                  p.limit_mv);
    Serial.printf("wifi     lost %lu, failed %lu, ", (unsigned long)s.wifi_lost, (unsigned long)s.wifi_failed);
    print_duration("offline", s.offline_s);
    print_duration(" in all, longest", s.longest_offline_s);
    print_duration(", now", s.offline_now_s);
    Serial.printf("\nreasons ");
    uint8_t codes[REASON_SLOTS];
    uint16_t counts[REASON_SLOTS];
    const int n = diag_reasons(codes, counts, REASON_SLOTS);
    for (int i = 0; i < n; ++i)
        Serial.printf(" %u %s x%u%s", codes[i], net_reason_name(codes[i]), counts[i], i + 1 < n ? "," : "");
    Serial.printf("%s\nevents   oldest first; USB/VSYS in mV, ps = Wi-Fi power save, host = USB host\n",
                  n ? "" : " none");
    for (int i = 0; i < s_count; ++i) {
        const DiagEntry* e = diag_entry(i);
        char when[24] = "no time";
        if (e->utc) {
            struct tm lt;
            localtime_r(&e->utc, &lt);
            strftime(when, sizeof(when), "%m-%d %H:%M:%S", &lt);
        }
        Serial.printf("  %-14s up %7lus  %-13s", when, (unsigned long)e->uptime_s, diag_kind_name(e->kind));
        if (e->kind == DIAG_WIFI_LOST || e->kind == DIAG_WIFI_FAILED)
            Serial.printf(" %3u %-24s", e->reason, net_reason_name(e->reason));
        else
            Serial.printf(" %-28s", "");
        if (e->rssi) Serial.printf(" %4d dBm", e->rssi);
        else Serial.printf("    - dBm");
        Serial.printf("  USB %4u  VSYS %4u  ps %d  host %d\n", e->vbus_mv, e->vsys_mv, e->power_save, e->usb_host);
    }
}
