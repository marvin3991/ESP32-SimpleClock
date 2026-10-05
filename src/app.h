#pragma once
#include <stdint.h>

#include "face.h"
#include "input.h"

// Application state machine: pages, brightness, night mode, screen power,
// rotation and pixel shift. The console calls into it for testing.
void app_init();
void app_tick();
void app_handle(const InputEvent& ev);
void app_screenshot();
void app_set_level(uint8_t level);          // brightness index for the current period
void app_set_rotation_mode(uint8_t mode);   // 0..3 fixed, ROTATION_AUTO
void app_show_page(Page p);
void app_print_status();
