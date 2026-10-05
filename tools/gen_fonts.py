#!/usr/bin/env python3
"""Rasterise the clock font into 8-bit alpha bitmaps and compute the face layout.

Usage:
    python tools/gen_fonts.py                 # writes src/generated/font_data.{h,cpp}
    python tools/gen_fonts.py --preview a.png # also renders a layout preview
    python tools/gen_fonts.py --preview a.png --alt   # side column on the left

Digits use tabular figures so every digit has the same advance width; that is
what keeps the hour and minute columns aligned. If the font's default digits
are not tabular, its OpenType `tnum` substitutions are applied.
Layout numbers are computed here once and emitted as constants, so the preview
PNG and the firmware share exactly the same geometry.
"""
import argparse
import datetime
import io
import os
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_TTF = os.path.join(ROOT, "fonts", "MPLUSRounded1c-Black.ttf")   # SIL OFL 1.1
OUT_DIR = os.path.join(ROOT, "src", "generated")

SCREEN = 480

# Cap heights in pixels. Font size (em) is derived from the font's cap height.
CAP_MAIN = 176        # hour / minute digits
CAP_SEC = 44          # seconds
CAP_LABEL = 25        # weekday (and AM/PM) next to the hours
CAP_TEXT = 22         # UI text: setup page, status page, toasts
# The day of month always has two tabular digits ("04", "31"); its size is
# the one whose two digit cells come closest to this weekday's ink width.
DATE_WIDTH_REF = "SUN"

ROW_GAP = 30          # px between hour baseline and minute cap top
COL_GAP = 12          # px between minute block and seconds column
LABEL_LINE_GAP = 12   # px between weekday baseline and the date's cap top

WEEKDAYS = ("SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT")
# Ink must stay at least this far from the screen edge at the largest pixel
# shift (offsets -N/2..N/2-1 with N = SHIFT_GRID in src/config.h).
MIN_EDGE_MARGIN = 12

DIGITS = "0123456789-"
ASCII = "".join(chr(c) for c in range(0x20, 0x7F))


def tnum_mapping(font):
    """Glyph substitutions of the font's OpenType `tnum` feature (single substitution)."""
    mapping = {}
    if "GSUB" not in font:
        return mapping
    gsub = font["GSUB"].table
    lookups = {i for rec in gsub.FeatureList.FeatureRecord if rec.FeatureTag == "tnum"
               for i in rec.Feature.LookupListIndex}
    for index in sorted(lookups):
        lookup = gsub.LookupList.Lookup[index]
        for sub in lookup.SubTable:
            sub = sub.ExtSubTable if lookup.LookupType == 7 else sub
            mapping.update(getattr(sub, "mapping", None) or {})
    return mapping


def load_font_bytes(ttf_path, tabular):
    """Return TTF bytes; with tabular=True digits 0-9 are mapped to tabular glyphs."""
    font = TTFont(ttf_path)
    if tabular:
        mapping = tnum_mapping(font)
        for table in font["cmap"].tables:
            if not table.isUnicode():
                continue
            for code in range(ord("0"), ord("9") + 1):
                glyph = table.cmap.get(code)
                if glyph in mapping:
                    table.cmap[code] = mapping[glyph]
    buf = io.BytesIO()
    font.save(buf)
    return buf.getvalue()


def cap_height_units(ttf_path):
    """Units per em and cap height (OS/2 sCapHeight, else the height of "H")."""
    font = TTFont(ttf_path)
    cap = getattr(font["OS/2"], "sCapHeight", 0)
    if not cap:
        cap = font["glyf"][font.getBestCmap()[ord("H")]].yMax
    return font["head"].unitsPerEm, cap


class RasterFont:
    """A set of glyphs rendered at one pixel size."""

    def __init__(self, name, font_bytes, units_per_em, cap_units, cap_px, chars):
        self.name = name
        self.chars = chars
        self.size_px = round(cap_px * units_per_em / cap_units)
        self.pil = ImageFont.truetype(io.BytesIO(font_bytes), self.size_px)
        ascent, descent = self.pil.getmetrics()
        self.ascent = ascent
        self.descent = descent
        self.cap = cap_px
        self.glyphs = {}
        for ch in chars:
            self.glyphs[ch] = self._render(ch)

    def _render(self, ch):
        adv = round(self.pil.getlength(ch))
        x0, y0, x1, y1 = self.pil.getbbox(ch, anchor="ls")
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        if w == 0 or h == 0:
            return {"w": 0, "h": 0, "x": 0, "y": 0, "adv": adv, "data": b""}
        img = Image.new("L", (w, h), 0)
        ImageDraw.Draw(img).text((-x0, -y0), ch, font=self.pil, fill=255, anchor="ls")
        return {"w": w, "h": h, "x": x0, "y": y0, "adv": adv, "data": img.tobytes()}

    def text_width(self, text):
        return sum(self.glyphs[c]["adv"] for c in text)


def ink_box(font, text):
    """Ink bounds of `text` laid out by advances (no kerning, like the firmware)."""
    pen, x0, y0, x1, y1 = 0, 1 << 30, 1 << 30, -(1 << 30), -(1 << 30)
    for ch in text:
        g = font.glyphs[ch]
        if g["w"]:
            x0, x1 = min(x0, pen + g["x"]), max(x1, pen + g["x"] + g["w"])
            y0, y1 = min(y0, g["y"]), max(y1, g["y"] + g["h"])
        pen += g["adv"]
    return x0, y0, x1, y1


def pick_date_cap(tab_bytes, units_per_em, cap_units, target_w):
    """Cap height whose two tabular digit cells add up closest to target_w."""
    best = None
    for cap in range(20, 61):
        size = round(cap * units_per_em / cap_units)
        adv = round(ImageFont.truetype(io.BytesIO(tab_bytes), size).getlength("0"))
        err = abs(2 * adv - target_w)
        if best is None or err < best[0]:
            best = (err, cap)
    return best[1]


def shift_range():
    """Pixel-shift offsets used by the firmware, read from src/config.h."""
    with open(os.path.join(ROOT, "src", "config.h"), encoding="utf-8") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "#define" and parts[1] == "SHIFT_GRID":
                n = int(parts[2])
                return -(n // 2), n // 2 - 1
    sys.exit("SHIFT_GRID not found in src/config.h")


def edge_margins(fonts, layout):
    """Smallest ink distance to each screen edge over every digit, label,
    side and pixel-shift offset the firmware can draw."""
    by = {f.name: f for f in fonts}
    boxes = []

    def add(font, text, pen_x, base):
        x0, y0, x1, y1 = ink_box(font, text)
        if x1 > x0:
            boxes.append((pen_x + x0, base + y0, pen_x + x1, base + y1))

    for left in (False, True):
        bx = layout["ALT_BLOCK_X"] if left else layout["BLOCK_X"]
        cx = layout["ALT_COL_X"] if left else layout["RIGHT_X"]
        edge = cx if left else cx + layout["RIGHT_W"]
        for d in "0123456789-":
            for i in range(2):
                for font, base, cell, x in ((by["MAIN"], layout["HOUR_BASE"], layout["CELL"], bx),
                                            (by["MAIN"], layout["MIN_BASE"], layout["CELL"], bx),
                                            (by["SEC"], layout["SEC_BASE"], layout["CELL_SEC"], cx),
                                            (by["DATE"], layout["DATE_BASE"], layout["CELL_DATE"],
                                             edge if left else edge - 2 * layout["CELL_DATE"])):
                    if d in font.glyphs:
                        add(font, d, x + i * cell + (cell - font.glyphs[d]["adv"]) // 2, base)
        for text, font in [(w, by["LABEL"]) for w in WEEKDAYS + ("AM", "PM")] + [(w, by["TEXT"]) for w in ("SYNC", "BATT")]:
            x0, _, x1, _ = ink_box(font, text)
            add(font, text, edge - x0 if left else edge - x1, layout["LABEL_BASE1"])
    lo, hi = shift_range()
    return {
        "left": min(b[0] for b in boxes) + lo,
        "right": SCREEN - (max(b[2] for b in boxes) + hi),
        "top": min(b[1] for b in boxes) + lo,
        "bottom": SCREEN - (max(b[3] for b in boxes) + hi),
    }


def compute_layout(main, sec, label, date):
    """All positions are for pixel-shift (0, 0); the firmware adds the shift."""
    cell = main.glyphs["0"]["adv"]
    cell_s = sec.glyphs["0"]["adv"]
    block_w = 2 * cell
    right_w = 2 * cell_s
    group_w = block_w + COL_GAP + right_w
    block_x = (SCREEN - group_w) // 2
    right_x = block_x + block_w + COL_GAP

    total_h = CAP_MAIN + ROW_GAP + CAP_MAIN
    top = (SCREEN - total_h) // 2
    hour_base = top + CAP_MAIN
    min_base = hour_base + ROW_GAP + CAP_MAIN

    label_base1 = top + CAP_LABEL
    date_top = label_base1 + LABEL_LINE_GAP
    ref_x0, _, ref_x1, _ = ink_box(label, DATE_WIDTH_REF)
    # Mirrored arrangement (used every other hour): side column first.
    alt_col_x = block_x
    alt_block_x = block_x + right_w + COL_GAP
    return {
        "CELL": cell,
        "CELL_SEC": cell_s,
        "BLOCK_X": block_x,
        "ALT_BLOCK_X": alt_block_x,
        "ALT_COL_X": alt_col_x,
        "HOUR_BASE": hour_base,
        "MIN_BASE": min_base,
        "RIGHT_X": right_x,
        "RIGHT_W": right_w,
        "SEC_BASE": min_base,
        "LABEL_BASE1": label_base1,
        "DATE_BASE": date_top + date.cap,
        "CELL_DATE": date.glyphs["0"]["adv"],
        "DATE_REF_W": ref_x1 - ref_x0,
        "TOP": top,
    }


# ---------------------------------------------------------------- emit C

def emit(fonts, layout, ttf_name):
    os.makedirs(OUT_DIR, exist_ok=True)
    stamp = "// Generated by tools/gen_fonts.py from %s - do not edit by hand.\n" % ttf_name
    h = [stamp, "#pragma once\n", "#include <stdint.h>\n", '#include "../font.h"\n\n']
    for f in fonts:
        h.append("extern const Font FONT_%s;  // size %d px\n" % (f.name, f.size_px))
        h.append("#define FONT_%s_CAP %d\n" % (f.name, f.cap))
    h.append("\n// Face layout at pixel-shift (0,0), screen %dx%d\n" % (SCREEN, SCREEN))
    for key, val in layout.items():
        h.append("#define LAYOUT_%s %d\n" % (key, val))
    with open(os.path.join(OUT_DIR, "font_data.h"), "w", encoding="utf-8") as fh:
        fh.write("".join(h))

    c = [stamp, '#include "font_data.h"\n\n']
    total = 0
    for f in fonts:
        blob = bytearray()
        rows = []
        first, last = min(map(ord, f.chars)), max(map(ord, f.chars))
        for code in range(first, last + 1):
            ch = chr(code)
            g = f.glyphs.get(ch)
            if g is None:
                rows.append("    {0, 0, 0, 0, 0, 0},  // %r (absent)\n" % ch)
                continue
            rows.append("    {%d, %d, %d, %d, %d, %d},  // %r\n"
                        % (len(blob), g["w"], g["h"], g["x"], g["y"], g["adv"], ch))
            blob += g["data"]
        total += len(blob)
        c.append("static const uint8_t %s_bitmap[%d] = {\n" % (f.name.lower(), len(blob)))
        for i in range(0, len(blob), 24):
            c.append("    " + ",".join(str(b) for b in blob[i:i + 24]) + ",\n")
        c.append("};\n\n")
        c.append("static const Glyph %s_glyphs[%d] = {\n" % (f.name.lower(), last - first + 1))
        c.extend(rows)
        c.append("};\n\n")
        c.append("const Font FONT_%s = {%s_bitmap, %s_glyphs, %d, %d, %d, %d, %d};\n\n"
                 % (f.name, f.name.lower(), f.name.lower(), first, last - first + 1,
                    f.cap, f.ascent, f.descent))
    with open(os.path.join(OUT_DIR, "font_data.cpp"), "w", encoding="utf-8") as fh:
        fh.write("".join(c))
    return total


# ---------------------------------------------------------------- preview

def hex_rgb(value):
    return ((value >> 16) & 255, (value >> 8) & 255, value & 255)


def draw_text(img, font, text, x, base, color):
    """Same blending rule as the firmware: dst = dst + (fg - dst) * a / 255."""
    px = img.load()
    pen = x
    for ch in text:
        g = font.glyphs[ch]
        if g["w"]:
            gx, gy = pen + g["x"], base + g["y"]
            data = g["data"]
            for yy in range(g["h"]):
                for xx in range(g["w"]):
                    a = data[yy * g["w"] + xx]
                    if not a:
                        continue
                    X, Y = gx + xx, gy + yy
                    if 0 <= X < SCREEN and 0 <= Y < SCREEN:
                        r, gg, b = px[X, Y]
                        px[X, Y] = (r + (color[0] - r) * a // 255,
                                    gg + (color[1] - gg) * a // 255,
                                    b + (color[2] - b) * a // 255)
        pen += g["adv"]


def preview(path, fonts, layout, alt=False, hh="10", mm="08", ss="42", wd="SUN", day="4"):
    by = {f.name: f for f in fonts}
    img = Image.new("RGB", (SCREEN, SCREEN), (0, 0, 0))
    main, sec, label = by["MAIN"], by["SEC"], by["LABEL"]
    white, accent, grey = hex_rgb(0xF4EFE6), hex_rgb(0xFF9F0A), hex_rgb(0x8E8E93)
    block_x = layout["ALT_BLOCK_X"] if alt else layout["BLOCK_X"]
    col_x = layout["ALT_COL_X"] if alt else layout["RIGHT_X"]
    for i, ch in enumerate(hh):
        draw_text(img, main, ch, block_x + i * layout["CELL"], layout["HOUR_BASE"], white)
    for i, ch in enumerate(mm):
        draw_text(img, main, ch, block_x + i * layout["CELL"], layout["MIN_BASE"], white)
    for i, ch in enumerate(ss):
        draw_text(img, sec, ch, col_x + i * layout["CELL_SEC"], layout["SEC_BASE"], accent)
    # Weekday ink and the two date cells both end exactly at the column's outer edge.
    edge = col_x if alt else col_x + layout["RIGHT_W"]
    wx0, _, wx1, _ = ink_box(label, wd)
    draw_text(img, label, wd, edge - wx0 if alt else edge - wx1, layout["LABEL_BASE1"], accent)
    cell = layout["CELL_DATE"]
    dx = edge if alt else edge - 2 * cell
    date = by["DATE"]
    for i, ch in enumerate("%02d" % int(day)):
        g = date.glyphs[ch]
        draw_text(img, date, ch, dx + i * cell + (cell - g["adv"]) // 2, layout["DATE_BASE"], grey)
    img.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ttf", default=DEFAULT_TTF)
    ap.add_argument("--preview", help="write a layout preview PNG")
    ap.add_argument("--alt", action="store_true", help="preview the mirrored (side column left) layout")
    ap.add_argument("--weekday", default="SUN", help="weekday shown in the preview")
    ap.add_argument("--day", default="4", help="day of month shown in the preview")
    ap.add_argument("--no-emit", action="store_true", help="skip writing C sources")
    args = ap.parse_args()

    if not os.path.isfile(args.ttf):
        sys.exit("font not found: %s" % args.ttf)
    upem, cap_units = cap_height_units(args.ttf)
    tab = load_font_bytes(args.ttf, tabular=True)
    prop = load_font_bytes(args.ttf, tabular=False)

    fonts = [
        RasterFont("MAIN", tab, upem, cap_units, CAP_MAIN, DIGITS),
        RasterFont("SEC", tab, upem, cap_units, CAP_SEC, DIGITS),
        RasterFont("LABEL", prop, upem, cap_units, CAP_LABEL, ASCII),
        RasterFont("TEXT", prop, upem, cap_units, CAP_TEXT, ASCII),
    ]
    ref_x0, _, ref_x1, _ = ink_box(fonts[2], DATE_WIDTH_REF)
    cap_date = pick_date_cap(tab, upem, cap_units, ref_x1 - ref_x0)
    fonts.append(RasterFont("DATE", tab, upem, cap_units, cap_date, "0123456789"))
    layout = compute_layout(fonts[0], fonts[1], fonts[2], fonts[4])
    for key, val in layout.items():
        print("%-12s %d" % (key, val))
    margins = edge_margins(fonts, layout)
    print("edge margins at max pixel shift: %s" % margins)
    if min(margins.values()) < MIN_EDGE_MARGIN:
        sys.exit("layout too close to the screen edge (< %d px)" % MIN_EDGE_MARGIN)
    if not args.no_emit:
        size = emit(fonts, layout, os.path.basename(args.ttf))
        print("bitmap bytes: %d" % size)
    if args.preview:
        preview(args.preview, fonts, layout, alt=args.alt, wd=args.weekday, day=args.day)
        print("preview: %s" % args.preview)


if __name__ == "__main__":
    main()
