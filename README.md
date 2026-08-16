# DeepSeek Credits Remaining — ESP32-S3 AMOLED

A desk gadget that shows your remaining [DeepSeek](https://www.deepseek.com/) API credit balance (USD) on a Waveshare ESP32-S3 **1.8" AMOLED touch display** — plus a live peak/off-peak pricing countdown that ticks every second.

<!-- Want a photo? Drop one at docs/photo.jpg and uncomment:
<p align="center"><img src="docs/photo.jpg" alt="DeepSeek credits on the AMOLED" width="420"></p>
-->

## Features

- 💰 Remaining DeepSeek API credit balance in **USD**, big and centered (auto-shrinks to fit).
- ⏱️ **PEAK / OFF-PEAK** status with an **HH:MM:SS** countdown to the next pricing boundary (IST).
- 🔄 Balance auto-refreshes every 60 s — the fetch runs on a **background core**, so the countdown never freezes.
- 👆 **Double-tap** the balance to open a "Refresh?" popup — **Yes** (green) refreshes now, **No** (red) dismisses.
- ⚡ Flicker-free UI: static text is drawn once, only the countdown digits update each second.
- 🔒 No secrets are committed — see [Configuration](#configuration).

## Hardware

| Component | Detail |
|-----------|--------|
| Board | Waveshare **ESP32-S3-Touch-AMOLED-1.8**, **V1** revision (SH8601 display + FT3168 touch) |
| Display | 1.8" AMOLED, 368×448, QSPI |
| Touch | Capacitive (FT3168) via I²C (SDA=15, SCL=14, INT=21) |
| MCU | ESP32-S3R8 (8 MB PSRAM, 16 MB flash) |
| Wi-Fi | 2.4 GHz only |

> ⚠️ **V1 vs V2** — this project targets the **V1** board (SH8601 / FT3168). If your board is **V2** (CO5300 / CST820), the Waveshare `arduino-v2` demos are the right tree instead.

## How it works

1. Connects to Wi-Fi (2.4 GHz).
2. Calls `GET https://api.deepseek.com/user/balance` with your API key.
3. Reads the `USD` entry from the `balance_infos` array.
4. Displays `$<total_balance>` plus a peak/off-peak countdown.

The balance fetch runs in a [FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/freertos.html) task pinned to **core 0**; the UI (countdown + touch) runs on **core 1**, so network I/O never blocks the display.

### Peak / off-peak schedule

DeepSeek bills different rates for peak vs off-peak hours (effective 2026-08-16 16:00 UTC):

| Period | UTC | IST (UTC+5:30) |
|--------|-----|----------------|
| **Peak** | 01:00–04:00 & 06:00–10:00 | **06:30–09:30** & **11:30–15:30** |
| **Off-peak** | all other hours | all other hours |

The countdown shows **"for HH:MM:SS"** while off-peak (time left until peak begins) and **"until HH:MM:SS"** while peak (time left until peak ends).

## Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 2.x (or Arduino CLI)
- **esp32** board package by Espressif (≥ 3.0.6)
- Board definition **Waveshare ESP32-S3-Touch-AMOLED-1.8**

### Libraries

The Waveshare display/touch libraries are **not** in the Library Manager. Install them from the official board repo:

1. Download https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8
2. Copy the folders under `examples/arduino/libraries/` into your Arduino `libraries/` directory:
   - `GFX_Library_for_Arduino`
   - `Arduino_DriveBus_Library`
   - `Mylibrary` (provides `pin_config.h`)
3. Restart the Arduino IDE.

> No ArduinoJson needed — the balance JSON is parsed with a tiny dependency-free string parser.

## Configuration

Open `chitti-deepseek-credits-amoled/chitti-deepseek-credits-amoled.ino` and edit these three lines near the top:

```cpp
static const char *WIFI_SSID        = "YOUR_WIFI_SSID";
static const char *WIFI_PASS        = "YOUR_WIFI_PASSWORD";
static const char *DEEPSEEK_API_KEY = "sk-REPLACE_WITH_YOUR_KEY";   // your key
```

> 🔒 **Security** — the API key is embedded in firmware. Don't commit a build with your real key, and don't share a keyed `.ino`. The balance endpoint is read-only (it only reveals your balance), but a leaked key can still make paid API calls.

### Optional: touch orientation

If taps land in the wrong place on your board, flip one of these flags:

```cpp
static const bool TOUCH_SWAP_XY = false;   // swap X and Y axes
static const bool TOUCH_FLIP_X  = false;   // mirror X
static const bool TOUCH_FLIP_Y  = false;   // mirror Y
```

## Flashing

Arduino **Tools** settings:

| Setting | Value |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.8 |
| USB CDC On Boot | Enabled |
| Flash Size | 16MB |
| Partition Scheme | 16M Flash (3MB APP / 9.9MB FATFS) |
| PSRAM | Enabled |
| Port | your COM port |
| Serial Monitor | 115200 |

1. Plug the board in with a **data** USB-C cable.
2. Paste your Wi-Fi credentials and API key into the sketch.
3. Select board + port, click **Upload**.
4. Open Serial Monitor (115200) — you should see `HTTP 200` and `USD=…`.

> Upload stuck on "Connecting…"? Hold **BOOT**, start the upload, and release while it writes.

## Usage

- The balance auto-refreshes every 60 seconds.
- **Double-tap the balance number** to force a manual refresh.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `SSID not in scan` | Router is 5 GHz-only for that SSID, or SSID typo — use 2.4 GHz. |
| `HTTP 401` | Wrong API key — edit `DEEPSEEK_API_KEY`. |
| `HTTP -1` | No internet / DNS on this network. |
| `no USD balance` | Account has no USD balance entry (only CNY). |
| `FT3168 touch NOT FOUND` | Touch init failed — check the board revision (V1 vs V2). |

## Project structure

```
.
├── chitti-deepseek-credits-amoled/
│   └── chitti-deepseek-credits-amoled.ino   # the sketch
├── LICENSE
└── README.md
```

## License

[MIT](LICENSE)
