// Unit tests for src/ntp_proto.cpp: timestamp conversion across the 2036 NTP
// era change, the request format, offset and delay arithmetic, the error
// bound, and every kind of reply that must be refused. Built and run by
// tests/run.sh; exits non-zero on a failure.
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "ntp_proto.h"

static int checks = 0, fails = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++checks;                                                         \
        if (!(cond)) {                                                    \
            ++fails;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)
#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        ++checks;                                                                          \
        const long long a_ = (long long)(a), b_ = (long long)(b);                          \
        if (a_ != b_) {                                                                    \
            ++fails;                                                                       \
            printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); \
        }                                                                                  \
    } while (0)

static const long long NTP_TO_UNIX_S = 2208988800LL;   // 1900-01-01 to 1970-01-01

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static void put64(uint8_t* p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}
// Unix microseconds to an NTP timestamp; the seconds wrap at 2^32 like a server's.
static uint64_t ntp_ts(long long unix_us) {
    const long long s = unix_us / 1000000, us = unix_us % 1000000;
    const uint32_t sec = (uint32_t)((unsigned long long)(s + NTP_TO_UNIX_S) & 0xFFFFFFFFULL);
    const uint32_t frac = (uint32_t)(((unsigned long long)us << 32) / 1000000);
    return (uint64_t)sec << 32 | frac;
}

struct Reply {
    uint8_t li = 0, version = 4, mode = 4, stratum = 2;
    uint32_t root_delay = 0, root_disp = 0;   // 16.16 fixed-point seconds
    const char* refid = nullptr;
    uint64_t origin = 0, receive = 0, transmit = 0;
};

static void build(uint8_t* p, const Reply& r) {
    memset(p, 0, 48);
    p[0] = (uint8_t)(r.li << 6 | r.version << 3 | r.mode);
    p[1] = r.stratum;
    put32(p + 4, r.root_delay);
    put32(p + 8, r.root_disp);
    if (r.refid) memcpy(p + 12, r.refid, 4);
    put64(p + 24, r.origin);
    put64(p + 32, r.receive);
    put64(p + 40, r.transmit);
}

static void test_conversion() {
    CHECK_EQ(ntp_to_unix_us((uint32_t)NTP_TO_UNIX_S, 0), 0);   // 1970-01-01
    CHECK_EQ(ntp_to_unix_us(3968000000u, 0x80000000u), (3968000000LL - NTP_TO_UNIX_S) * 1000000 + 500000);
    CHECK_EQ(ntp_to_unix_us(0xFFFFFFFFu, 0), (4294967295LL - NTP_TO_UNIX_S) * 1000000);   // 2036-02-07 06:28:15
    CHECK_EQ(ntp_to_unix_us(0u, 0), (4294967296LL - NTP_TO_UNIX_S) * 1000000);           // era 1 begins
    const long long y2100 = 4102444800LL;   // 2100-01-01, in era 1
    CHECK_EQ(ntp_to_unix_us((uint32_t)((y2100 + NTP_TO_UNIX_S) & 0xFFFFFFFF), 0), y2100 * 1000000);
    CHECK_EQ(ntp_to_unix_us((uint32_t)NTP_TO_UNIX_S, 0xFFFFFFFFu), 999999);   // a fraction just under 1 s
}

static void test_request() {
    uint8_t req[48];
    ntp_make_request(req, 0x0123456789ABCDEFULL);
    CHECK_EQ(req[0], 0x23);   // leap indicator 0, version 4, mode 3 (client)
    CHECK_EQ(req[40], 0x01);  // the nonce as transmit timestamp, big-endian
    CHECK_EQ(req[47], 0xEF);
    int set = 0;
    for (int i = 1; i < 40; ++i) set += req[i] != 0;
    CHECK_EQ(set, 0);         // nothing else: no clock reading leaves the device
}

int main() {
    test_conversion();
    test_request();

    // A normal exchange: our clock is 250 ms behind the server, 7 ms out, 9 ms back.
    const uint64_t nonce = 0xDEADBEEF01020304ULL;
    const long long true_t1 = 1791349844000000LL;   // 2026-10-07
    const long long behind = 250000, up = 7000, down = 9000, hold = 300;
    Reply r;
    r.origin = nonce;
    r.receive = ntp_ts(true_t1 + up);
    r.transmit = ntp_ts(true_t1 + up + hold);
    r.root_delay = (uint32_t)(0.010 * 65536);   // 10 ms
    r.root_disp = (uint32_t)(0.0348 * 65536);   // 34.8 ms, like tock.stdtime.gov.tw
    uint8_t pkt[48];
    build(pkt, r);
    const long long t1 = true_t1 - behind, t4 = true_t1 + up + hold + down - behind;
    NtpSample s;
    char kiss[5];
    CHECK_EQ(ntp_parse_reply(pkt, 48, nonce, t1, t4, &s, kiss), NTP_REPLY_OK);
    // the offset is off by (up - down) / 2 from the truth; the delay excludes the server's hold
    CHECK(s.offset_us >= behind + (up - down) / 2 - 1 && s.offset_us <= behind + (up - down) / 2 + 1);
    CHECK(s.delay_us >= up + down - 1 && s.delay_us <= up + down + 1);
    CHECK_EQ(s.root_delay_us, ((unsigned long long)r.root_delay * 1000000) >> 16);   // 1/65536 s steps
    CHECK_EQ(s.root_disp_us, ((unsigned long long)r.root_disp * 1000000) >> 16);
    CHECK_EQ(s.stratum, 2);
    CHECK(ntp_error_bound_us(s) >= 1000);   // covers the true error, |up - down| / 2
    CHECK_EQ(ntp_error_bound_us(s), (s.delay_us + s.root_delay_us) / 2);
    uint8_t longer[64] = {};   // extension fields after the header are ignored
    memcpy(longer, pkt, 48);
    CHECK_EQ(ntp_parse_reply(longer, 64, nonce, t1, t4, &s, kiss), NTP_REPLY_OK);

    // The worst case, the whole round trip on one path: the bound still covers it.
    for (long long u : {0LL, 16000LL}) {
        Reply w = r;
        w.receive = ntp_ts(true_t1 + u);
        w.transmit = ntp_ts(true_t1 + u + hold);
        build(pkt, w);
        CHECK_EQ(ntp_parse_reply(pkt, 48, nonce, t1, true_t1 + u + hold + (16000 - u) - behind, &s, kiss),
                 NTP_REPLY_OK);
        const long long err = s.offset_us - behind;
        CHECK((err < 0 ? -err : err) <= ntp_error_bound_us(s));
    }

    // A clock still at 1970 (no RTC): an offset of decades, the delay still right.
    build(pkt, r);
    CHECK_EQ(ntp_parse_reply(pkt, 48, nonce, 5000000, 5000000 + up + hold + down, &s, kiss), NTP_REPLY_OK);
    CHECK(s.offset_us > 1791349800000000LL);
    CHECK(s.delay_us >= up + down - 1 && s.delay_us <= up + down + 1);

    // Refused replies.
    CHECK_EQ(ntp_parse_reply(pkt, 47, nonce, t1, t4, &s, kiss), NTP_REPLY_BAD);         // too short
    CHECK_EQ(ntp_parse_reply(pkt, 48, nonce + 1, t1, t4, &s, kiss), NTP_REPLY_OTHER);   // another request
    struct Case {
        const char* what;
        Reply reply;
        NtpReply expect;
    };
    Reply b;
    Case cases[] = {
        {"mode 3", (b = r, b.mode = 3, b), NTP_REPLY_BAD},
        {"mode 5", (b = r, b.mode = 5, b), NTP_REPLY_BAD},
        {"version 2", (b = r, b.version = 2, b), NTP_REPLY_BAD},
        {"version 5", (b = r, b.version = 5, b), NTP_REPLY_BAD},
        {"version 3", (b = r, b.version = 3, b), NTP_REPLY_OK},
        {"leap alarm", (b = r, b.li = 3, b), NTP_REPLY_UNSYNCED},
        {"leap second ahead", (b = r, b.li = 1, b), NTP_REPLY_OK},
        {"stratum 16", (b = r, b.stratum = 16, b), NTP_REPLY_UNSYNCED},
        {"stratum 15", (b = r, b.stratum = 15, b), NTP_REPLY_OK},
        {"no transmit time", (b = r, b.transmit = 0, b), NTP_REPLY_BAD},
        {"no receive time", (b = r, b.receive = 0, b), NTP_REPLY_BAD},
        {"sent before received", (b = r, b.receive = ntp_ts(true_t1 + 5000), b.transmit = ntp_ts(true_t1), b),
         NTP_REPLY_BAD},
        {"root distance 1.6 s", (b = r, b.root_disp = (uint32_t)(1.6 * 65536), b), NTP_REPLY_UNSYNCED},
        {"root distance 1.5 s", (b = r, b.root_delay = (uint32_t)(0.2 * 65536), b.root_disp = (uint32_t)(1.4 * 65536), b),
         NTP_REPLY_OK},
    };
    for (const Case& c : cases) {
        build(pkt, c.reply);
        const NtpReply got = ntp_parse_reply(pkt, 48, nonce, t1, t4, &s, kiss);
        ++checks;
        if (got != c.expect) {
            ++fails;
            printf("FAIL %s: got %d, expected %d\n", c.what, got, c.expect);
        }
    }
    // Kiss-o'-death: the code is returned, made printable.
    b = r;
    b.stratum = 0;
    b.refid = "RATE";
    build(pkt, b);
    CHECK_EQ(ntp_parse_reply(pkt, 48, nonce, t1, t4, &s, kiss), NTP_REPLY_KISS);
    CHECK(strcmp(kiss, "RATE") == 0);
    b.refid = "D\x01N\xff";
    build(pkt, b);
    CHECK_EQ(ntp_parse_reply(pkt, 48, nonce, t1, t4, &s, kiss), NTP_REPLY_KISS);
    CHECK(strcmp(kiss, "D?N?") == 0);
    // Our clock jumped back 1.1 ms during the exchange; -0.5 ms is rounding.
    build(pkt, r);
    CHECK_EQ(ntp_parse_reply(pkt, 48, nonce, t1, t1 + hold - 1100, &s, kiss), NTP_REPLY_BAD);
    CHECK_EQ(ntp_parse_reply(pkt, 48, nonce, t1, t1 + hold - 500, &s, kiss), NTP_REPLY_OK);
    CHECK_EQ(s.delay_us, 0);

    // Era 1: a server in 2040 sends wrapped seconds.
    const long long t2040 = 2208988800LL * 1000000;   // 2040-01-01 is 2208988800 s after 1970
    b = r;
    b.receive = ntp_ts(t2040 + up);
    b.transmit = ntp_ts(t2040 + up + hold);
    build(pkt, b);
    CHECK((b.receive >> 32) < 0x80000000ULL);
    CHECK_EQ(ntp_parse_reply(pkt, 48, nonce, t2040, t2040 + up + hold + down, &s, kiss), NTP_REPLY_OK);
    CHECK(s.offset_us > -2000 && s.offset_us < 2000);

    // The shortest round trip wins; the first of equals.
    NtpSample v[4] = {};
    v[0].delay_us = 30000;
    v[1].delay_us = 12000;
    v[2].delay_us = 12000;
    v[3].delay_us = 50000;
    CHECK_EQ(ntp_best_sample(v, 4), 1);
    CHECK_EQ(ntp_best_sample(v, 1), 0);
    CHECK_EQ(ntp_best_sample(v, 0), -1);

    printf("ntp_proto: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
