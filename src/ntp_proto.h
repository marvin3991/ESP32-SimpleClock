#pragma once
#include <stddef.h>
#include <stdint.h>

// NTP packets (RFC 5905), without any I/O so that the host tests can run it.
// Times are microseconds since 1970-01-01 UTC on the clock being corrected.

static const size_t NTP_PACKET_LEN = 48;

struct NtpSample {
    int64_t offset_us;      // server clock minus ours: ((t2 - t1) + (t3 - t4)) / 2
    int64_t delay_us;       // round trip without the server's own time: (t4 - t1) - (t3 - t2)
    int64_t root_delay_us;  // the server's round trip to its reference clock
    int64_t root_disp_us;   // the server's own worst case error estimate
    uint8_t stratum;
};

enum NtpReply : uint8_t {
    NTP_REPLY_OK,
    NTP_REPLY_OTHER,      // answers another request (e.g. a late one): keep waiting
    NTP_REPLY_BAD,        // malformed or impossible timestamps
    NTP_REPLY_UNSYNCED,   // the server is not synchronised itself, or too far from its source
    NTP_REPLY_KISS,       // kiss-o'-death (RATE, DENY, ...): stop asking this server
};

// A client request whose transmit timestamp carries a random nonce instead of
// our clock: the server echoes it as the origin, which ties the reply to it.
void ntp_make_request(uint8_t pkt[NTP_PACKET_LEN], uint64_t nonce);
// t1: our clock when the request was sent, t4: when the reply arrived. kiss
// receives the 4-letter code of a kiss-o'-death (printable, else '?').
NtpReply ntp_parse_reply(const uint8_t* pkt, size_t len, uint64_t nonce, int64_t t1_us, int64_t t4_us,
                         NtpSample* out, char kiss[5]);
// NTP timestamp (seconds since 1900, 32-bit fraction) to microseconds since 1970.
int64_t ntp_to_unix_us(uint32_t seconds, uint32_t fraction);
// Index of the sample with the shortest round trip (its offset suffers least
// from uneven paths), -1 when n == 0.
int ntp_best_sample(const NtpSample* samples, int n);
// Error bound of a sample's offset from uneven paths: if the whole round trip
// to the server and on to its reference ran one way, delay / 2 + root delay / 2.
// The server's root dispersion is left out: it is a worst case that grows
// between the server's own syncs (tock.stdtime.gov.tw reported 35 ms while it
// agreed with time.stdtime.gov.tw within a few ms), and a rate is measured
// between two syncs that normally come from the same server.
int64_t ntp_error_bound_us(const NtpSample& s);
