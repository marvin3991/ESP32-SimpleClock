#pragma once
#include <stdint.h>

#include "ntp_proto.h"

// NTP client. A background task runs one burst per request: a few queries to
// the first server that answers (in the order given), of which the answer
// with the shortest round trip wins. The loop picks the result up; the task
// never sets the clock itself.
struct NtpResult {
    bool ok;
    NtpSample best;     // the answer with the shortest round trip
    uint8_t answers;    // usable answers from that server
    uint8_t queries;    // queries sent to it
    int8_t server;      // index of that server, or of the last one tried; -1 = none
    uint32_t gen;       // as passed to ntp_start_burst()
    char error[96];     // why the burst failed (last server tried, name up to 63); empty when ok
};

bool ntp_begin();                                     // starts the task; false without memory
void ntp_set_servers(const char* const servers[3]);   // copied; empty strings are skipped
// Starts a burst; false while one runs or without the task. gen is handed
// back in the result so the caller can tell whether its clock was set in
// between (the result is then stale).
bool ntp_start_burst(uint32_t gen);
bool ntp_take_result(NtpResult* out);                 // true once per finished burst
