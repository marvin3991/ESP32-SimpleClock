#include "display.h"

#include <Arduino_GFX_Library.h>

#include "board.h"
#include "log.h"

// MADCTL value per quadrant, checked by eye on this board in all four
// orientations (the controller accepts the row/column exchange bit).
static const uint8_t MADCTL_FOR_QUADRANT[4] = {0x30, 0x50, 0xF0, 0x90};

static Arduino_DataBus* s_bus = nullptr;
static Arduino_SH8601* s_gfx = nullptr;

bool display_init(uint8_t quadrant) {
    s_bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    s_gfx = new Arduino_SH8601(s_bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);
    if (!s_gfx->begin()) {
        LOGE("disp", "QSPI bus init failed");
        return false;
    }
    // Vendor registers from Waveshare's ESP-IDF example for this board: without
    // the page-0x20 writes the panel stays black even though SPI works.
    s_bus->beginWrite();
    s_bus->writeC8D8(0xFE, 0x20);   // manufacturer command page
    s_bus->writeC8D8(0x19, 0x10);
    s_bus->writeC8D8(0x1C, 0xA0);
    s_bus->writeC8D8(0xFE, 0x00);   // back to user command page
    s_bus->writeC8D8(0xC4, 0x80);   // SPI mode control
    s_bus->writeC8D8(0x36, MADCTL_FOR_QUADRANT[quadrant & 3]);
    s_bus->writeC8D8(0x53, 0x20);   // brightness control block on
    s_bus->writeC8D8(0x51, 0x00);   // start dark; the app fades in after frame 1
    s_bus->writeC8D8(0x63, 0xFF);   // HBM brightness ceiling
    s_bus->writeCommand(0x29);      // display on
    s_bus->endWrite();
    delay(20);
    LOGI("disp", "panel ready, quadrant %u", quadrant & 3);
    return true;
}

void display_set_quadrant(uint8_t quadrant) {
    if (!s_bus) return;
    s_bus->beginWrite();
    s_bus->writeC8D8(0x36, MADCTL_FOR_QUADRANT[quadrant & 3]);
    s_bus->endWrite();
}

void display_set_brightness(uint8_t dbv) {
    if (!s_bus) return;
    s_bus->beginWrite();
    s_bus->writeC8D8(0x51, dbv);
    s_bus->endWrite();
}

void display_set_on(bool on) {
    if (!s_bus) return;
    s_bus->sendCommand(on ? 0x29 : 0x28);
}

void display_push(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* pixels) {
    if (!s_gfx) return;
    s_gfx->draw16bitRGBBitmap(x, y, pixels, w, h);
}
