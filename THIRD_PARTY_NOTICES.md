# Third-party notices / 第三方授權與出處

The project code is under the MIT License (see `LICENSE`). The components
below keep their own licenses.

本專案程式碼採 MIT 授權（見 `LICENSE`）；以下元件維持各自的授權。

## Included in this repository / 包含在本 repo

| Component | Files | Author / copyright | License |
|---|---|---|---|
| **M PLUS Rounded 1c**, Black weight (font name "Rounded Mplus 1c", version 1.059) | `fonts/MPLUSRounded1c-Black.ttf` (unmodified, from [google/fonts](https://github.com/google/fonts/tree/main/ofl/mplusrounded1c); upstream [coz-m/MPLUS_FONTS](https://github.com/coz-m/MPLUS_FONTS)) | Copyright 2016 The Rounded M+ Project Authors. Design: Coji Morishita, M+ Fonts Project | SIL Open Font License 1.1, full text in `fonts/OFL.txt` |
| Glyph bitmaps rendered from the font above | `src/generated/font_data.cpp`, `src/generated/font_data.h` (made by `tools/gen_fonts.py`) | as above | SIL Open Font License 1.1 (derived from the font), notice in the file headers |
| **QR Code generator library** (C), v1.8.0 | `src/qrcodegen.c`, `src/qrcodegen.h` (unmodified) | Copyright © Project Nayuki, [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator) | MIT, notice kept in the file headers |

## Downloaded when building (not included) / 編譯時下載（不在 repo 內）

| Component | Version | License |
|---|---|---|
| [pioarduino platform-espressif32](https://github.com/pioarduino/platform-espressif32) | 55.03.38-1 | Apache-2.0 |
| [Arduino-ESP32](https://github.com/espressif/arduino-esp32) | 3.3.8 | LGPL-2.1 |
| [ESP-IDF](https://github.com/espressif/esp-idf) (inside Arduino-ESP32) | 5.5.4 | Apache-2.0 |
| [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) | 1.6.6 | BSD (Adafruit GFX license) |
| [SensorLib](https://github.com/lewisxhe/SensorLib) | 0.2.6 | MIT |
| [XPowersLib](https://github.com/lewisxhe/XPowersLib) | 0.2.9 | MIT |
| [PlatformIO Core](https://github.com/platformio/platformio-core) (build tool) | 6.2.0 | Apache-2.0 |
| [fontTools](https://github.com/fonttools/fonttools) (font generator) | 4.66.1 | MIT |
| [Pillow](https://github.com/python-pillow/Pillow) (font generator, screenshots) | 12.3.0 | MIT-CMU |
| [pySerial](https://github.com/pyserial/pyserial) (serial tools) | 3.5 | BSD |

## References (no code copied) / 參考資料（未複製程式碼）

- Waveshare ESP32-C6-Touch-AMOLED-2.16 [documentation](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16), schematic and [example code](https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16): pin assignments and the panel's vendor initialisation values.
- Datasheets: CO5300 (display controller), PCF85063A (RTC), AXP2101 (PMU), QMI8658 (IMU).
- Default time servers `tock.stdtime.gov.tw` and `time.stdtime.gov.tw` belong to Taiwan's National Time and Frequency Standards Laboratory (commissioned by BSMI, run by Chunghwa Telecom Laboratories); `pool.ntp.org` is the [NTP Pool Project](https://www.ntppool.org/).
