#include "console.h"

#include <Arduino.h>
#include <string.h>
#include <sys/time.h>

#include "app.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "net.h"
#include "orient.h"
#include "rtc.h"
#include "settings.h"
#include "timekeep.h"

static char s_line[96];
static size_t s_len = 0;

static void help() {
    Serial.print(
        "commands:\n"
        "  status                      overall state\n"
        "  shot                        stream the current frame (tools/screenshot.py)\n"
        "  btn boot|key|pwr|touch [short|long]   simulate an input\n"
        "  level <1-8>                 brightness for the current period\n"
        "  rot auto|0|1|2|3            rotation (0 = USB down, 1 left, 2 up, 3 right)\n"
        "  page clock|status|setup\n"
        "  settime <unix-epoch>        set the clock (also writes the RTC)\n"
        "  ntp                         sync with NTP now\n"
        "  rtc                         read the RTC; its next tick vs. the system clock\n"
        "  imu                         accelerometer + quadrant\n"
        "  reboot\n"
        "  factory yes                 erase all settings and reboot\n");
}

static bool parse_button(const char* s, Button* b) {
    if (!strcmp(s, "boot")) *b = BTN_BOOT;
    else if (!strcmp(s, "key")) *b = BTN_KEY;
    else if (!strcmp(s, "pwr")) *b = BTN_PWR;
    else if (!strcmp(s, "touch")) *b = BTN_TOUCH;
    else return false;
    return true;
}

static void run(char* line) {
    char* argv[4] = {};
    int argc = 0;
    for (char* tok = strtok(line, " \t"); tok && argc < 4; tok = strtok(nullptr, " \t")) argv[argc++] = tok;
    if (!argc) return;
    const char* cmd = argv[0];

    if (!strcmp(cmd, "help")) {
        help();
    } else if (!strcmp(cmd, "status")) {
        app_print_status();
    } else if (!strcmp(cmd, "shot")) {
        app_screenshot();
    } else if (!strcmp(cmd, "btn") && argc >= 2) {
        Button b;
        if (!parse_button(argv[1], &b)) return (void)Serial.println("ERR unknown button");
        const bool is_long = argc >= 3 && !strcmp(argv[2], "long");
        if (b == BTN_BOOT || b == BTN_KEY) input_inject(b, IN_PRESS);
        input_inject(b, is_long ? IN_LONG : IN_SHORT);
        Serial.println("OK");
    } else if (!strcmp(cmd, "level") && argc >= 2) {
        const int n = atoi(argv[1]);
        if (n < 1 || n > BRIGHTNESS_LEVELS) return (void)Serial.println("ERR level 1-8");
        app_set_level((uint8_t)(n - 1));
        Serial.println("OK");
    } else if (!strcmp(cmd, "rot") && argc >= 2) {
        if (!strcmp(argv[1], "auto")) app_set_rotation_mode(ROTATION_AUTO);
        else if (argv[1][0] >= '0' && argv[1][0] <= '3' && !argv[1][1]) app_set_rotation_mode(argv[1][0] - '0');
        else return (void)Serial.println("ERR rot auto|0|1|2|3");
        Serial.println("OK");
    } else if (!strcmp(cmd, "page") && argc >= 2) {
        if (!strcmp(argv[1], "clock")) app_show_page(PAGE_CLOCK);
        else if (!strcmp(argv[1], "status")) app_show_page(PAGE_STATUS);
        else if (!strcmp(argv[1], "setup")) app_show_page(PAGE_SETUP);
        else return (void)Serial.println("ERR page clock|status|setup");
        Serial.println("OK");
    } else if (!strcmp(cmd, "settime") && argc >= 2) {
        char* end = nullptr;
        const long long t = strtoll(argv[1], &end, 10);
        if (end == argv[1] || *end) return (void)Serial.println("ERR settime <unix-epoch>");
        Serial.println(timekeep_set_manual((time_t)t) ? "OK" : "ERR epoch outside firmware build date .. 2099");
    } else if (!strcmp(cmd, "ntp")) {
        if (!net_connected()) return (void)Serial.println("ERR wifi not connected");
        timekeep_start_ntp();
        Serial.println("OK");
    } else if (!strcmp(cmd, "rtc")) {
        // Waits for the RTC's next tick and prints where it fell on the system
        // clock: +0..2 ms means in phase (the rest is the I2C read time).
        static const uint32_t RTC_TICK_WAIT_MS = 1100;
        time_t first = 0, utc = 0;
        RtcResult r = rtc_read(&first);
        struct timeval tv = {};
        utc = first;
        const uint32_t start = millis();
        while (r == RTC_OK && utc == first && millis() - start < RTC_TICK_WAIT_MS) {
            r = rtc_read(&utc);
            gettimeofday(&tv, nullptr);
        }
        if (r != RTC_OK) {
            Serial.println(r == RTC_INVALID ? "RTC invalid (oscillator stopped / unset)" : "RTC I2C error");
            return;
        }
        if (utc == first) return (void)Serial.println("ERR RTC did not tick");
        struct tm t;
        gmtime_r(&utc, &t);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
        const long offset_ms = (long)(tv.tv_sec - utc) * 1000L + (long)(tv.tv_usec / 1000);
        Serial.printf("RTC %s UTC (epoch %ld), ticked at system %+ld ms\n", buf, (long)utc, offset_ms);
        static const float PPM_PER_STEP_FAST = 4.069f, PPM_PER_STEP_NORMAL = 4.34f;   // datasheet Table 12
        int8_t steps;
        bool fast;
        if (rtc_get_offset(&steps, &fast))
            Serial.printf("RTC offset %d steps (%s mode), corrects a crystal %+.1f ppm off\n", steps,
                          fast ? "fast" : "normal", steps * (fast ? PPM_PER_STEP_FAST : PPM_PER_STEP_NORMAL));
    } else if (!strcmp(cmd, "imu")) {
        float ax, ay, az;
        if (orient_read(&ax, &ay, &az))
            Serial.printf("accel x=%.2f y=%.2f z=%.2f g, quadrant %u\n", ax, ay, az, orient_quadrant());
        else Serial.println("ERR imu unavailable");
    } else if (!strcmp(cmd, "reboot")) {
        Serial.println("OK");
        Serial.flush();
        delay(100);
        ESP.restart();
    } else if (!strcmp(cmd, "factory")) {
        if (argc < 2 || strcmp(argv[1], "yes")) return (void)Serial.println("ERR type: factory yes");
        settings_factory_reset();
        Serial.println("OK, rebooting");
        Serial.flush();
        delay(100);
        ESP.restart();
    } else {
        Serial.println("ERR unknown command (help)");
    }
}

void console_tick() {
    while (Serial.available()) {
        const int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (s_len) {
                s_line[s_len] = 0;
                s_len = 0;
                run(s_line);
            }
        } else if (s_len < sizeof(s_line) - 1) {
            s_line[s_len++] = (char)c;
        }
    }
}
