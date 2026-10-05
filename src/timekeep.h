#pragma once
#include <stdint.h>
#include <time.h>

enum TimeSource : uint8_t { TIME_NONE, TIME_RTC, TIME_NTP, TIME_MANUAL };

void timekeep_init(const char* tz);   // apply TZ, load RTC time
void timekeep_set_tz(const char* tz);
// Up to three host names / IPs, empty strings skipped. Restarts SNTP if running.
void timekeep_set_servers(const char* const servers[3]);
const char* timekeep_server(int i);   // compacted list, "" past the end
void timekeep_start_ntp();            // call whenever the network comes up
void timekeep_tick();                 // handles SNTP results (RTC drift, writes the RTC)
bool timekeep_valid();
bool timekeep_set_manual(time_t utc); // console / testing
TimeSource timekeep_source();
time_t timekeep_last_sync();          // last successful NTP sync, 0 = never
uint32_t timekeep_sync_count();
bool timekeep_rtc_ok();
// RTC drift correction: Offset steps, measurements so far, and the rate found
// by the last one that had a base (ppm, > 0 = fast) over how many hours.
void timekeep_rtc_drift(int8_t* steps, uint32_t* checks, float* last_ppm, float* last_hours);
bool timekeep_stale(time_t now);      // no NTP sync within SYNC_STALE_S
const char* timekeep_source_name();
