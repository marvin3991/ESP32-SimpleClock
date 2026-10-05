#include "face.h"

#include <Arduino.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "generated/font_data.h"
#include "log.h"
#include "qrcodegen.h"
#include "render.h"

// ------------------------------------------------------------------ geometry
// Base positions come from tools/gen_fonts.py (LAYOUT_*), so the preview PNG
// and the firmware share one layout. Pixel shift is added at draw time.

static const int TOAST_CY = (LAYOUT_HOUR_BASE + LAYOUT_MIN_BASE - FONT_MAIN_CAP) / 2;  // row gap
static const int IND_BASE = TOAST_CY + FONT_TEXT_CAP / 2;
// Text in the side column may be wider than the seconds (e.g. "WED", "SYNC"),
// so its redraw rectangles get this much slack on both sides.
static const int COL_SLACK = 24;

// Brightness pill: one segment per level, sized to sit inside the row gap.
static const int SEG_W = 20, SEG_H = 10, SEG_GAP = 6, SEG_PAD_X = 12, SEG_PAD_Y = 8;
static const int BRI_W = BRIGHTNESS_LEVELS * SEG_W + (BRIGHTNESS_LEVELS - 1) * SEG_GAP + 2 * SEG_PAD_X;
static const int BRI_H = SEG_H + 2 * SEG_PAD_Y;
static const int TEXT_TOAST_H = 44, TEXT_TOAST_PAD = 18;

static const uint16_t C_BG = rgb565(COLOR_BG);
static const uint16_t C_MAIN = rgb565(COLOR_MAIN);
static const uint16_t C_ACCENT = rgb565(COLOR_ACCENT);
static const uint16_t C_DIM = rgb565(COLOR_DIM);
static const uint16_t C_WARN = rgb565(COLOR_WARN);
static const uint16_t C_PANEL = rgb565(COLOR_PANEL);
static const uint16_t C_SEG_OFF = rgb565(COLOR_SEG_OFF);
static const uint16_t C_NIGHT = rgb565(COLOR_NIGHT);

static const char* const WEEKDAYS[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};

// Vertical ink extent of each digit font (from the glyph table).
static int s_main_top, s_main_bot, s_sec_top, s_sec_bot;

// ------------------------------------------------------------------ models

struct ClockModel {
    char hh[3], mm[3], ss[3];
    char wd[4], day[3], ampm[3];
    char ind[6];
    uint16_t ind_color;
    int8_t dx, dy;
    bool left;   // side column on the left
};

struct ToastModel {
    Toast kind;
    uint8_t level;
    bool night;
    bool left;   // follows the digit block
    char text[24];
};

static int block_x(bool left) { return left ? LAYOUT_ALT_BLOCK_X : LAYOUT_BLOCK_X; }
static int col_x(bool left) { return left ? LAYOUT_ALT_COL_X : LAYOUT_RIGHT_X; }
static int toast_cx(bool left) { return block_x(left) + LAYOUT_CELL; }   // centre of the digit block

static FaceState s_state;          // state of the frame being drawn
static ClockModel s_clock = {};    // clock content currently on the panel
static ToastModel s_toast = {};    // overlay currently on the panel
static Page s_page = PAGE_CLOCK;
static bool s_has_frame = false;
static uint32_t s_page_hash = 0;

// Wi-Fi QR code (WIFI:T:WPA;S:...;P:...;;) for the setup page.
static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
static bool s_qr_ok = false;
static char s_qr_for[48] = "";

static void ink_extent(const Font& f, int* top, int* bot) {
    *top = 0;
    *bot = 0;
    for (int i = 0; i < f.count; ++i) {
        const Glyph& g = f.glyphs[i];
        if (!g.w) continue;
        *top = min(*top, (int)g.y);
        *bot = max(*bot, (int)g.y + (int)g.h);
    }
}

void face_init() {
    ink_extent(FONT_MAIN, &s_main_top, &s_main_bot);
    ink_extent(FONT_SEC, &s_sec_top, &s_sec_bot);
}

static Rect r_hours(const ClockModel& m) {
    return Rect{(int16_t)(block_x(m.left) + m.dx), (int16_t)(LAYOUT_HOUR_BASE + s_main_top + m.dy),
                (int16_t)(2 * LAYOUT_CELL), (int16_t)(s_main_bot - s_main_top)};
}
static Rect r_minutes(const ClockModel& m) {
    return Rect{(int16_t)(block_x(m.left) + m.dx), (int16_t)(LAYOUT_MIN_BASE + s_main_top + m.dy),
                (int16_t)(2 * LAYOUT_CELL), (int16_t)(s_main_bot - s_main_top)};
}
static Rect r_seconds(const ClockModel& m) {
    return Rect{(int16_t)(col_x(m.left) + m.dx), (int16_t)(LAYOUT_SEC_BASE + s_sec_top + m.dy),
                (int16_t)(2 * LAYOUT_CELL_SEC), (int16_t)(s_sec_bot - s_sec_top)};
}
// Side column above the seconds: weekday, day, AM/PM.
static Rect r_labels(const ClockModel& m) {
    const int x = col_x(m.left) - COL_SLACK, y = LAYOUT_TOP - 8;
    return Rect{(int16_t)(x + m.dx), (int16_t)(y + m.dy), (int16_t)(LAYOUT_RIGHT_W + 2 * COL_SLACK),
                (int16_t)(LAYOUT_HOUR_BASE + 8 - y)};
}
static Rect r_indicator(const ClockModel& m) {
    const int x = col_x(m.left) - COL_SLACK, y = IND_BASE - FONT_TEXT_CAP - 8;
    return Rect{(int16_t)(x + m.dx), (int16_t)(y + m.dy), (int16_t)(LAYOUT_RIGHT_W + 2 * COL_SLACK),
                (int16_t)(FONT_TEXT_CAP + 16)};
}
static Rect r_toast(const ToastModel& t) {
    const int cx = toast_cx(t.left);
    if (t.kind == TOAST_BRIGHTNESS)
        return Rect{(int16_t)(cx - BRI_W / 2), (int16_t)(TOAST_CY - BRI_H / 2), (int16_t)BRI_W, (int16_t)BRI_H};
    if (t.kind == TOAST_TEXT) {
        const int w = font_text_width(FONT_TEXT, t.text) + 2 * TEXT_TOAST_PAD;
        return Rect{(int16_t)(cx - w / 2), (int16_t)(TOAST_CY - TEXT_TOAST_H / 2), (int16_t)w,
                    (int16_t)TEXT_TOAST_H};
    }
    return Rect{0, 0, 0, 0};
}

static void two_digits(char* out, int v) {
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
}

static ClockModel build_clock(const FaceState& s) {
    ClockModel m = {};
    m.dx = s.shift_dx;
    m.dy = s.shift_dy;
    m.left = s.side_left;
    if (s.time_valid) {
        int hour = s.local.tm_hour;
        if (s.h12) {
            strcpy(m.ampm, hour < 12 ? "AM" : "PM");
            hour %= 12;
            if (hour == 0) hour = 12;
        }
        two_digits(m.hh, hour);
        two_digits(m.mm, s.local.tm_min);
        two_digits(m.ss, s.local.tm_sec);
        if (s.show_date) {
            strcpy(m.wd, WEEKDAYS[s.local.tm_wday % 7]);
            two_digits(m.day, s.local.tm_mday);   // always two digits: same width every day
        }
    } else {
        strcpy(m.hh, "--");
        strcpy(m.mm, "--");
        strcpy(m.ss, "--");
    }
    if (s.battery_low) {
        strcpy(m.ind, "BATT");
        m.ind_color = C_WARN;
    } else if (s.sync_stale) {
        strcpy(m.ind, "SYNC");
        m.ind_color = C_ACCENT;
    }
    return m;
}

static ToastModel build_toast(const FaceState& s) {
    ToastModel t = {};
    t.kind = s.toast;
    t.left = s.page == PAGE_CLOCK && s.side_left;
    if (t.kind == TOAST_BRIGHTNESS) {
        t.level = s.toast_level;
        t.night = s.toast_night;
    } else if (t.kind == TOAST_TEXT) {
        strncpy(t.text, s.toast_text, sizeof(t.text) - 1);
    }
    return t;
}

static bool same_toast(const ToastModel& a, const ToastModel& b) {
    return a.kind == b.kind && a.level == b.level && a.night == b.night && a.left == b.left &&
           strcmp(a.text, b.text) == 0;
}

// ------------------------------------------------------------------ drawing

static ClockModel s_draw_clock;   // what draw_clock() paints
static ToastModel s_draw_toast;

static void draw_toast(Canvas& c, const ToastModel& t) {
    if (t.kind == TOAST_NONE) return;
    const Rect r = r_toast(t);
    if (!c.touches(r)) return;
    c.fill_round_rect(r.x, r.y, r.w, r.h, r.h / 2, C_PANEL);
    if (t.kind == TOAST_BRIGHTNESS) {
        const uint16_t on = t.night ? C_NIGHT : C_ACCENT;
        for (int i = 0; i < BRIGHTNESS_LEVELS; ++i) {
            const int x = r.x + SEG_PAD_X + i * (SEG_W + SEG_GAP);
            c.fill_round_rect(x, r.y + SEG_PAD_Y, SEG_W, SEG_H, SEG_H / 2, i < t.level ? on : C_SEG_OFF);
        }
    } else {
        c.text_center(FONT_TEXT, t.text, toast_cx(t.left), TOAST_CY + FONT_TEXT_CAP / 2, C_MAIN);
    }
}

// Outer edge of the side column (screen side), including the pixel shift.
static int side_edge(const ClockModel& m) {
    return (m.left ? col_x(true) : col_x(false) + LAYOUT_RIGHT_W) + m.dx;
}

// Side-column text is placed by its ink, so its outer ink edge sits exactly on
// the column edge (letters' side bearings differ, advances would not line up).
static void side_text(Canvas& c, const ClockModel& m, const Font& f, const char* s, int base, uint16_t color) {
    int x0, x1;
    font_ink_x(f, s, &x0, &x1);
    c.text(f, s, m.left ? side_edge(m) - x0 : side_edge(m) - x1, base + m.dy, color);
}

static void draw_clock(Canvas& c) {
    const ClockModel& m = s_draw_clock;
    const int bx = block_x(m.left) + m.dx;
    c.fill(C_BG);
    if (c.touches(r_hours(m)))
        c.text_cells(FONT_MAIN, m.hh, bx, LAYOUT_HOUR_BASE + m.dy, LAYOUT_CELL, C_MAIN);
    if (c.touches(r_minutes(m)))
        c.text_cells(FONT_MAIN, m.mm, bx, LAYOUT_MIN_BASE + m.dy, LAYOUT_CELL, C_MAIN);
    if (c.touches(r_seconds(m)))
        c.text_cells(FONT_SEC, m.ss, col_x(m.left) + m.dx, LAYOUT_SEC_BASE + m.dy, LAYOUT_CELL_SEC, C_ACCENT);
    if (c.touches(r_labels(m))) {
        side_text(c, m, FONT_LABEL, m.wd, LAYOUT_LABEL_BASE1, C_ACCENT);
        // Two tabular cells, together about as wide as "SUN" (LAYOUT_DATE_REF_W, see tools/gen_fonts.py).
        const int date_x = m.left ? side_edge(m) : side_edge(m) - 2 * LAYOUT_CELL_DATE;
        c.text_cells(FONT_DATE, m.day, date_x, LAYOUT_DATE_BASE + m.dy, LAYOUT_CELL_DATE, C_DIM);
        side_text(c, m, FONT_LABEL, m.ampm, LAYOUT_HOUR_BASE, C_DIM);
    }
    if (m.ind[0] && c.touches(r_indicator(m))) side_text(c, m, FONT_TEXT, m.ind, IND_BASE, m.ind_color);
    draw_toast(c, s_draw_toast);
}

// Draw `s`, dropping trailing characters (and adding "...") if it would be
// wider than max_w.
static void text_fit(Canvas& c, const Font& f, const char* s, int x, int base, int max_w, uint16_t color) {
    if (font_text_width(f, s) <= max_w) {
        c.text(f, s, x, base, color);
        return;
    }
    char buf[STATUS_LINE_LEN + 4];
    size_t n = strnlen(s, STATUS_LINE_LEN);
    const int dots = font_text_width(f, "...");
    while (n > 0) {
        memcpy(buf, s, n);
        buf[n] = 0;
        if (font_text_width(f, buf) + dots <= max_w) break;
        --n;
    }
    strcpy(buf + n, "...");
    c.text(f, buf, x, base, color);
}

static void draw_status(Canvas& c) {
    static const int LX = 32, RIGHT_MARGIN = 28, TOP_BASE = 108, LINE_H = 36, GAP = 16;
    c.fill(C_BG);
    c.text(FONT_LABEL, "STATUS", LX, 66, C_ACCENT);
    // Value column starts after the widest label.
    char labels[STATUS_LINES][12] = {};
    const char* values[STATUS_LINES] = {};
    int label_w = 0;
    for (int i = 0; i < STATUS_LINES; ++i) {
        const char* line = s_state.status[i];
        const char* tab = strchr(line, '\t');
        if (!tab) continue;
        const size_t n = min((size_t)(tab - line), sizeof(labels[i]) - 1);
        memcpy(labels[i], line, n);
        values[i] = tab + 1;
        label_w = max(label_w, font_text_width(FONT_TEXT, labels[i]));
    }
    const int vx = LX + label_w + GAP, max_w = LCD_WIDTH - RIGHT_MARGIN - vx;
    for (int i = 0; i < STATUS_LINES; ++i) {
        if (!values[i]) continue;
        const int base = TOP_BASE + i * LINE_H;
        c.text(FONT_TEXT, labels[i], LX, base, C_DIM);
        text_fit(c, FONT_TEXT, values[i], vx, base, max_w, C_MAIN);
    }
    if (s_state.status_hint[0])
        c.text_center(FONT_TEXT, s_state.status_hint, LCD_WIDTH / 2, TOP_BASE + STATUS_LINES * LINE_H + 14,
                      C_ACCENT);
    draw_toast(c, s_draw_toast);
}

static void build_qr(const FaceState& s) {
    char key[48];
    snprintf(key, sizeof(key), "%s|%s", s.ap_ssid, s.ap_pass);
    if (s_qr_ok && strcmp(key, s_qr_for) == 0) return;
    strncpy(s_qr_for, key, sizeof(s_qr_for) - 1);
    // AP SSID and password are generated from [A-Za-z0-9-] only, so no
    // escaping of ;,:\" is needed in the WIFI: payload.
    char text[96];
    snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", s.ap_ssid, s.ap_pass);
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    s_qr_ok = qrcodegen_encodeText(text, tmp, s_qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, 10,
                                   qrcodegen_Mask_AUTO, true);
    if (!s_qr_ok) LOGE("face", "QR encode failed");
}

static void draw_setup(Canvas& c) {
    const FaceState& s = s_state;
    c.fill(C_BG);
    c.text_center(FONT_LABEL, "WI-FI SETUP", LCD_WIDTH / 2, 58, C_ACCENT);
    if (s_qr_ok) {
        const int n = qrcodegen_getSize(s_qr);
        const int quiet = 2, scale = 5;
        const int size = (n + 2 * quiet) * scale;
        const int x0 = (LCD_WIDTH - size) / 2, y0 = 78;
        const Rect qr_rect = {(int16_t)x0, (int16_t)y0, (int16_t)size, (int16_t)size};
        if (c.touches(qr_rect)) {
            c.fill_rect(x0, y0, size, size, rgb565(COLOR_QR_LIGHT));
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    if (qrcodegen_getModule(s_qr, x, y))
                        c.fill_rect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale, scale,
                                    rgb565(COLOR_QR_DARK));
        }
    }
    // Label left, value right-aligned: rows stay readable whatever the widths.
    const int lx = 52, rx = LCD_WIDTH - 52;
    c.text(FONT_TEXT, "WI-FI", lx, 296, C_DIM);
    c.text_right(FONT_TEXT, s.ap_ssid, rx, 296, C_MAIN);
    c.text(FONT_TEXT, "PASSWORD", lx, 336, C_DIM);
    c.text_right(FONT_TEXT, s.ap_pass, rx, 336, C_MAIN);
    c.text(FONT_TEXT, "THEN OPEN", lx, 376, C_DIM);
    c.text_right(FONT_TEXT, s.ap_ip, rx, 376, C_MAIN);
    if (s.setup_msg[0])
        c.text_center(FONT_TEXT, s.setup_msg, LCD_WIDTH / 2, 444,
                      s.setup_msg_color ? rgb565(s.setup_msg_color) : C_DIM);
    draw_toast(c, s_draw_toast);
}

// FNV-1a over the fields a page shows, to detect "something changed".
static uint32_t fnv(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}

static uint32_t page_hash(const FaceState& s, const ToastModel& t) {
    uint32_t h = 2166136261u;
    h = fnv(h, &t.kind, sizeof(t.kind));
    h = fnv(h, &t.level, sizeof(t.level));
    h = fnv(h, &t.night, sizeof(t.night));
    h = fnv(h, &t.left, sizeof(t.left));
    h = fnv(h, t.text, sizeof(t.text));
    if (s.page == PAGE_STATUS) {
        h = fnv(h, s.status, sizeof(s.status));
        h = fnv(h, s.status_hint, sizeof(s.status_hint));
    }
    if (s.page == PAGE_SETUP) {
        h = fnv(h, s.ap_ssid, sizeof(s.ap_ssid));
        h = fnv(h, s.ap_pass, sizeof(s.ap_pass));
        h = fnv(h, s.ap_ip, sizeof(s.ap_ip));
        h = fnv(h, s.setup_msg, sizeof(s.setup_msg));
        h = fnv(h, &s.setup_msg_color, sizeof(s.setup_msg_color));
    }
    return h;
}

static const Rect FULL = {0, 0, LCD_WIDTH, LCD_HEIGHT};

void face_update(const FaceState& s, bool full) {
    if (!s_has_frame || s.page != s_page) full = true;
    const ToastModel t = build_toast(s);

    if (s.page == PAGE_CLOCK) {
        const ClockModel m = build_clock(s);
        if (m.dx != s_clock.dx || m.dy != s_clock.dy || m.left != s_clock.left) full = true;
        s_draw_clock = m;
        s_draw_toast = t;
        if (full) {
            render_region(FULL, draw_clock);
        } else {
            if (strcmp(m.hh, s_clock.hh)) render_region(r_hours(m), draw_clock);
            if (strcmp(m.mm, s_clock.mm)) render_region(r_minutes(m), draw_clock);
            if (strcmp(m.ss, s_clock.ss)) render_region(r_seconds(m), draw_clock);
            if (strcmp(m.wd, s_clock.wd) || strcmp(m.day, s_clock.day) || strcmp(m.ampm, s_clock.ampm))
                render_region(r_labels(m), draw_clock);
            if (strcmp(m.ind, s_clock.ind) || m.ind_color != s_clock.ind_color)
                render_region(r_indicator(m), draw_clock);
            if (!same_toast(t, s_toast)) render_region(rect_union(r_toast(s_toast), r_toast(t)), draw_clock);
        }
        s_clock = m;
    } else {
        if (s.page == PAGE_SETUP) build_qr(s);
        const uint32_t h = page_hash(s, t);
        if (full || h != s_page_hash) {
            s_state = s;
            s_draw_toast = t;
            render_region(FULL, s.page == PAGE_STATUS ? draw_status : draw_setup);
        }
        s_page_hash = h;
    }
    s_toast = t;
    s_page = s.page;
    s_has_frame = true;
}

void face_screenshot() {
    if (!s_has_frame) {
        LOGE("face", "no frame yet");
        return;
    }
    if (s_page == PAGE_CLOCK) render_screenshot(draw_clock);
    else render_screenshot(s_page == PAGE_STATUS ? draw_status : draw_setup);
}
