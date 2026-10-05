#pragma once
#include <stdint.h>
#include <time.h>

// Everything that is drawn is described by FaceState. The app fills it in;
// face_update() compares it with what is on the panel and redraws only the
// parts that changed.

enum Page : uint8_t { PAGE_CLOCK, PAGE_STATUS, PAGE_SETUP };
enum Toast : uint8_t { TOAST_NONE, TOAST_BRIGHTNESS, TOAST_TEXT };

static const int STATUS_LINES = 9;
static const int STATUS_LINE_LEN = 48;

struct FaceState {
    Page page = PAGE_CLOCK;

    // Clock page
    bool time_valid = false;
    struct tm local = {};
    bool h12 = false;
    bool show_date = true;
    bool sync_stale = false;
    bool battery_low = false;
    int8_t shift_dx = 0;
    int8_t shift_dy = 0;
    bool side_left = false;    // side column left of the digits (hourly swap)

    // Overlay
    Toast toast = TOAST_NONE;
    uint8_t toast_level = 0;   // 1..BRIGHTNESS_LEVELS
    bool toast_night = false;
    char toast_text[24] = "";

    // Status page, one "LABEL\tvalue" per line, plus a centred hint below
    char status[STATUS_LINES][STATUS_LINE_LEN] = {};
    char status_hint[STATUS_LINE_LEN] = "";

    // Wi-Fi setup page
    char ap_ssid[24] = "";
    char ap_pass[16] = "";
    char ap_ip[16] = "";
    char setup_msg[48] = "";
    uint32_t setup_msg_color = 0;
};

void face_init();
void face_update(const FaceState& s, bool full);
void face_screenshot();
