#pragma once
#include <stdint.h>
#include <time.h>

// PCF85063 real-time clock, kept in UTC.
enum RtcResult { RTC_OK, RTC_INVALID, RTC_I2C_ERROR };

bool rtc_init();                       // probe + make sure the oscillator runs
RtcResult rtc_read(time_t* utc);       // RTC_INVALID: oscillator stopped / bad fields
// Setting the time accurately (datasheet Rev. 7.3, section 7.2.1.2): STOP
// holds the prescaler in reset while the time is written, and the clock does
// not count until rtc_release(); its first tick follows about half a second
// later (see RTC_FIRST_TICK_US in timekeep.cpp). Always call rtc_release()
// afterwards, also when this fails.
bool rtc_write_stopped(time_t utc);    // years 2000..2099
bool rtc_release();
time_t utc_from_fields(int year, int month, int day, int hour, int minute, int second);
