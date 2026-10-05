#include "rtc.h"

#include <Wire.h>

#include "board.h"
#include "log.h"

// Register map from the NXP PCF85063A datasheet (section 8.2).
static const uint8_t REG_CONTROL_1 = 0x00;
static const uint8_t REG_RAM = 0x03;       // one free battery-backed byte
static const uint8_t REG_SECONDS = 0x04;   // bit 7 = OS (oscillator stopped)
// Written to REG_RAM together with the time. A chip that was never set by
// this firmware (factory state, another firmware) is not trusted: the board
// arrived with an RTC reading 2056 and a clear OS flag.
static const uint8_t RAM_MAGIC = 0xC7;
static const uint8_t CTRL1_STOP = 0x20;
static const uint8_t CTRL1_12_24 = 0x02;   // 1 = 12-hour mode
static const uint8_t SECONDS_OS = 0x80;

static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static bool read_regs(uint8_t reg, uint8_t* buf, size_t n) {
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint16_t)PCF85063_ADDR, n) != n) return false;
    for (size_t i = 0; i < n; ++i) buf[i] = (uint8_t)Wire.read();
    return true;
}

static bool write_regs(uint8_t reg, const uint8_t* buf, size_t n) {
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(reg);
    Wire.write(buf, n);
    return Wire.endTransmission() == 0;
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
time_t utc_from_fields(int y, int m, int d, int hh, int mm, int ss) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + hh * 3600 + mm * 60 + ss;
}

bool rtc_init() {
    uint8_t ctrl;
    if (!read_regs(REG_CONTROL_1, &ctrl, 1)) {
        LOGE("rtc", "PCF85063 not responding");
        return false;
    }
    // Clock must run (STOP=0) in 24-hour mode; keep CAP_SEL etc. untouched.
    if (ctrl & (CTRL1_STOP | CTRL1_12_24)) {
        uint8_t fixed = ctrl & ~(CTRL1_STOP | CTRL1_12_24);
        if (!write_regs(REG_CONTROL_1, &fixed, 1)) {
            LOGE("rtc", "control write failed");
            return false;
        }
        LOGI("rtc", "control 0x%02x -> 0x%02x", ctrl, fixed);
    }
    return true;
}

RtcResult rtc_read(time_t* utc) {
    uint8_t ram_and_time[8];
    if (!read_regs(REG_RAM, ram_and_time, sizeof(ram_and_time))) return RTC_I2C_ERROR;
    const uint8_t* r = ram_and_time + 1;
    if (ram_and_time[0] != RAM_MAGIC) return RTC_INVALID;   // never set by us
    if (r[0] & SECONDS_OS) return RTC_INVALID;              // power was lost since last set
    const int sec = bcd2dec(r[0] & 0x7F), min = bcd2dec(r[1] & 0x7F), hour = bcd2dec(r[2] & 0x3F);
    const int day = bcd2dec(r[3] & 0x3F), month = bcd2dec(r[5] & 0x1F), year = 2000 + bcd2dec(r[6]);
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 || month < 1 || month > 12)
        return RTC_INVALID;
    *utc = utc_from_fields(year, month, day, hour, min, sec);
    return RTC_OK;
}

bool rtc_write(time_t utc) {
    struct tm t;
    gmtime_r(&utc, &t);
    if (t.tm_year + 1900 < 2000 || t.tm_year + 1900 > 2099) return false;
    const uint8_t r[8] = {
        RAM_MAGIC,
        dec2bcd((uint8_t)t.tm_sec),   // OS bit cleared
        dec2bcd((uint8_t)t.tm_min),
        dec2bcd((uint8_t)t.tm_hour),
        dec2bcd((uint8_t)t.tm_mday),
        (uint8_t)t.tm_wday,
        dec2bcd((uint8_t)(t.tm_mon + 1)),
        dec2bcd((uint8_t)(t.tm_year + 1900 - 2000)),
    };
    return write_regs(REG_RAM, r, sizeof(r));
}
