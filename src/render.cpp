#include "render.h"

#include <Arduino.h>
#include <math.h>

#include "board.h"
#include "display.h"
#include "log.h"

// 32 lines x 480 px x 2 B = 30 KB in internal SRAM (the C6 has no PSRAM, so
// a full 450 KB frame buffer is impossible; the frame is built in bands).
static const int BAND_LINES = 32;
static uint16_t s_band[LCD_WIDTH * BAND_LINES];

Rect rect_union(const Rect& a, const Rect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    int x0 = min(a.x, b.x), y0 = min(a.y, b.y);
    int x1 = max(a.x + a.w, b.x + b.w), y1 = max(a.y + a.h, b.y + b.h);
    return Rect{(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}

Rect rect_offset(const Rect& r, int dx, int dy) {
    return Rect{(int16_t)(r.x + dx), (int16_t)(r.y + dy), r.w, r.h};
}

// dst + (src - dst) * a, 5-bit alpha on the packed 0x07E0F81F layout.
static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t alpha) {
    uint32_t a = (alpha + 4) >> 3;  // 0..32
    uint32_t d = (dst | ((uint32_t)dst << 16)) & 0x07E0F81F;
    uint32_t s = (src | ((uint32_t)src << 16)) & 0x07E0F81F;
    uint32_t r = ((((s - d) * a) >> 5) + d) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

bool Canvas::touches(const Rect& r) const {
    return !(r.x >= a_.x + a_.w || r.x + r.w <= a_.x || r.y >= a_.y + a_.h || r.y + r.h <= a_.y);
}

void Canvas::fill(uint16_t color) {
    const int n = a_.w * a_.h;
    for (int i = 0; i < n; ++i) buf_[i] = color;
}

void Canvas::fill_rect(int x, int y, int w, int h, uint16_t color) {
    int x0 = max(x, (int)a_.x), x1 = min(x + w, a_.x + a_.w);
    int y0 = max(y, (int)a_.y), y1 = min(y + h, a_.y + a_.h);
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; ++yy) {
        uint16_t* p = buf_ + (yy - a_.y) * a_.w + (x0 - a_.x);
        for (int xx = x0; xx < x1; ++xx) *p++ = color;
    }
}

void Canvas::fill_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    int x0 = max(x, (int)a_.x), x1 = min(x + w, a_.x + a_.w);
    int y0 = max(y, (int)a_.y), y1 = min(y + h, a_.y + a_.h);
    if (x0 >= x1 || y0 >= y1) return;
    const float rf = (float)r;
    for (int yy = y0; yy < y1; ++yy) {
        uint16_t* p = buf_ + (yy - a_.y) * a_.w + (x0 - a_.x);
        const float cy = yy + 0.5f;
        float dy = 0;
        if (cy < y + rf) dy = y + rf - cy;
        else if (cy > y + h - rf) dy = cy - (y + h - rf);
        for (int xx = x0; xx < x1; ++xx, ++p) {
            const float cx = xx + 0.5f;
            float dx = 0;
            if (cx < x + rf) dx = x + rf - cx;
            else if (cx > x + w - rf) dx = cx - (x + w - rf);
            if (dx > 0 && dy > 0) {
                // Corner: coverage from the distance to the corner circle.
                float cover = rf + 0.5f - sqrtf(dx * dx + dy * dy);
                if (cover <= 0) continue;
                if (cover < 1) {
                    *p = blend565(*p, color, (uint8_t)(cover * 255));
                    continue;
                }
            }
            *p = color;
        }
    }
}

void Canvas::glyph(const Font& f, const Glyph& g, int gx, int gy, uint16_t color) {
    int x0 = max(gx, (int)a_.x), x1 = min(gx + (int)g.w, a_.x + a_.w);
    int y0 = max(gy, (int)a_.y), y1 = min(gy + (int)g.h, a_.y + a_.h);
    if (x0 >= x1 || y0 >= y1) return;
    const uint8_t* bitmap = f.bitmap + g.offset;
    for (int yy = y0; yy < y1; ++yy) {
        const uint8_t* src = bitmap + (yy - gy) * g.w + (x0 - gx);
        uint16_t* dst = buf_ + (yy - a_.y) * a_.w + (x0 - a_.x);
        for (int xx = x0; xx < x1; ++xx, ++src, ++dst) {
            const uint8_t a = *src;
            if (a == 0) continue;
            *dst = (a == 255) ? color : blend565(*dst, color, a);
        }
    }
}

int Canvas::text(const Font& f, const char* s, int x, int baseline, uint16_t color) {
    int pen = x;
    for (; *s; ++s) {
        const Glyph* g = font_glyph(f, *s);
        if (!g) continue;
        if (g->w && g->h) glyph(f, *g, pen + g->x, baseline + g->y, color);
        pen += g->adv;
    }
    return pen - x;
}

void Canvas::text_right(const Font& f, const char* s, int right, int baseline, uint16_t color) {
    text(f, s, right - font_text_width(f, s), baseline, color);
}

void Canvas::text_center(const Font& f, const char* s, int cx, int baseline, uint16_t color) {
    text(f, s, cx - font_text_width(f, s) / 2, baseline, color);
}

void Canvas::text_cells(const Font& f, const char* s, int x, int baseline, int cell, uint16_t color) {
    for (int i = 0; s[i]; ++i) {
        const Glyph* g = font_glyph(f, s[i]);
        if (!g) continue;
        const int pen = x + i * cell + (cell - (int)g->adv) / 2;
        if (g->w && g->h) glyph(f, *g, pen + g->x, baseline + g->y, color);
    }
}

// Clip to the screen and widen to even start / even size (CO5300 rule:
// "SC and EC-SC+1 must be divisible by 2", datasheet CASET/RASET).
static bool even_clip(const Rect& in, Rect& out) {
    int x0 = max((int)in.x, 0), y0 = max((int)in.y, 0);
    int x1 = min(in.x + in.w, LCD_WIDTH) - 1, y1 = min(in.y + in.h, LCD_HEIGHT) - 1;
    if (x0 > x1 || y0 > y1) return false;
    x0 &= ~1;
    y0 &= ~1;
    x1 |= 1;
    y1 |= 1;
    out = Rect{(int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0 + 1), (int16_t)(y1 - y0 + 1)};
    return true;
}

void render_region(const Rect& region, DrawFn draw) {
    Rect r;
    if (!even_clip(region, r)) return;
    for (int y = r.y; y < r.y + r.h; y += BAND_LINES) {
        const int h = min(BAND_LINES, r.y + r.h - y);
        Canvas c(s_band, Rect{r.x, (int16_t)y, r.w, (int16_t)h});
        draw(c);
        display_push(r.x, y, r.w, h, s_band);
    }
}

// CRC-32 as zlib.crc32() computes it (IEEE 802.3, reflected), chainable over
// pieces. The screenshot carries it because the device cannot tell when the
// stream was damaged: log output from other tasks can land inside it, and the
// USB driver drops data silently once the host has stalled.
static uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void render_screenshot(DrawFn draw) {
    // Abort if the host stops reading for this long (unplugged, tool killed).
    const uint32_t STALL_TIMEOUT_MS = 2000;
    // HWCDC marks the port disconnected after tx_timeout_ms without progress
    // and then silently drops output (core 3.3.8 HWCDC.cpp, default 100 ms).
    // A 460 KB transfer can hit a short host-side pause, so allow 2 s here.
    const uint32_t TX_TIMEOUT_SHOT_MS = 2000, TX_TIMEOUT_DEFAULT_MS = 100;
    const uint32_t bytes = (uint32_t)LCD_WIDTH * LCD_HEIGHT * 2;
    g_log_mute = true;
    Serial.setTxTimeoutMs(TX_TIMEOUT_SHOT_MS);
    Serial.flush();
    Serial.printf("SHOT %d %d %lu\n", LCD_WIDTH, LCD_HEIGHT, (unsigned long)bytes);
    bool ok = true;
    uint32_t crc = 0;
    for (int y = 0; ok && y < LCD_HEIGHT; y += BAND_LINES) {
        const int h = min(BAND_LINES, LCD_HEIGHT - y);
        Canvas c(s_band, Rect{0, (int16_t)y, LCD_WIDTH, (int16_t)h});
        draw(c);
        const uint8_t* p = (const uint8_t*)s_band;
        size_t left = (size_t)LCD_WIDTH * h * 2;
        crc = crc32_update(crc, p, left);
        uint32_t last_progress = millis();
        while (left) {
            size_t n = Serial.write(p, left);
            if (n == 0) {
                if (millis() - last_progress > STALL_TIMEOUT_MS) {
                    ok = false;
                    break;
                }
                delay(1);
                continue;
            }
            p += n;
            left -= n;
            last_progress = millis();
        }
    }
    if (ok) Serial.printf("\nSHOT_END %08lx\n", (unsigned long)crc);
    else Serial.print("\nSHOT_ABORT\n");
    Serial.flush();
    Serial.setTxTimeoutMs(TX_TIMEOUT_DEFAULT_MS);
    g_log_mute = false;
}
