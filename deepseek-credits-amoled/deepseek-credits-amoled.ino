/*
 * DeepSeek API Credits (USD) — Waveshare ESP32-S3-Touch-AMOLED-1.8 (V1)
 *
 * Screen (368x448 portrait):
 *   - Header: "DeepSeek" / "API credits remaining" — centered
 *   - Remaining credit balance in USD, big and centered (e.g. "$15.37")
 *   - Status block:
 *       "Currently"           (small, muted)
 *       PEAK / OFF-PEAK       (colored)
 *       "for" / "until"       (small)
 *       HH:MM:SS              (big countdown, ticks every second)
 *
 * Interaction:
 *   - Double-tap the credits area → popup "Refresh?" with Yes (green) / No (red)
 *   - Yes → fetch balance immediately; No → dismiss.
 *
 * Balance refresh: background task on core 0, every 60 s (and on "Yes").
 * The UI (core 1) never blocks, so the countdown keeps ticking during fetches.
 * Countdown ticks every second; static status text is drawn once (no flicker).
 *
 * Peak/off-peak (DeepSeek, effective 2026-08-16 16:00 UTC):
 *   Peak   (UTC): 01:00-04:00 and 06:00-10:00
 *   Peak   (IST): 06:30-09:30 and 11:30-15:30   (IST = UTC+5:30)
 *   Off-peak: all other hours
 *
 * Arduino Tools (same as bring-up):
 *   Board:              Waveshare ESP32-S3-Touch-AMOLED-1.8
 *   USB CDC On Boot:    Enabled
 *   Flash / Partition:  16M Flash (3MB APP / 9.9MB FATFS)
 *   PSRAM:              Enabled
 *   Port:               your COMx (e.g. COM5)
 *
 * Libraries (already installed from the Waveshare pack):
 *   GFX_Library_for_Arduino, Arduino_DriveBus_Library, Mylibrary (pin_config.h)
 *   — no ArduinoJson needed.
 *
 * ── SECURITY NOTE ──────────────────────────────────────────────────────────
 * Your DeepSeek API key is embedded in this firmware. Keep the .ino private and
 * never commit it to a public repo. A leaked key can make paid API calls.
 * ───────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include "pin_config.h"
#include "HWCDC.h"

// ===================== USER CONFIG =====================
static const char *WIFI_SSID        = "YOUR_WIFI_SSID";
static const char *WIFI_PASS        = "YOUR_WIFI_PASSWORD";
static const char *DEEPSEEK_API_KEY = "sk-REPLACE_WITH_YOUR_KEY"; // <-- paste your key

static const char *BALANCE_URL = "https://api.deepseek.com/user/balance";
static const uint32_t POLL_MS  = 60UL * 1000UL;   // balance refresh: 1 minute
static const uint8_t  BRIGHTNESS = 210;
// =======================================================

// Touch orientation fix-ups — leave all false; flip ONLY if taps land wrong.
static const bool TOUCH_SWAP_XY = false;   // swap X and Y axes
static const bool TOUCH_FLIP_X  = false;   // mirror X
static const bool TOUCH_FLIP_Y  = false;   // mirror Y

HWCDC USBSerial;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_SH8601 *gfx = new Arduino_SH8601(
  bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT);

// FT3168 touch (I2C: SDA=15, SCL=14, INT=21, address 0x38)
std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
  std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void Arduino_IIC_Touch_Interrupt(void);

std::unique_ptr<Arduino_IIC> FT3168(new Arduino_FT3x68(
  IIC_Bus, FT3168_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT,
  Arduino_IIC_Touch_Interrupt));

void Arduino_IIC_Touch_Interrupt(void) {
  FT3168->IIC_Interrupt_Flag = true;
}

// Colors (RGB565) — clean dark card on black AMOLED
static const uint16_t C_BG     = RGB565_BLACK;
static const uint16_t C_TEXT   = RGB565_WHITE;
static const uint16_t C_MUTED  = 0x9CF3;
static const uint16_t C_DIM    = 0x632C;
static const uint16_t C_ACCENT = 0x4D5F;   // DeepSeek-ish blue
static const uint16_t C_GOOD   = 0x07E0;   // green (off-peak / Yes)
static const uint16_t C_WARN   = 0x7DEF;   // amber (peak)
static const uint16_t C_BAD    = 0xF800;   // red (No)
static const uint16_t C_CARD   = 0x18E3;   // popup card fill

// Layout
static const int W = LCD_WIDTH;    // 368
static const int H = LCD_HEIGHT;   // 448

// Balance state (owned by the UI loop / core 1)
static bool  balanceLoaded = false;
static bool  balanceOk     = false;
static bool  isAvailable   = false;
static char  usdValue[32];          // e.g. "15.37"
static char  statusLine[48] = "boot";
static uint32_t lastPoll = 0;
static uint32_t lastTick = 0;

// Touch / popup state
static bool touchOk = false;
static bool popupOpen = false;
static bool wasTouching = false;
static uint32_t lastTapAt = 0;
static const uint32_t DOUBLE_TAP_MS = 500;

// Status display state (-2 none, -1 placeholder, 0 off-peak, 1 peak)
static int  statusState = -2;
static int  lastDrawnSec = -1;

// Background fetch (runs on core 0; UI on core 1 never blocks)
static TaskHandle_t fetchTask = NULL;
static volatile bool fetchRequested = false;
static volatile bool fetchBusy = false;
static volatile bool fetchReady = false;
static volatile bool fetchedOk = false;
static volatile bool fetchedAvailable = true;
static volatile char fetchedUsd[32];
static volatile char fetchedStatus[48];

// ---------- tiny JSON helpers (no ArduinoJson) ----------
// Find the "currency":"USD" object, then read its "total_balance" string.
static bool findUsdBalance(const char *js, char *out, size_t outLen) {
  const char *p = js;
  while ((p = strstr(p, "\"currency\"")) != NULL) {
    const char *v = strchr(p + 10, ':');
    if (!v) return false;
    v++;
    while (*v == ' ' || *v == '\t') v++;
    if (strncmp(v, "\"USD\"", 5) == 0) {
      const char *tb = strstr(v, "\"total_balance\"");
      if (!tb) return false;
      const char *tv = strchr(tb + 14, ':');
      if (!tv) return false;
      tv++;
      while (*tv == ' ' || *tv == '\t') tv++;
      if (*tv != '"') return false;
      tv++;
      size_t i = 0;
      while (*tv && *tv != '"' && i + 1 < outLen) out[i++] = *tv++;
      out[i] = 0;
      return true;
    }
    p = v;
  }
  return false;
}

static bool jsonFindBool(const char *js, const char *key, bool *out) {
  char pat[48];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(js, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
  if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
  return false;
}

// ---------- drawing helpers ----------
static void clearScreen() {
  gfx->fillRect(0, 0, gfx->width(), gfx->height(), C_BG);
}

// Center a string horizontally on the full panel width.
static void drawCentered(const char *s, int y, int textSize, uint16_t color) {
  int w = (int)strlen(s) * 6 * textSize;   // default GFX font = 6px/char
  gfx->setTextSize(textSize);
  gfx->setTextColor(color);
  gfx->setCursor((W - w) / 2, y);
  gfx->print(s);
}

// Same, but draws with a background color so it self-clears (no flicker).
static void drawCenteredBg(const char *s, int y, int textSize, uint16_t fg, uint16_t bg) {
  int w = (int)strlen(s) * 6 * textSize;
  gfx->setTextSize(textSize);
  gfx->setTextColor(fg, bg);
  gfx->setCursor((W - w) / 2, y);
  gfx->print(s);
}

// ---------- header (drawn once) ----------
static void drawHeader() {
  drawCentered("DeepSeek", 12, 3, C_ACCENT);
  drawCentered("API credits remaining", 46, 2, C_MUTED);
}

// ---------- credits (big number) ----------
static void drawCredits() {
  // clear the credits band
  gfx->fillRect(0, 84, W, 166, C_BG);

  char big[40];
  if (balanceLoaded && balanceOk && usdValue[0]) {
    snprintf(big, sizeof(big), "$%s", usdValue);
  } else if (balanceLoaded && !balanceOk) {
    snprintf(big, sizeof(big), "--");
  } else {
    snprintf(big, sizeof(big), "...");
  }

  // fit: default GFX font is 6px wide * textSize, 8px tall * textSize
  int ts = 11;
  int tw = (int)strlen(big) * 6 * ts;
  while (tw > (W - 16) && ts > 5) {
    ts--;
    tw = (int)strlen(big) * 6 * ts;
  }

  uint16_t col = C_TEXT;
  if (balanceLoaded && balanceOk && !isAvailable) col = C_WARN;
  if (balanceLoaded && !balanceOk) col = C_DIM;

  gfx->setTextSize(ts);
  gfx->setTextColor(col);
  int x = (W - tw) / 2;
  int y = 84 + (166 - 8 * ts) / 2;   // vertically centered in the band
  gfx->setCursor(x, y);
  gfx->print(big);

  // Show an error hint (only on failure — no permanent WiFi/status text)
  if (balanceLoaded && !balanceOk) {
    drawCentered(statusLine, 84 + 166 - 20, 1, C_DIM);
  }
}

// ---------- peak / off-peak ----------
// DeepSeek peak windows (IST): 06:30-09:30 and 11:30-15:30
static bool isPeak(int secOfDay) {
  const int P1_START =  6 * 3600 + 30 * 60;   // 06:30
  const int P1_END   =  9 * 3600 + 30 * 60;   // 09:30
  const int P2_START = 11 * 3600 + 30 * 60;   // 11:30
  const int P2_END   = 15 * 3600 + 30 * 60;   // 15:30
  return (secOfDay >= P1_START && secOfDay < P1_END) ||
         (secOfDay >= P2_START && secOfDay < P2_END);
}

// Seconds until the end of the current peak (if peak) or start of the next.
static int secondsToNextBoundary(int secOfDay) {
  const int P1_START =  6 * 3600 + 30 * 60;
  const int P1_END   =  9 * 3600 + 30 * 60;
  const int P2_START = 11 * 3600 + 30 * 60;
  const int P2_END   = 15 * 3600 + 30 * 60;
  if (secOfDay >= P1_START && secOfDay < P1_END) return P1_END - secOfDay;
  if (secOfDay >= P2_START && secOfDay < P2_END) return P2_END - secOfDay;
  if (secOfDay < P1_START)      return P1_START - secOfDay;
  if (secOfDay < P2_START)      return P2_START - secOfDay;
  return (86400 - secOfDay) + P1_START;
}

static void fmtHMS(int sec, char *out, size_t n) {
  int h = sec / 3600;
  int m = (sec % 3600) / 60;
  int s = sec % 60;
  snprintf(out, n, "%02d:%02d:%02d", h, m, s);
}

// Static header of the status block: "Currently" / PEAK|OFF-PEAK / "for"|"until"
static void drawStatusHeader(bool peak) {
  gfx->fillRect(0, 258, W, 106, C_BG);   // header band only
  drawCentered("Currently", 262, 2, C_MUTED);
  const char *label = peak ? "PEAK" : "OFF-PEAK";
  drawCentered(label, 296, 4, peak ? C_WARN : C_GOOD);
  drawCentered(peak ? "until" : "for", 344, 2, C_DIM);
}

// Dynamic part: the HH:MM:SS countdown (redrawn each second)
static void drawCountdown(int secRemaining) {
  // bg-colored text self-clears — no fillRect, so no flicker / stall
  char hms[16];
  fmtHMS(secRemaining, hms, sizeof(hms));
  drawCenteredBg(hms, 370, 7, C_TEXT, C_BG);
}

// No-time placeholder (NTP still syncing)
static void drawPlaceholder() {
  gfx->fillRect(0, 258, W, 172, C_BG);
  drawCentered("Currently", 262, 2, C_MUTED);
  drawCentered("--:--:--", 370, 7, C_DIM);
}

// ---------- popup (double-tap on credits) ----------
static void drawPopup() {
  const int px = 54, py = 120, pw = 260, ph = 230;
  gfx->fillRoundRect(px, py, pw, ph, 16, C_CARD);
  gfx->drawRoundRect(px, py, pw, ph, 16, C_ACCENT);
  gfx->drawRoundRect(px + 1, py + 1, pw - 2, ph - 2, 16, C_ACCENT);

  drawCentered("Refresh?", 140, 3, C_TEXT);

  // Yes (green)
  gfx->fillRoundRect(84, 190, 200, 52, 26, C_GOOD);
  drawCentered("Yes", 204, 3, C_TEXT);

  // No (red)
  gfx->fillRoundRect(84, 258, 200, 52, 26, C_BAD);
  drawCentered("No", 272, 3, C_TEXT);
}

static bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void onTouchDown(int x, int y) {
  USBSerial.printf("tap %d,%d\n", x, y);

  if (popupOpen) {
    if (inRect(x, y, 84, 190, 200, 52)) {          // Yes → refresh now
      popupOpen = false;
      drawCredits();                 // restore credits band
      statusState = -2;              // force full status redraw (popup covered it)
      lastDrawnSec = -1;
      updateStatus();
      lastPoll = millis();           // restart the 60s auto-poll timer
      fetchRequested = true;         // non-blocking refresh
      return;
    }
    if (inRect(x, y, 84, 258, 200, 52)) {          // No → just dismiss
      popupOpen = false;
      drawCredits();
      statusState = -2;
      lastDrawnSec = -1;
      updateStatus();
      lastPoll = millis();
      return;
    }
    return;                          // tapped elsewhere: ignore
  }

  // Not open: double-tap on the credits band opens the popup
  if (y >= 84 && y <= 250) {
    uint32_t now = millis();
    if (lastTapAt != 0 && now - lastTapAt <= DOUBLE_TAP_MS) {
      lastTapAt = 0;
      USBSerial.println("DOUBLE TAP");
      popupOpen = true;
      drawPopup();
    } else {
      lastTapAt = now;
    }
  }
}

static void handleTouch() {
  if (!touchOk) return;

  int32_t points = FT3168->IIC_Read_Device_Value(
    FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  bool touching = points > 0;

  if (touching && !wasTouching) {
    int32_t x = FT3168->IIC_Read_Device_Value(
      FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int32_t y = FT3168->IIC_Read_Device_Value(
      FT3168->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);

    // orientation fix-ups (flip only if taps land in the wrong place)
    if (TOUCH_SWAP_XY) { int32_t t = x; x = y; y = t; }
    if (TOUCH_FLIP_X)  x = LCD_WIDTH  - 1 - x;
    if (TOUCH_FLIP_Y)  y = LCD_HEIGHT - 1 - y;

    onTouchDown(x, y);
  }
  wasTouching = touching;

  // keep the touch interrupt flag clean
  if (FT3168->IIC_Interrupt_Flag == true) {
    FT3168->IIC_Interrupt_Flag = false;
  }
}

// ---------- WiFi ----------
static const char *wifiStatusName(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:      return "idle";
    case WL_NO_SSID_AVAIL:    return "SSID not found";
    case WL_SCAN_COMPLETED:   return "scan done";
    case WL_CONNECTED:        return "connected";
    case WL_CONNECT_FAILED:   return "bad password?";
    case WL_CONNECTION_LOST:  return "conn lost";
    case WL_DISCONNECTED:     return "disconnected";
    default:                  return "unknown";
  }
}

// Runs on the fetch task; writes only to the fetched* / fetchedStatus globals.
static bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  delay(200);

  int n = WiFi.scanNetworks(false, true);
  bool sawSsid = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) sawSsid = true;
  }
  WiFi.scanDelete();
  USBSerial.printf("scan: %d networks, saw '%s': %s\n", n, WIFI_SSID, sawSsid ? "yes" : "no");

  if (n > 0 && !sawSsid) {
    snprintf((char *)fetchedStatus, sizeof(fetchedStatus), "SSID not in scan");
    USBSerial.println("SSID not found — wrong name or 5 GHz-only AP");
    return false;
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 45000) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    wl_status_t st = WiFi.status();
    snprintf((char *)fetchedStatus, sizeof(fetchedStatus), "WiFi: %s", wifiStatusName(st));
    USBSerial.printf("WiFi FAILED: %s\n", wifiStatusName(st));
    return false;
  }

  USBSerial.print("IP ");
  USBSerial.println(WiFi.localIP());
  snprintf((char *)fetchedStatus, sizeof(fetchedStatus), "WiFi OK");
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");   // IST = UTC+5:30
  return true;
}

// ---------- fetch (background task) ----------
static const char *httpCodeName(int code) {
  switch (code) {
    case -1:  return "conn refused";
    case -2:  return "send hdr fail";
    case -3:  return "send body fail";
    case -4:  return "not connected";
    case -5:  return "conn lost";
    case -11: return "read timeout";
    default:  return "";
  }
}

static void doFetchOnce() {
  fetchedOk = false;
  fetchedAvailable = true;
  fetchedUsd[0] = 0;

  if (!ensureWifi()) {
    return;   // fetchedStatus already set by ensureWifi
  }

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(20000);
  http.setTimeout(25000);

  WiFiClientSecure sclient;
  sclient.setInsecure();   // api.deepseek.com has a valid cert; insecure keeps it simple
  sclient.setTimeout(20);

  if (!http.begin(sclient, BALANCE_URL)) {
    snprintf((char *)fetchedStatus, sizeof(fetchedStatus), "bad URL");
    return;
  }

  http.addHeader("Accept", "application/json");
  http.addHeader("Connection", "close");
  http.addHeader("Authorization", String("Bearer ") + DEEPSEEK_API_KEY);
  http.setUserAgent("DeepSeekCreditsAMOLED/1.2");

  int code = http.GET();
  String body = (code > 0) ? http.getString() : String("");
  http.end();
  sclient.stop();

  USBSerial.printf("HTTP %d len=%d\n", code, body.length());
  if (body.length() > 0) {
    USBSerial.println(body.substring(0, min(240, (int)body.length())));
  }

  if (code != 200) {
    const char *name = httpCodeName(code);
    if (name && name[0]) snprintf((char *)fetchedStatus, sizeof(fetchedStatus), "HTTP %d %s", code, name);
    else if (code == 401) snprintf((char *)fetchedStatus, sizeof(fetchedStatus), "HTTP 401 bad key");
    else snprintf((char *)fetchedStatus, sizeof(fetchedStatus), "HTTP %d", code);
    return;
  }

  // parse into locals, then publish
  const char *js = body.c_str();
  bool avail = true;
  jsonFindBool(js, "is_available", &avail);

  char usd[32];
  usd[0] = 0;
  if (!findUsdBalance(js, usd, sizeof(usd))) {
    snprintf((char *)fetchedStatus, sizeof(fetchedStatus), "no USD balance");
    return;
  }

  fetchedAvailable = avail;
  strncpy((char *)fetchedUsd, usd, sizeof(fetchedUsd) - 1);
  fetchedUsd[sizeof(fetchedUsd) - 1] = 0;
  fetchedOk = true;
  snprintf((char *)fetchedStatus, sizeof(fetchedStatus), "ok");
  USBSerial.printf("USD=%s available=%d\n", usd, avail ? 1 : 0);
}

static void fetchWorker(void *arg) {
  (void)arg;
  for (;;) {
    if (fetchRequested) {
      fetchRequested = false;
      fetchBusy = true;
      doFetchOnce();
      fetchBusy = false;
      fetchReady = true;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// ---------- 1 Hz UI tick: status block ----------
static void updateStatus() {
  struct tm t;
  if (!getLocalTime(&t, 0)) {
    if (!popupOpen && statusState != -1) {
      drawPlaceholder();
      statusState = -1;
      lastDrawnSec = -1;
    }
    return;
  }

  int secOfDay = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  bool peak = isPeak(secOfDay);
  int newState = peak ? 1 : 0;

  // header only when peak/off-peak flips (or first draw); skip while popup open
  if (!popupOpen && statusState != newState) {
    drawStatusHeader(peak);
    statusState = newState;
  }

  // countdown every second (below the popup, so it keeps ticking)
  if (t.tm_sec != lastDrawnSec) {
    lastDrawnSec = t.tm_sec;
    drawCountdown(secondsToNextBoundary(secOfDay));
  }
}

// ---------- setup / loop ----------
void setup() {
  USBSerial.begin(115200);
  USBSerial.setTxTimeoutMs(0);   // never let serial output block the UI loop
  delay(150);
  USBSerial.println();
  USBSerial.println("DeepSeek API credits (USD) — AMOLED 1.8 V1");

  if (!gfx->begin()) {
    USBSerial.println("ERROR: gfx->begin() failed");
  }
  gfx->setBrightness(BRIGHTNESS);
  gfx->setRotation(0);
  clearScreen();

  // FT3168 touch (I2C)
  Wire.begin(IIC_SDA, IIC_SCL);
  int touchTries = 0;
  while (!FT3168->begin() && touchTries < 5) {
    touchTries++;
    USBSerial.printf("FT3168 init retry %d\n", touchTries);
    delay(500);
  }
  if (touchTries < 5) {
    FT3168->IIC_Write_Device_State(
      FT3168->Arduino_IIC_Touch::Device::TOUCH_POWER_MODE,
      FT3168->Arduino_IIC_Touch::Device_Mode::TOUCH_POWER_MONITOR);
    touchOk = true;
    USBSerial.println("FT3168 touch OK");
  } else {
    USBSerial.println("FT3168 touch NOT FOUND — double-tap disabled");
  }

  // Arm NTP early (IST) so the countdown starts as soon as WiFi is up
  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  drawHeader();
  drawCredits();          // shows "..." until first fetch
  updateStatus();         // shows "--:--:--" until NTP syncs

  // Start the background fetch on core 0 (UI stays on core 1, never blocked)
  xTaskCreatePinnedToCore(fetchWorker, "fetch", 16384, NULL, 1, &fetchTask, 0);
  fetchRequested = true;  // kick off the first fetch

  lastPoll = millis();
  lastTick = millis();
}

void loop() {
  uint32_t now = millis();

  // 1 Hz: countdown tick (never blocked — fetch runs on the other core)
  if (now - lastTick >= 1000) {
    lastTick = now;
    updateStatus();
  }

  // 60 s auto-refresh: request a background fetch
  if (!popupOpen && !fetchBusy && now - lastPoll >= POLL_MS) {
    lastPoll = now;
    fetchRequested = true;
  }

  // apply the latest fetch result when ready
  if (fetchReady) {
    fetchReady = false;
    balanceLoaded = true;
    balanceOk = fetchedOk;
    isAvailable = fetchedAvailable;
    if (fetchedOk && fetchedUsd[0]) {
      strncpy(usdValue, (const char *)fetchedUsd, sizeof(usdValue) - 1);
      usdValue[sizeof(usdValue) - 1] = 0;
    } else {
      usdValue[0] = 0;
    }
    strncpy(statusLine, (const char *)fetchedStatus, sizeof(statusLine) - 1);
    statusLine[sizeof(statusLine) - 1] = 0;
    if (!popupOpen) drawCredits();
  }

  handleTouch();

  delay(20);
}
