#include "ntp.h"

#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

// A burst sends BURST_QUERIES requests BURST_GAP_MS apart, the spacing ntpd
// uses for its "iburst" start-up burst; one burst an hour stays far below
// public servers' rate limits. Each request waits up to REPLY_TIMEOUT_MS
// (with Wi-Fi power save on, a reply came up to 122 ms late in a test).
static const int BURST_QUERIES = 4;
static const uint32_t BURST_GAP_MS = 2000;
static const uint32_t REPLY_TIMEOUT_MS = 1000;
static const char NTP_PORT[] = "123";
static const uint32_t TASK_STACK = 6144;   // getaddrinfo + sockets: 1.8 KB used at the peak (measured)
// Above the Arduino loop task (priority 1), so a reply is timestamped as soon
// as lwIP (18) hands it over, even while the loop busy-waits on the RTC.
static const UBaseType_t TASK_PRIORITY = 5;
static const size_t NAME_LEN = 64;

static TaskHandle_t s_task = nullptr;
static SemaphoreHandle_t s_lock = nullptr;   // guards everything below up to s_result
static char s_names[3][NAME_LEN];
static bool s_busy = false, s_ready = false;
static uint32_t s_gen = 0;
static NtpResult s_result;

static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Waits for the reply to one request; false on timeout or a reply that ends
// this request. *stop is set when the server should not be asked again.
static bool await_reply(int sock, uint64_t nonce, int64_t t1, NtpSample* sample, bool* stop, const char* name,
                        char* error, size_t error_len) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(REPLY_TIMEOUT_MS);
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) break;
        const uint32_t left_ms = (deadline - now) * portTICK_PERIOD_MS;
        struct timeval tmo = {(time_t)(left_ms / 1000), (suseconds_t)(left_ms % 1000 * 1000)};
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
        uint8_t reply[NTP_PACKET_LEN + 16];   // extensions are ignored
        const int len = lwip_recv(sock, reply, sizeof(reply), 0);
        const int64_t t4 = now_us();
        if (len < 0) break;   // timeout or socket error
        char kiss[5];
        switch (ntp_parse_reply(reply, (size_t)len, nonce, t1, t4, sample, kiss)) {
            case NTP_REPLY_OK: return true;
            case NTP_REPLY_OTHER: continue;   // e.g. the late reply to an earlier request
            case NTP_REPLY_KISS:
                snprintf(error, error_len, "%.63s: kiss-o'-death %s", name, kiss);
                *stop = true;
                return false;
            case NTP_REPLY_UNSYNCED: snprintf(error, error_len, "%.63s: not synchronised", name); return false;
            default: snprintf(error, error_len, "%.63s: bad reply", name); return false;
        }
    }
    snprintf(error, error_len, "%.63s: no reply", name);
    return false;
}

// Queries one server BURST_QUERIES times; returns the number of usable
// answers, the one with the shortest round trip in *best.
static int query_server(const char* name, NtpSample* best, uint8_t* queries, char* error, size_t error_len) {
    *queries = 0;
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    if (lwip_getaddrinfo(name, NTP_PORT, &hints, &res) != 0 || !res) {
        snprintf(error, error_len, "%.63s: DNS lookup failed", name);
        return 0;
    }
    const int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    // connect() makes the socket accept datagrams from this server only.
    const bool ready = sock >= 0 && lwip_connect(sock, res->ai_addr, res->ai_addrlen) == 0;
    lwip_freeaddrinfo(res);
    if (!ready) {
        if (sock >= 0) lwip_close(sock);
        snprintf(error, error_len, "%.63s: socket failed", name);
        return 0;
    }
    NtpSample samples[BURST_QUERIES];
    int n = 0;
    bool stop = false;
    TickType_t last_send = xTaskGetTickCount();
    for (int q = 0; q < BURST_QUERIES && !stop; ++q) {
        if (q > 0) vTaskDelayUntil(&last_send, pdMS_TO_TICKS(BURST_GAP_MS));
        uint8_t pkt[NTP_PACKET_LEN];
        const uint64_t nonce = (uint64_t)esp_random() << 32 | esp_random();
        ntp_make_request(pkt, nonce);
        const int64_t t1 = now_us();
        ++*queries;
        if (lwip_send(sock, pkt, sizeof(pkt), 0) != (int)sizeof(pkt)) {
            snprintf(error, error_len, "%.63s: send failed", name);
            continue;
        }
        if (await_reply(sock, nonce, t1, &samples[n], &stop, name, error, error_len)) ++n;
    }
    lwip_close(sock);
    const int b = ntp_best_sample(samples, n);
    if (b >= 0) *best = samples[b];
    return n;
}

static void ntp_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char names[3][NAME_LEN];
        NtpResult r = {};
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(names, s_names, sizeof(names));
        r.gen = s_gen;
        xSemaphoreGive(s_lock);
        r.server = -1;
        // Always in the given order: a fallback (pool.ntp.org answered from
        // 262 ms away here) is used only while the servers before it fail.
        for (int i = 0; i < 3 && !r.ok; ++i) {
            if (!names[i][0]) continue;
            r.server = (int8_t)i;
            NtpSample best;
            const int n = query_server(names[i], &best, &r.queries, r.error, sizeof(r.error));
            if (n > 0) {
                r.ok = true;
                r.best = best;
                r.answers = (uint8_t)n;
                r.error[0] = 0;
            }
        }
        if (r.server < 0) snprintf(r.error, sizeof(r.error), "no server set");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_result = r;
        s_ready = true;
        s_busy = false;
        xSemaphoreGive(s_lock);
    }
}

bool ntp_begin() {
    if (s_task) return true;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_lock && xTaskCreate(ntp_task, "ntp", TASK_STACK, nullptr, TASK_PRIORITY, &s_task) != pdPASS) s_task = nullptr;
    return s_task != nullptr;
}

void ntp_set_servers(const char* const servers[3]) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        if (!servers[i] || !servers[i][0]) continue;
        snprintf(s_names[n++], NAME_LEN, "%s", servers[i]);
    }
    for (int i = n; i < 3; ++i) s_names[i][0] = 0;
    xSemaphoreGive(s_lock);
}

bool ntp_start_burst(uint32_t gen) {
    if (!s_task) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool start = !s_busy;
    if (start) {
        s_busy = true;
        s_gen = gen;
    }
    xSemaphoreGive(s_lock);
    if (start) xTaskNotifyGive(s_task);
    return start;
}

bool ntp_take_result(NtpResult* out) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ready = s_ready;
    if (ready) {
        *out = s_result;
        s_ready = false;
    }
    xSemaphoreGive(s_lock);
    return ready;
}
