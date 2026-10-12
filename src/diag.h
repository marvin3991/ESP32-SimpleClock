#pragma once
#include <stdint.h>
#include <time.h>

// Field diagnostics: Wi-Fi and USB power events, each with the power state at
// that moment, and counters since start-up. Made for a clock that dropped off
// Wi-Fi for hours on a monitor's USB port while it connected at once on a
// computer's. Kept in RAM only: a restart clears them (the reset reason of
// that restart is kept). Read with the console ("diag", "status") or the
// settings page.
enum DiagKind : uint8_t { DIAG_WIFI_UP, DIAG_WIFI_LOST, DIAG_WIFI_FAILED, DIAG_USB_OFF, DIAG_USB_ON };
static const int DIAG_LOG_LEN = 32;   // events kept, the oldest dropped first
static const uint8_t DIAG_REASON_OTHER = 255;   // failure codes beyond the 7 counted separately
static const uint8_t DIAG_REASON_REFUSED = 254; // the Wi-Fi driver refused the connect request

struct DiagEntry {
    time_t utc;          // 0 while the clock had no valid time
    uint32_t uptime_s;
    DiagKind kind;
    uint8_t reason;      // Wi-Fi driver code; 0 = the attempt timed out, DIAG_REASON_REFUSED
    int8_t rssi;         // dBm, 0 = unknown
    bool power_save;     // Wi-Fi modem sleep was on
    bool usb_host;       // a USB host was attached (not just a charger)
    uint16_t vbus_mv;    // USB input, 0 = none
    uint16_t vsys_mv;    // system rail
};

struct DiagSummary {
    uint32_t uptime_s;
    uint32_t wifi_lost, wifi_failed;
    uint32_t offline_s;           // in all since start-up, the current stretch included
    uint32_t longest_offline_s;
    uint32_t offline_now_s;       // 0 while connected
    uint32_t loop_max_ms, slow_loops;
    const char* reset;
};

void diag_init(const char* reset_reason);
// rssi: dBm, 0 = unknown. reason: as in DiagEntry.
void diag_event(DiagKind kind, uint8_t reason, int8_t rssi);
void diag_loop_ms(uint32_t ms);   // one pass of the main loop; also keeps the uptime
void diag_summary(DiagSummary* out);
int diag_count();                          // events kept (the newest DIAG_LOG_LEN)
const DiagEntry* diag_entry(int i);        // 0 = oldest
int diag_reasons(uint8_t* codes, uint16_t* counts, int max);   // failure codes, most frequent first
const char* diag_kind_name(DiagKind k);
void diag_print();                         // console "diag"
