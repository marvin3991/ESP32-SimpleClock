#pragma once
// All tunables in one place. Every number here is a deliberate choice; the
// comment says what it controls and where the value comes from.

#define FW_VERSION "1.0.0"

// ---------------------------------------------------------------- brightness
// AMOLED brightness register (0x51, DBV 0..255). Roughly geometric steps
// (x1.7..x2.7) because perceived brightness is logarithmic; the panel is
// rated 600 cd/m2 at 255 (Waveshare spec), so level 1 is ~7 cd/m2.
#define BRIGHTNESS_TABLE        {3, 8, 16, 30, 55, 95, 160, 255}
#define BRIGHTNESS_LEVELS       8
#define BRIGHTNESS_DAY_DEFAULT  5      // index -> 95: comfortable indoors
#define BRIGHTNESS_NIGHT_DEFAULT 1     // index -> 8
#define BRIGHTNESS_RAMP_MS      240    // fade length when brightness changes
#define BRIGHTNESS_SAVE_DELAY_MS 5000  // debounce NVS writes (flash wear)

// Night window: brightness switches to the night level inside it.
#define NIGHT_DEFAULT_ENABLED   true
#define NIGHT_DEFAULT_START_MIN (23 * 60)   // 23:00
#define NIGHT_DEFAULT_END_MIN   (7 * 60)    // 07:00

// Screen-off schedule (e.g. nobody home): the panel is switched off inside
// the window. A button press or tap shows the clock for SLEEP_PEEK_MS.
#define SLEEP_DEFAULT_ENABLED   true
#define SLEEP_DEFAULT_START_MIN (3 * 60)    // 03:00
#define SLEEP_DEFAULT_END_MIN   (9 * 60)    // 09:00
#define SLEEP_PEEK_MS           30000

// ---------------------------------------------------------------- burn-in
// Pixel shift: the whole face walks a closed loop over an N x N grid, one
// pixel per step, stepping on every minute change (the minute digits redraw
// then anyway, so the move is not noticed). 12 x 12 = 144 positions, offsets
// -6..+5 px, one full loop every 144 minutes. The digits are 176 px tall, so a
// few pixels of travel spread the wear of their edges. tools/gen_fonts.py
// lays the face out so ink stays clear of the screen edge at every offset.
// The Wi-Fi setup page moves the same way.
#define SHIFT_GRID              12
// Without a valid time ("--") there are no minute changes: step this often.
#define SHIFT_NO_TIME_STEP_MS   60000UL

// Every hour the side column (weekday, day, seconds) changes sides: odd
// hours put it left of the digits, which then move right by the column's
// width plus gap (LAYOUT_ALT_BLOCK_X - LAYOUT_BLOCK_X). The
// hour digits stay put for a whole hour, so moving them every hour keeps
// their strokes from wearing the same pixels hour after hour.
#define SWAP_DEFAULT_ENABLED    true

// Side-column text may be wider than the column (e.g. "WED", "SYNC"); its
// redraw rectangles reach this far past both column edges. tools/gen_fonts.py
// checks that every label fits inside.
#define SIDE_COL_SLACK          24

// The setup page drops to the night brightness after this long without a
// button press or tap (it can stay up for days when there is no Wi-Fi).
#define SETUP_DIM_AFTER_MS      (10UL * 60UL * 1000UL)

// ---------------------------------------------------------------- colours
// AMOLED: black pixels are off. Warm white instead of pure white lowers
// blue sub-pixel load (blue OLED material ages fastest).
#define COLOR_BG        0x000000
#define COLOR_MAIN      0xF4EFE6
#define COLOR_ACCENT    0xFF9F0A
#define COLOR_DIM       0x8E8E93
#define COLOR_WARN      0xFF453A
#define COLOR_OK        0x30D158
#define COLOR_PANEL     0x2C2C2E
#define COLOR_SEG_OFF   0x48484A
#define COLOR_NIGHT     0x5E5CE6   // brightness toast while the night window is active
#define COLOR_QR_LIGHT  0xE5E5EA
#define COLOR_QR_DARK   0x000000

// ---------------------------------------------------------------- timing
#define LOOP_IDLE_MS            10     // main loop poll period
#define BTN_DEBOUNCE_MS         25
#define BTN_KEY_LONG_MS         1500   // KEY long press -> rotation mode
#define BTN_BOOT_LONG_MS        3000   // BOOT long press -> Wi-Fi setup
#define TAP_GUARD_MS            300    // taps closer together than this count once
#define TOAST_MS                1500
#define STATUS_PAGE_MS          10000

// ---------------------------------------------------------------- time sync
#define TZ_DEFAULT              "CST-8"     // Asia/Taipei, no DST
#define NTP_SERVER_1            "tock.stdtime.gov.tw"   // TL (Taiwan) NTP
#define NTP_SERVER_2            "time.stdtime.gov.tw"
#define NTP_SERVER_3            "pool.ntp.org"
#define NTP_INTERVAL_MS         (60UL * 60UL * 1000UL)  // re-sync hourly
#define SYNC_STALE_S            (24L * 60L * 60L)       // "SYNC" warning after 24 h
#define TIME_VALID_EPOCH        1735689600L             // 2025-01-01T00:00:00Z

// ---------------------------------------------------------------- network
#define HOSTNAME                "clock"                 // http://clock.local
#define AP_SSID_PREFIX          "Clock-"
#define WIFI_RETRY_MIN_MS       10000UL
#define WIFI_RETRY_MAX_MS       (5UL * 60UL * 1000UL)
#define SETUP_CONNECT_TIMEOUT_MS 20000UL
#define SETUP_SUCCESS_LINGER_MS 8000UL   // keep AP up so the phone sees "OK"

// ---------------------------------------------------------------- sensors
// Orientation (QMI8658). A clock is moved rarely, so a change must hold for
// a while before the screen rotates.
#define IMU_POLL_MS             100
#define IMU_STABLE_MS           700
#define IMU_TILT_G              0.5f   // ~30 deg from vertical (sin 30 = 0.5)

#define BATTERY_LOW_PCT         15
#define POWER_POLL_MS           50     // PWR key IRQ polling period
#define BATTERY_POLL_MS         5000
