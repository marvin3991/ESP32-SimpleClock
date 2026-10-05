#pragma once
#include <stdint.h>

// QMI8658 accelerometer -> which way the USB port points (0 down, 1 left,
// 2 up, 3 right), with settle time so a brief tilt does not rotate.
bool orient_init();
void orient_tick();
bool orient_ok();
uint8_t orient_quadrant();
bool orient_read(float* ax, float* ay, float* az);
