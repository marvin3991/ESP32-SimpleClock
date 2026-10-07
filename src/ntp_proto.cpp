#include "ntp_proto.h"

#include <string.h>

// RFC 5905: header fields (section 7.3) and limits (section 7.2).
static const uint8_t NTP_VERSION = 4;
static const uint8_t MODE_CLIENT = 3, MODE_SERVER = 4;
static const uint8_t LI_ALARM = 3;          // leap indicator: clock not synchronised
static const uint8_t STRATUM_MAX = 15;      // 0 = kiss-o'-death, 16 = unsynchronised
static const int64_t MAX_ROOT_DIST_US = 1500000;   // MAXDIST, 1.5 s
// Field offsets in the 48-byte header.
static const size_t ROOT_DELAY_AT = 4, ROOT_DISP_AT = 8, REFID_AT = 12;
static const size_t ORIGIN_AT = 24, RECEIVE_AT = 32, TRANSMIT_AT = 40;
// Seconds from 1900-01-01 (NTP epoch) to 1970-01-01, and the length of an NTP
// era: era 0 ends 2036-02-07, the 32-bit seconds then start again from 0.
static const int64_t NTP_TO_UNIX_S = 2208988800LL;
static const int64_t NTP_ERA_S = 4294967296LL;
// Rounding and timer granularity can make a fast exchange look a little
// negative; anything below this means a clock jumped during the exchange.
static const int64_t MIN_DELAY_US = -1000;

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// 16.16 fixed-point seconds (root delay, root dispersion) to microseconds.
static int64_t short_to_us(uint32_t v) { return (int64_t)(((uint64_t)v * 1000000) >> 16); }

void ntp_make_request(uint8_t pkt[NTP_PACKET_LEN], uint64_t nonce) {
    memset(pkt, 0, NTP_PACKET_LEN);
    pkt[0] = (uint8_t)(NTP_VERSION << 3 | MODE_CLIENT);   // leap indicator 0
    for (int i = 0; i < 8; ++i) pkt[TRANSMIT_AT + i] = (uint8_t)(nonce >> (56 - 8 * i));
}

int64_t ntp_to_unix_us(uint32_t seconds, uint32_t fraction) {
    int64_t s = (int64_t)seconds - NTP_TO_UNIX_S;
    // Seconds below 2^31 would be before 1968 in era 0: they are era 1
    // (2036..2104), which covers every time this clock accepts.
    if (seconds < 0x80000000u) s += NTP_ERA_S;
    return s * 1000000 + (int64_t)(((uint64_t)fraction * 1000000) >> 32);
}

NtpReply ntp_parse_reply(const uint8_t* p, size_t len, uint64_t nonce, int64_t t1_us, int64_t t4_us,
                         NtpSample* out, char kiss[5]) {
    kiss[0] = 0;
    if (len < NTP_PACKET_LEN) return NTP_REPLY_BAD;
    if (be64(p + ORIGIN_AT) != nonce) return NTP_REPLY_OTHER;
    const uint8_t li = p[0] >> 6, version = (p[0] >> 3) & 7, mode = p[0] & 7, stratum = p[1];
    if (mode != MODE_SERVER || version < 3 || version > NTP_VERSION) return NTP_REPLY_BAD;
    if (stratum == 0) {
        for (int i = 0; i < 4; ++i) {
            const uint8_t c = p[REFID_AT + i];
            kiss[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        kiss[4] = 0;
        return NTP_REPLY_KISS;
    }
    if (li == LI_ALARM || stratum > STRATUM_MAX) return NTP_REPLY_UNSYNCED;
    const uint32_t rx_s = be32(p + RECEIVE_AT), rx_f = be32(p + RECEIVE_AT + 4);
    const uint32_t tx_s = be32(p + TRANSMIT_AT), tx_f = be32(p + TRANSMIT_AT + 4);
    if ((rx_s == 0 && rx_f == 0) || (tx_s == 0 && tx_f == 0)) return NTP_REPLY_BAD;
    const int64_t t2 = ntp_to_unix_us(rx_s, rx_f), t3 = ntp_to_unix_us(tx_s, tx_f);
    const int64_t delay = (t4_us - t1_us) - (t3 - t2);
    if (t3 < t2 || delay < MIN_DELAY_US) return NTP_REPLY_BAD;
    const int64_t root_delay = short_to_us(be32(p + ROOT_DELAY_AT)), root_disp = short_to_us(be32(p + ROOT_DISP_AT));
    if (root_delay / 2 + root_disp > MAX_ROOT_DIST_US) return NTP_REPLY_UNSYNCED;
    out->offset_us = ((t2 - t1_us) + (t3 - t4_us)) / 2;
    out->delay_us = delay < 0 ? 0 : delay;
    out->root_delay_us = root_delay;
    out->root_disp_us = root_disp;
    out->stratum = stratum;
    return NTP_REPLY_OK;
}

int ntp_best_sample(const NtpSample* samples, int n) {
    int best = -1;
    for (int i = 0; i < n; ++i)
        if (best < 0 || samples[i].delay_us < samples[best].delay_us) best = i;
    return best;
}

int64_t ntp_error_bound_us(const NtpSample& s) { return (s.delay_us + s.root_delay_us) / 2; }
