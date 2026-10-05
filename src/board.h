#pragma once
// Waveshare ESP32-C6-Touch-AMOLED-2.16 — pin map and I2C addresses.
//
// Sources:
//  - Schematic: files.waveshare.com/wiki/ESP32-C6-Touch-AMOLED-2.16/
//    ESP32-C6-Touch-AMOLED-2.16-Schematic.pdf (nets LCD_CS=GPIO15,
//    TP_INT=GPIO5, TP_RESET=GPIO11, Key1=GPIO9, Key3=GPIO10)
//  - Waveshare ESP-IDF example 09_LVGL_V9_Test (QSPI pins, AXP2101 @0x34)
//  - all pins verified on a real board with this firmware

#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// QSPI AMOLED (CO5300 / SH8601-compatible command set)
#define LCD_CS      15
#define LCD_SCLK    0
#define LCD_SDIO0   1
#define LCD_SDIO1   2
#define LCD_SDIO2   3
#define LCD_SDIO3   4
// LCD reset is not wired to a GPIO: AXP2101 ALDO3 power-cycles the panel.

// Shared I2C bus: PMU, RTC, IMU, touch, audio codecs
#define I2C_SDA     8
#define I2C_SCL     7

#define AXP2101_ADDR   0x34
#define PCF85063_ADDR  0x51
#define QMI8658_ADDR   0x6B
#define CST9220_ADDR   0x5A

// Touch controller CST9220 (the chip reports its model at boot; CST92xx driver)
#define TP_INT      5
#define TP_RST      11

// Side buttons (active low, 10k pull-ups on board)
#define BTN_BOOT_GPIO  9    // "BOOT" — also the download-mode strap at reset
#define BTN_KEY_GPIO   10   // "KEY" (silkscreen IO10)
// "PWR" goes to AXP2101 PWRON; read through the PMU's PKEY IRQ flags.
