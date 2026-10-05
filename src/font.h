#pragma once
#include <stdint.h>

// 8-bit alpha glyph bitmaps produced by tools/gen_fonts.py.
struct Glyph {
    uint32_t offset;  // into Font::bitmap
    uint16_t w, h;    // bitmap size (0 for blank glyphs such as space)
    int16_t  x, y;    // bitmap top-left relative to pen position / baseline
    uint16_t adv;     // horizontal advance
};

struct Font {
    const uint8_t* bitmap;
    const Glyph*   glyphs;
    uint8_t        first;   // first character code in glyphs[]
    uint8_t        count;
    uint16_t       cap;     // cap height in px
    int16_t        ascent;
    int16_t        descent;
};

// Missing characters fall back to '?' (or nothing if that is missing too).
inline const Glyph* font_glyph(const Font& f, char c) {
    uint8_t code = (uint8_t)c;
    if (code >= f.first && code < f.first + f.count) {
        const Glyph* g = &f.glyphs[code - f.first];
        if (g->adv) return g;
    }
    if (c != '?') return font_glyph(f, '?');
    return nullptr;
}

// Horizontal ink extent of `s` relative to the pen start (0, 0 if no ink).
inline void font_ink_x(const Font& f, const char* s, int* x0, int* x1) {
    int pen = 0, lo = 0, hi = 0;
    bool any = false;
    for (; *s; ++s) {
        const Glyph* g = font_glyph(f, *s);
        if (!g) continue;
        if (g->w) {
            const int a = pen + g->x, b = pen + g->x + g->w;
            lo = any ? (a < lo ? a : lo) : a;
            hi = any ? (b > hi ? b : hi) : b;
            any = true;
        }
        pen += g->adv;
    }
    *x0 = lo;
    *x1 = hi;
}

inline int font_text_width(const Font& f, const char* s) {
    int w = 0;
    for (; *s; ++s) {
        const Glyph* g = font_glyph(f, *s);
        if (g) w += g->adv;
    }
    return w;
}
