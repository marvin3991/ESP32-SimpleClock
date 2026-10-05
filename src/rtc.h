#pragma once
#include <stdint.h>
#include <time.h>

// PCF85063 real-time clock, kept in UTC.
enum RtcResult { RTC_OK, RTC_INVALID, RTC_I2C_ERROR };

bool rtc_init();                       // probe + make sure the oscillator runs
RtcResult rtc_read(time_t* utc);       // RTC_INVALID: oscillator stopped / bad fields
bool rtc_write(time_t utc);
time_t utc_from_fields(int year, int month, int day, int hour, int minute, int second);
