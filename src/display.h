#pragma once
#include <stdint.h>

// AMOLED panel (QSPI). Quadrant = which way the USB port points:
// 0 down, 1 left, 2 up, 3 right. Rotation is done by the panel (MADCTL),
// so the renderer always draws in the same 480x480 logical space.
bool display_init(uint8_t quadrant);
void display_set_quadrant(uint8_t quadrant);
void display_set_brightness(uint8_t dbv);   // 0..255
void display_set_on(bool on);
// x, y, w, h must be even (CO5300 CASET/RASET restriction).
void display_push(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* pixels);
