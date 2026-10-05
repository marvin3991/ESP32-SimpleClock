#pragma once
#include <stdint.h>

#include "font.h"

constexpr uint16_t rgb565(uint32_t rgb) {
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

struct Rect {
    int16_t x, y, w, h;
    bool empty() const { return w <= 0 || h <= 0; }
};

Rect rect_union(const Rect& a, const Rect& b);
Rect rect_offset(const Rect& r, int dx, int dy);

// One horizontal band of the frame; every draw call is clipped to it.
class Canvas {
public:
    Canvas(uint16_t* buf, const Rect& area) : buf_(buf), a_(area) {}
    const Rect& area() const { return a_; }
    bool touches(const Rect& r) const;

    void fill(uint16_t color);
    void fill_rect(int x, int y, int w, int h, uint16_t color);
    void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);
    // Returns the advance width.
    int text(const Font& f, const char* s, int x, int baseline, uint16_t color);
    void text_right(const Font& f, const char* s, int right, int baseline, uint16_t color);
    void text_center(const Font& f, const char* s, int cx, int baseline, uint16_t color);
    // Monospaced: each character is centred in a cell of `cell` px.
    void text_cells(const Font& f, const char* s, int x, int baseline, int cell, uint16_t color);

private:
    void glyph(const Font& f, const Glyph& g, int gx, int gy, uint16_t color);
    uint16_t* buf_;
    Rect a_;
};

typedef void (*DrawFn)(Canvas& c);

// Render `region` band by band and push it to the panel. The region is
// widened to even coordinates as the panel requires.
void render_region(const Rect& region, DrawFn draw);
// Render the whole frame and stream it on Serial (RGB565 little endian):
//   "SHOT <w> <h> <bytes>\n" <raw bytes> "\nSHOT_END <crc32 of the bytes, 8 hex digits>\n"
void render_screenshot(DrawFn draw);
