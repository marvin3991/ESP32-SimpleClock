# ESP32 SimpleClock

[繁體中文](README.zh-TW.md)

A desk clock for the **Waveshare ESP32-C6-Touch-AMOLED-2.16** (480 × 480 square AMOLED). Big hours on top, minutes below in the same columns, small seconds in the corner. It sets itself over Wi-Fi (NTP), keeps time offline with the on-board RTC, and takes care of the AMOLED panel against burn-in.

| Even hours | Odd hours (side column on the left) | Wi-Fi setup | Status page (tap the screen) |
|---|---|---|---|
| ![Clock](docs/clock.png) | ![Clock, side column left](docs/clock-left.png) | ![Wi-Fi setup](docs/setup.png) | ![Status](docs/status.png) |

The screenshots are read back from a real board with `tools/screenshot.py`; the Wi-Fi name on the status page is masked.

## Features

- Hours and minutes in tabular digits, aligned column by column; seconds in small type at the minutes' lower corner.
- Weekday and day of month at the top of the side column. The day always has two digits and one fixed size, chosen so the widest day is as wide as "SUN"; both sit with their ink on the column's outer edge. AM/PM in 12-hour mode.
- Time sync over NTP every hour, with up to three servers you can edit (defaults `tock.stdtime.gov.tw`, `time.stdtime.gov.tw`, `pool.ntp.org`). Each sync is also written to the PCF85063 RTC, and the clock starts from the RTC time after a restart, so it keeps working without Wi-Fi.
- Eight brightness levels, remembered separately for day and night; automatic night dimming (default 23:00–07:00).
- Screen-off schedule (default 03:00–09:00): the panel is off in that window; a button press or a tap shows the clock for 30 s.
- Auto-rotation from the accelerometer, or a fixed orientation.
- Burn-in protection: the whole face drifts 1 px every minute around a 12 × 12 loop (offsets −6…+5 px); every hour the side column swaps sides and the digits move 92 px; warm white instead of pure white; night dimming; screen-off schedule. Without a valid time ("--") the face still drifts every minute and swaps sides every hour of uptime; the Wi-Fi setup page drifts too and drops to the night brightness after 10 minutes without a press or tap.
- Wi-Fi setup from a phone: the clock opens its own hotspot (random 12-character password) and shows a QR code to join it; the setup page opens by itself (captive portal).
- Settings page at `http://clock.local` in Traditional Chinese or English (follows the browser language; switch at the top right). Changes are accepted only from the page itself, not from other web sites.
- Indicators: orange `SYNC` when the last NTP sync is more than 24 h old (or none succeeded in the first 5 minutes after start-up), red `BATT` when the battery is at 15 % or less without USB power.

## Hardware

| Part | Details |
|---|---|
| Board | [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16) (with or without the lithium battery) |
| MCU | ESP32-C6, RISC-V 160 MHz, 512 KB SRAM, 16 MB flash, no PSRAM; Wi-Fi 6 (2.4 GHz only) |
| Display | 2.16″ AMOLED, 480 × 480, CO5300 controller over QSPI |
| Touch | CST9220 (I²C) |
| Power | AXP2101 PMU: panel power rails, PWR key, battery charging |
| RTC | PCF85063 |
| Motion sensor | QMI8658 (orientation) |
| Buttons | BOOT (GPIO 9), KEY / IO10 (GPIO 10), PWR (through the AXP2101) |
| Cable | USB-C data cable |

No soldering or extra parts are needed.

## Buttons

| Button | Press | Hold |
|---|---|---|
| KEY (IO10) | Brightness + | 1.5 s: change orientation (auto → USB down → left → up → right → auto) |
| BOOT | Brightness − | 3 s: open / close Wi-Fi setup |
| PWR | Screen on / off | 6 s: power the board off (AXP2101 hardware; a short press turns it back on, per Waveshare) |
| Tap the screen | Show the status page for 10 s (tap again to close) | — |

While the screen is off, the first press or tap only wakes it. Inside the screen-off schedule it turns off again 30 s after the last press.

## Build and flash

You need Python 3 (tested with 3.14) and a USB-C data cable. On the first build PlatformIO downloads the toolchain into `~/.platformio` (about 7 GB on the test machine), so that run depends on your connection. After that, on the test machine (Apple M4), a full build from a fresh clone took 34–39 s and a rebuild after a small change about 10 s.

macOS / Linux:

```bash
git clone https://github.com/marvin3991/ESP32-SimpleClock.git
cd ESP32-SimpleClock
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/pio run -t upload
```

Windows (PowerShell, not tested):

```powershell
py -m venv .venv
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\pio run -t upload
```

- The serial port is detected automatically; add `--upload-port /dev/cu.usbmodemXXXX` (or `COM5`) to pick one.
- If downloads from GitHub are slow, set `IDF_GITHUB_ASSETS=dl.espressif.com/github_assets` so the compiler is fetched from Espressif's mirror.
- Optional, before the first flash: back up the factory firmware (16 MB, about a minute). `backup/` is ignored by git.

```bash
mkdir -p backup
~/.platformio/penv/bin/esptool --chip esp32c6 read-flash 0 0x1000000 backup/original-flash.bin
```

To restore it later, run the same command with `write-flash 0 backup/original-flash.bin` in place of `read-flash 0 0x1000000 backup/original-flash.bin`.

## Wi-Fi

Either of these works; whichever was changed last wins.

1. **`.env` file**: copy `.env.example` to `.env`, fill in `WIFI_SSID` and `WIFI_PASSWORD`, and flash again. Quote values that contain spaces or `#`; outside quotes, a `#` after a space starts a comment. The file is never committed; `scripts/pio_env.py` turns it into a header inside the build directory only.
2. **From a phone**: when there is no Wi-Fi and no valid time, the clock starts its setup hotspot by itself; otherwise hold BOOT for 3 s. Scan the QR code on the screen to join `Clock-XXXX` (the password is random, 12 characters, and shown on the screen). The setup page opens by itself; if it does not, open `http://192.168.4.1`. If the connection fails, the hotspot stays open so you can correct the settings.

After it has joined your network, the settings page is at `http://clock.local` (or the IP shown on the status page).

## Settings page

Wi-Fi, time zone, 12/24-hour clock, date on/off, three time servers, day and night brightness, night hours, screen-off hours, hourly side swap and orientation. The page never sends the saved Wi-Fi password back to the browser. Anyone on the same network can open it.

Changes are accepted only from the page itself: it sends an `X-Clock` header that pages from other web sites cannot add, and its API answers only requests addressed to an IP address, a name without dots or a local-only name such as `clock.local` (this also stops DNS rebinding). Open the page at `http://clock.local` or at the clock's IP address.

## Serial console and screenshots

```bash
.venv/bin/python tools/console.py status
.venv/bin/python tools/screenshot.py shot.png
```

Commands (type `help`): `status`, `shot`, `btn boot|key|pwr|touch [short|long]`, `level <1-8>`, `rot auto|0|1|2|3`, `page clock|status|setup`, `settime <unix-epoch>`, `ntp`, `rtc` (RTC time, and where its tick falls against the system clock's second), `imu`, `reboot`, `factory yes`. The tools find the port by themselves on macOS and Linux; add `--port` to choose one. A screenshot ends with a CRC-32 of its pixels and is retried if the stream was damaged.

## Using another font

The digits and text are pre-rendered from a TrueType font. To use another one:

```bash
.venv/bin/python tools/gen_fonts.py --ttf path/to/font.ttf --preview preview.png
.venv/bin/pio run -t upload
```

The layout (column widths, date size, positions) is recalculated for the new font. The generator stops with an error if any glyph would come closer than 12 px to the screen edge at the largest drift, or if digits, the two rows or the side-column text would overlap. Check the font's license before publishing it.

## Specifications

| Item | Value | Notes |
|---|---|---|
| Font | M PLUS Rounded 1c Black | Hours/minutes 176 px cap height, seconds 44 px, weekday 25 px, date 39 px, UI text 22 px; ASCII only |
| Layout | Digit columns 159 px; baselines y = 225 / 431; side column 80 px | Digits at x = 35 (column right) or x = 127 (column left) |
| Date | Two tabular digits in 35 px cells; the widest day is 68 px of ink | "SUN" is 67 px of ink; the day is placed by its ink on the column edge, like the weekday |
| Edge margin | ≥ 29 px left, 30 right, 40 top, 41 bottom | At the largest drift, both sides, all digits and labels |
| Rendering | 480 × 32 px bands, only changed areas redrawn | The ESP32-C6 has no PSRAM for a full 450 KB frame |
| Time sync | SNTP every 3600 s, up to 3 servers | Host names or IPv4 addresses (letters, digits, `.`, `-`, up to 63 characters); all empty restores the defaults |
| RTC | PCF85063 kept in UTC, marker byte 0xC7 in its RAM register | Ignored when the marker is missing, the oscillator-stop flag is set, or the time is more than a day before the firmware build date |
| RTC setting | Written with STOP held; STOP released at x.500 s | The RTC on this board ticks 0.500 s after the release (datasheet: 0.507813–0.507935 s); its tick then falls +1…+3 ms after the system clock's second (`rtc` command) |
| Brightness | 8 levels: 3, 8, 16, 30, 55, 95, 160, 255 (register 0x51) | Saved to flash 5 s after the last change |
| Night dimming | Default 23:00–07:00 | Same start and end disables it; may cross midnight |
| Screen-off schedule | Default on, 03:00–09:00 | Switches only at the window edges; 30 s on after a press; never during Wi-Fi setup |
| Orientation | QMI8658, > 0.5 g held for 0.7 s | Lying flat keeps the last orientation |
| Wi-Fi retry | 10 s, doubling up to 5 min | An attempt with no answer for 30 s counts as failed |
| Wi-Fi power save | Off on USB power, on with battery only | Keeps `clock.local` responsive |
| Setup hotspot | `Clock-` + last 2 MAC bytes, WPA2, random 12-character password (31 symbols, no 0/o/1/l/i), 192.168.4.1 | Closes 8 s after a successful connection, or after 10 min without web requests if Wi-Fi is configured and nothing was tried in this session |
| Memory | RAM 25.2 % (82,592 / 327,680 B), flash 24.4 % (1,601,768 / 6,553,600 B) | Fresh clone without `.env`, 2026-10-05 |

## Credits

- Font: **M PLUS Rounded 1c** by Coji Morishita and the M+ Fonts Project, © 2016 The Rounded M+ Project Authors, [SIL Open Font License 1.1](fonts/OFL.txt) (from [Google Fonts](https://github.com/google/fonts/tree/main/ofl/mplusrounded1c)).
- QR codes: [QR Code generator library](https://github.com/nayuki/QR-Code-generator) by Project Nayuki (MIT).
- Libraries used at build time: Arduino-ESP32, ESP-IDF, pioarduino, GFX Library for Arduino, SensorLib, XPowersLib.
- Board pins and panel initialisation values: Waveshare's documentation, schematic and examples.

Details and licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

The code is under the [MIT License](LICENSE). The font in `fonts/` and the glyph bitmaps generated from it in `src/generated/` are under the SIL Open Font License 1.1.
