# ESP32 SimpleClock

[English](README.md)

**Waveshare ESP32-C6-Touch-AMOLED-2.16**（480 × 480 正方形 AMOLED）桌上型電子時鐘。上半部大字小時、下半部分鐘，兩列數字同欄對齊，秒數以小字放在角落。透過 Wi-Fi 自動網路對時（NTP），斷網時靠板載 RTC 繼續走時，並有多項措施減少 AMOLED 烙印。

| 偶數小時 | 奇數小時（側欄換到左邊） | Wi-Fi 設定 | 狀態頁（輕觸螢幕） |
|---|---|---|---|
| ![時鐘](docs/clock.png) | ![時鐘，側欄在左](docs/clock-left.png) | ![Wi-Fi 設定](docs/setup.png) | ![狀態頁](docs/status.png) |

以上截圖用 `tools/screenshot.py` 從實機讀回；狀態頁上的 Wi-Fi 名稱已遮蔽。

## 功能

- 時、分使用等寬數字，上下兩列逐欄對齊；秒數以小字放在分鐘旁的下角。
- 側欄上方顯示星期與日期。日期固定兩位數、固定字級，兩個數字合起來的寬度與「SUN」相同；12 小時制時加上 AM／PM。
- 每小時透過 NTP 對時，最多 3 個伺服器、可在網頁自行填寫（預設 `tock.stdtime.gov.tw`、`time.stdtime.gov.tw`、`pool.ntp.org`）。每次對時也會寫入 PCF85063 RTC，重新開機時先採用 RTC 的時間，所以沒有 Wi-Fi 也能正常走時。
- 亮度 8 段，白天、夜間各自記憶；夜間時段自動調暗（預設 23:00–07:00）。
- 定時關閉螢幕（預設 03:00–09:00）：時段內螢幕關閉，按任一鍵或輕觸會亮 30 秒。
- 重力感應自動旋轉，或固定為某一方向。
- 防烙印：整個畫面每分鐘移動 1 像素，走 12 × 12 的封閉迴圈（位移 −6～+5 px）；側欄每小時左右換邊，大數字隨之移動 92 px；使用暖白色而非純白；夜間調暗；定時關閉。
- 用手機設定 Wi-Fi：時鐘會開啟自己的熱點並在螢幕顯示 QR code，連上後設定頁自動跳出（captive portal）。
- 設定網頁 `http://clock.local`，可切換繁體中文／English（預設跟隨瀏覽器語言，右上角切換）。
- 提示：距上次對時超過 24 小時（或開機 5 分鐘內都沒有對時成功）顯示橘色 `SYNC`；只用電池且電量 15% 以下顯示紅色 `BATT`。

## 使用硬體

| 項目 | 說明 |
|---|---|
| 開發板 | [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16)（有無鋰電池版本皆可） |
| 主控 | ESP32-C6，RISC-V 160 MHz、512 KB SRAM、16 MB flash、無 PSRAM；Wi-Fi 6（僅 2.4 GHz） |
| 螢幕 | 2.16 吋 AMOLED，480 × 480，CO5300 控制器（QSPI） |
| 觸控 | CST9220（I²C） |
| 電源 | AXP2101：螢幕供電、PWR 鍵、電池充電 |
| RTC | PCF85063 |
| 姿態感測 | QMI8658（判斷方向） |
| 按鍵 | BOOT（GPIO 9）、KEY／IO10（GPIO 10）、PWR（經 AXP2101） |
| 線材 | 可傳資料的 USB-C 線 |

不需要焊接或其他零件。

## 按鍵

| 按鍵 | 短按 | 長按 |
|---|---|---|
| KEY（IO10） | 亮度 ＋ | 1.5 秒：切換方向（自動 → USB 朝下 → 左 → 上 → 右 → 自動） |
| BOOT | 亮度 － | 3 秒：進入／離開 Wi-Fi 設定 |
| PWR | 螢幕開／關 | 6 秒：整機關機（AXP2101 硬體行為；依 Waveshare 說明，短按 PWR 即可開機） |
| 輕觸螢幕 | 顯示狀態頁 10 秒（再點一次關閉） | — |

螢幕關閉時，第一次按鍵或輕觸只會喚醒螢幕。在定時關閉時段內，最後一次操作 30 秒後會再自動關閉。

## 編譯與燒錄

需要 Python 3（以 3.14 測試）與可傳資料的 USB-C 線。PlatformIO 會把工具鏈下載到 `~/.platformio`（測試機上約 7 GB）。第一次編譯還要編 Arduino 核心，需要幾分鐘；之後約 10 秒。

macOS／Linux：

```bash
git clone https://github.com/marvin3991/ESP32-SimpleClock.git
cd ESP32-SimpleClock
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/pio run -t upload
```

Windows（PowerShell，未實測）：

```powershell
py -m venv .venv
.venv\Scripts\pip install -r requirements.txt
.venv\Scripts\pio run -t upload
```

- 連接埠會自動偵測；要指定時加上 `--upload-port /dev/cu.usbmodemXXXX`（Windows 為 `COM5` 之類）。
- 從 GitHub 下載很慢時，設定 `IDF_GITHUB_ASSETS=dl.espressif.com/github_assets`，編譯器會改從 Espressif 鏡像站下載。
- 選擇性：第一次燒錄前先備份原廠韌體（16 MB，約 1 分鐘）。`backup/` 已列入 `.gitignore`。

```bash
mkdir -p backup
~/.platformio/penv/bin/esptool --chip esp32c6 read-flash 0 0x1000000 backup/original-flash.bin
```

要還原時，把同一行指令的 `read-flash 0 0x1000000 backup/original-flash.bin` 換成 `write-flash 0 backup/original-flash.bin`。

## Wi-Fi 設定

以下兩種擇一，以最後一次修改的為準。

1. **`.env` 檔**：把 `.env.example` 複製成 `.env`，填入 `WIFI_SSID` 和 `WIFI_PASSWORD` 後重新燒錄。值含空白或 `#` 時用雙引號包起來。`.env` 不會進版控，`scripts/pio_env.py` 只會把它轉成 build 目錄裡的標頭檔。
2. **用手機**：沒有 Wi-Fi 設定且沒有有效時間時，時鐘會自動開啟設定熱點；其他時候長按 BOOT 3 秒。用手機相機掃螢幕上的 QR code 加入 `Clock-XXXX`（密碼每次隨機並顯示在螢幕上），設定頁會自動跳出；沒跳出就開 `http://192.168.4.1`。

連上家中網路後，設定頁在 `http://clock.local`（或狀態頁上顯示的 IP）。

## 設定網頁

可設定 Wi-Fi、時區、12／24 小時制、是否顯示日期、3 個對時伺服器、白天與夜間亮度、夜間時段、定時關閉時段、側欄每小時換邊、螢幕方向。網頁不會把已儲存的 Wi-Fi 密碼傳回瀏覽器；同一個區網內的任何人都能開啟這個頁面。

## 序列埠指令與截圖

```bash
.venv/bin/python tools/console.py status
.venv/bin/python tools/screenshot.py shot.png
```

指令（輸入 `help` 查看）：`status`、`shot`、`btn boot|key|pwr|touch [short|long]`、`level <1-8>`、`rot auto|0|1|2|3`、`page clock|status|setup`、`settime <unix-epoch>`、`ntp`、`rtc`、`imu`、`reboot`、`factory yes`。在 macOS／Linux 上工具會自動找連接埠；要指定時加上 `--port`。

## 更換字型

數字與文字是由 TrueType 字型預先轉成點陣。要換字型：

```bash
.venv/bin/python tools/gen_fonts.py --ttf path/to/font.ttf --preview preview.png
.venv/bin/pio run -t upload
```

版面（欄寬、日期字級、位置）會依新字型重新計算；如果最大飄移時有字跡離螢幕邊緣不到 12 px，產生器會報錯停止。公開前請確認字型授權。

## 規格

| 項目 | 數值 | 說明 |
|---|---|---|
| 字型 | M PLUS Rounded 1c Black | 時分字高 176 px、秒 44 px、星期 25 px、日期 39 px、介面文字 22 px；只含 ASCII |
| 版面 | 數字欄寬 159 px；基線 y = 225／431；側欄寬 80 px | 側欄在右時數字 x = 35，在左時 x = 127 |
| 日期 | 兩位等寬數字，每位 35 px，共 70 px | 等於「SUN」的墨跡寬度 70 px |
| 邊距 | 左 ≥ 29、右 30、上 40、下 41 px | 最大飄移、左右兩種排列、所有數字與標籤 |
| 繪圖 | 每次 480 × 32 px 一帶，只重畫變動區域 | ESP32-C6 沒有 PSRAM，放不下 450 KB 的整張畫面 |
| 對時 | SNTP 每 3600 秒，最多 3 個伺服器 | 主機名稱或 IPv4（英數字、`.`、`-`，最長 63 字元）；三個都留空恢復預設 |
| RTC | PCF85063 存 UTC，RAM 位元組寫入標記 0xC7 | 沒有標記、振盪器停止旗標為 1、或時間早於韌體編譯日前一天時不採用 |
| 亮度 | 8 段：3、8、16、30、55、95、160、255（暫存器 0x51） | 最後一次調整 5 秒後才寫入 flash |
| 夜間調暗 | 預設 23:00–07:00 | 開始＝結束時停用；可跨午夜 |
| 定時關閉 | 預設啟用，03:00–09:00 | 只在時段開始／結束時切換；按鍵後亮 30 秒；Wi-Fi 設定中不關閉 |
| 方向 | QMI8658，傾斜 > 0.5 g 且持續 0.7 秒 | 平放時維持上一個方向 |
| Wi-Fi 重連 | 10 秒起，每次加倍，上限 5 分鐘 | 單次連線 30 秒沒有回應視為失敗 |
| Wi-Fi 省電 | 接 USB 時關閉，只用電池時開啟 | 讓 `clock.local` 即時回應 |
| 設定熱點 | `Clock-` + MAC 後 2 bytes，WPA2，8 位數隨機密碼，192.168.4.1 | 連線成功 8 秒後關閉；已有 Wi-Fi 設定時閒置 10 分鐘關閉 |
| 記憶體 | RAM 25.2%（82,528／327,680 B），flash 25.5%（1,672,012／6,553,600 B） | 2026-10-05 編譯結果 |

## 出處

- 字型：**M PLUS Rounded 1c**，設計 Coji Morishita 與 M+ Fonts Project，© 2016 The Rounded M+ Project Authors，[SIL Open Font License 1.1](fonts/OFL.txt)（取自 [Google Fonts](https://github.com/google/fonts/tree/main/ofl/mplusrounded1c)）。
- QR code：Project Nayuki 的 [QR Code generator library](https://github.com/nayuki/QR-Code-generator)（MIT）。
- 編譯時使用的函式庫：Arduino-ESP32、ESP-IDF、pioarduino、GFX Library for Arduino、SensorLib、XPowersLib。
- 開發板腳位與螢幕初始化數值：Waveshare 官方文件、電路圖與範例。

完整清單與授權：[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 授權

程式碼採 [MIT 授權](LICENSE)。`fonts/` 內的字型，以及 `src/generated/` 內由它產生的點陣字，採 SIL Open Font License 1.1。
