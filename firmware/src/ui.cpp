/**
 * Waveshare ESP32-S3-Touch-LCD-2.1 UI:
 * ST7701 RGB panel + CST820 touch + TCA9554 expander.
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <display/Arduino_RGB_Display.h>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <stdint.h>
#include "ui.h"
#include "ambient.h"
#include "buzzer.h"
#include "board.h"
#include "config.h"
#include "exio.h"
#include "st7701.h"

#ifndef BUDDY_DEVICE_NAME
#define BUDDY_DEVICE_NAME "CURSOR BUDDY"
#endif

// Round 480×480: keep chrome inside the inscribed circle (r≈240).
// Buttons sit on a chord well above the clipped bottom edge.
static const int BTN_Y = 348;
static const int BTN_H = 50;
static const int BTN_W = 138;
static const int BTN_GAP = 16;
static const int BTN_L_X = (PANEL_W - (BTN_W * 2 + BTN_GAP)) / 2;
static const int BTN_R_X = BTN_L_X + BTN_W + BTN_GAP;

// Pager sits just above the action buttons, inside the round bezel.
static const int CHEV_W = 56;
static const int CHEV_H = 44;
static const int CHEV_Y = 300;
static const int CHEV_L_X = 132;
static const int CHEV_R_X = PANEL_W - 132 - CHEV_W;

static Arduino_ESP32RGBPanel *rgb = nullptr;
static Arduino_RGB_Display *disp = nullptr;
static Arduino_GFX *gfx = nullptr;

static std::vector<BuddySession> g_sessions;
static bool g_muted = false;
static bool g_offline = true;
static bool g_can_act = false;
static BuddyState g_focus = BuddyState::Idle;
static String g_focused_id;
static String g_view_id;
static size_t g_focus_idx = 0;
static String g_pending_session;
static String g_pending_action;
static bool g_has_pending = false;
static uint32_t g_last_draw = 0;
static uint32_t g_last_input = 0;
static bool touchBegan = false;

static uint16_t colorFor(BuddyState st) {
  switch (st) {
    case BuddyState::Working: return 0x047F;
    case BuddyState::Attention: return 0xFD20;
    case BuddyState::Complete: return 0x07E0;
    case BuddyState::Error: return 0xF800;
    case BuddyState::Offline: return 0x8410;
    default: return 0xC618;
  }
}

static void fillScreen(uint16_t c) {
  if (gfx) gfx->fillScreen(c);
}

static void drawCentered(const String &text, int y, uint16_t color, uint8_t size) {
  if (!gfx) return;
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (PANEL_W - (int)w) / 2;
  gfx->setCursor(max(0, x), y);
  gfx->print(text);
}

static int alertRank(BuddyState st) {
  if (st == BuddyState::Attention) return 0;
  if (st == BuddyState::Error) return 1;
  if (st == BuddyState::Complete) return 2;
  return 100;
}

static bool isAlert(BuddyState st) {
  return alertRank(st) < 100;
}

static size_t indexOfView() {
  if (g_view_id.length()) {
    for (size_t i = 0; i < g_sessions.size(); i++) {
      if (g_sessions[i].session_id == g_view_id) return i;
    }
  }
  return 0;
}

static void chimeFor(BuddyState st) {
  if (st == BuddyState::Attention) buzzerAttention();
  else if (st == BuddyState::Complete) buzzerComplete();
  else if (st == BuddyState::Error) buzzerError();
}

static void drawChevron(int x, const char *label) {
  if (!gfx) return;
  gfx->fillRoundRect(x, CHEV_Y, CHEV_W, CHEV_H, 10, 0x2104);
  gfx->setTextSize(3);
  gfx->setTextColor(0xFFFF);
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(x + (CHEV_W - (int)tw) / 2, CHEV_Y + 10);
  gfx->print(label);
}

static const int CLK_DATE_Y = 118;
static const int CLK_TIME_Y = 168;
static const int CLK_TIME_SIZE = 8;
static const int CLK_TEMP_Y = 256;
static const int CLK_COND_Y = 320;
static const int CLK_RANGE_Y = 354;
static const int CLK_STATUS_Y = 392;
static const int CLK_ICON_W = 56;

static const char *kDow[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *kMonName[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                 "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static bool g_clock_face = false;
static int g_shown_min = -1;
static int g_shown_hour = -1;
static int g_shown_mday = -1;
static int g_shown_colon = -1;
static int g_shown_wstate = -1;
static int g_shown_temp = 0;
static int g_shown_code = -2;
static int g_shown_lo = 0;
static int g_shown_hi = 0;
static bool g_shown_range = false;
static char g_shown_unit = 0;
static char g_shown_sum[24] = "";
static int g_shown_status = -1;

static int clockX() { return (PANEL_W - 5 * 6 * CLK_TIME_SIZE) / 2; }

static void formatClock(const AmbientClock &c, char out[6], const char **ampm) {
#if defined(BUDDY_24H) && BUDDY_24H
  snprintf(out, 6, "%02d:%02d", c.hour, c.minute);
  *ampm = nullptr;
#else
  int h = c.hour % 12;
  if (h == 0) h = 12;
  snprintf(out, 6, "%2d:%02d", h, c.minute);
  *ampm = c.hour < 12 ? "AM" : "PM";
#endif
}

static void drawTimeDigits(const char *hhmm) {
  gfx->setTextSize(CLK_TIME_SIZE);
  gfx->setTextColor(0xFFFF, 0x0000);
  gfx->setCursor(clockX(), CLK_TIME_Y);
  gfx->print(hhmm);
}

static void drawColon(bool on) {
  gfx->setTextSize(CLK_TIME_SIZE);
  gfx->setTextColor(0xFFFF, 0x0000);
  gfx->setCursor(clockX() + 2 * 6 * CLK_TIME_SIZE, CLK_TIME_Y);
  gfx->print(on ? ":" : " ");
}

static void drawAmPm(const char *ampm) {
  gfx->setTextSize(3);
  gfx->setTextColor(0xC618, 0x0000);
  gfx->setCursor(clockX() + 5 * 6 * CLK_TIME_SIZE + 14, CLK_TIME_Y + 40);
  gfx->print(ampm);
}

static void drawDateLine(const AmbientClock &c) {
  char line[12];
  snprintf(line, sizeof(line), "%s %s %02d", kDow[c.wday], kMonName[c.mon], c.mday);
  const int size = 3;
  int x = (PANEL_W - 10 * 6 * size) / 2;
  gfx->setTextSize(size);
  gfx->setTextColor(0xC618, 0x0000);
  gfx->setCursor(x, CLK_DATE_Y);
  gfx->print(line);
}

static uint16_t weatherAccent(int code) {
  if (code == 0 || code == 1 || code == 2) return 0xFE60;
  if (code >= 95) return 0xFD20;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 0xFFFF;
  if (code >= 51) return 0x047F;
  return 0xC618;
}

static void drawSun(int cx, int cy, int r, uint16_t color) {
  gfx->fillCircle(cx, cy, r, color);
  static const int8_t kDir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
  for (int i = 0; i < 8; i++) {
    int dx = kDir[i][0];
    int dy = kDir[i][1];
    int inner = r + 3;
    int outer = r + (dx && dy ? 7 : 9);
    gfx->drawLine(cx + dx * inner, cy + dy * inner, cx + dx * outer, cy + dy * outer, color);
    if (dx == 0) gfx->drawLine(cx + 1, cy + dy * inner, cx + 1, cy + dy * outer, color);
    else if (dy == 0) gfx->drawLine(cx + dx * inner, cy + 1, cx + dx * outer, cy + 1, color);
  }
}

static void drawCloud(int x, int y, uint16_t color) {
  gfx->fillCircle(x + 12, y + 16, 10, color);
  gfx->fillCircle(x + 26, y + 10, 13, color);
  gfx->fillCircle(x + 40, y + 16, 9, color);
  gfx->fillRoundRect(x + 6, y + 16, 40, 14, 7, color);
}

static void drawFlake(int x, int y, uint16_t color) {
  gfx->drawLine(x - 4, y, x + 4, y, color);
  gfx->drawLine(x, y - 4, x, y + 4, color);
  gfx->drawLine(x - 3, y - 3, x + 3, y + 3, color);
  gfx->drawLine(x - 3, y + 3, x + 3, y - 3, color);
}

static void drawBolt(int x, int y, uint16_t color) {
  gfx->fillTriangle(x + 8, y, x, y + 14, x + 9, y + 12, color);
  gfx->fillTriangle(x + 4, y + 10, x + 14, y + 10, x + 5, y + 26, color);
}

static void drawWeatherIcon(int x, int y, int code) {
  const uint16_t cloud = 0xE71C;
  uint16_t accent = weatherAccent(code);
  if (code == 0) {
    drawSun(x + 28, y + 24, 10, accent);
  } else if (code == 1) {
    drawSun(x + 20, y + 18, 10, accent);
    gfx->fillCircle(x + 38, y + 30, 8, cloud);
    gfx->fillCircle(x + 48, y + 32, 6, cloud);
    gfx->fillRoundRect(x + 32, y + 32, 22, 8, 4, cloud);
  } else if (code == 2) {
    drawSun(x + 16, y + 14, 8, accent);
    drawCloud(x + 6, y + 14, cloud);
  } else if (code == 45 || code == 48) {
    gfx->fillRoundRect(x + 4, y + 10, 48, 5, 2, 0xC618);
    gfx->fillRoundRect(x + 10, y + 22, 42, 5, 2, 0x8410);
    gfx->fillRoundRect(x + 6, y + 34, 46, 5, 2, 0xC618);
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    drawCloud(x + 2, y + 2, cloud);
    for (int i = 0; i < 3; i++) drawFlake(x + 16 + i * 12, y + 40, 0xFFFF);
  } else if (code >= 95) {
    drawCloud(x + 2, y, cloud);
    drawBolt(x + 20, y + 26, accent);
  } else if (code >= 51) {
    drawCloud(x + 2, y + 2, cloud);
    for (int i = 0; i < 3; i++) {
      int dx = x + 16 + i * 12;
      int dy = y + 32;
      gfx->fillTriangle(dx, dy, dx - 6, dy + 12, dx + 2, dy + 12, accent);
    }
  } else {
    drawCloud(x + 2, y + 8, cloud);
  }
}

static void drawWeatherBlock(const AmbientClock &c) {
  uint16_t accent = weatherAccent(c.code);
  char num[8];
  snprintf(num, sizeof(num), "%d", c.temp);
  gfx->setTextSize(6);
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(num, 0, 0, &x1, &y1, &tw, &th);
  const int unitW = 6 * 3;
  int total = CLK_ICON_W + 14 + (int)tw + 8 + unitW;
  int x = (PANEL_W - total) / 2;
  drawWeatherIcon(x, CLK_TEMP_Y - 4, c.code);
  gfx->setTextSize(6);
  gfx->setTextColor(accent);
  gfx->setCursor(x + CLK_ICON_W + 14, CLK_TEMP_Y);
  gfx->print(num);
  gfx->setTextSize(3);
  gfx->setCursor(x + CLK_ICON_W + 14 + (int)tw + 8, CLK_TEMP_Y + 24);
  gfx->print(c.unit ? c.unit : 'F');
  if (c.summary[0]) drawCentered(c.summary, CLK_COND_Y, 0xC618, 3);
  if (c.has_range) {
    char range[16];
    snprintf(range, sizeof(range), "%d / %d", c.temp_lo, c.temp_hi);
    drawCentered(range, CLK_RANGE_Y, accent, 2);
  }
}

static bool paintClock(const AmbientClock &c) {
  if (!c.valid) {
    if (g_shown_min == -2) return false;
    gfx->fillRect(60, 110, 360, 130, 0x0000);
    drawTimeDigits("--:--");
    g_shown_min = -2;
    g_shown_hour = -1;
    g_shown_colon = -1;
    return true;
  }
  if (c.wday < 0 || c.wday > 6 || c.mon < 0 || c.mon > 11) return false;

  char hhmm[6];
  const char *ampm = nullptr;
  formatClock(c, hhmm, &ampm);
  bool colonOn = (c.second % 2) == 0;
  if (!colonOn && hhmm[2] == ':') hhmm[2] = ' ';

  if (g_shown_min != c.minute || g_shown_hour != c.hour || g_shown_mday != c.mday) {
    drawDateLine(c);
    drawTimeDigits(hhmm);
    if (ampm) drawAmPm(ampm);
    g_shown_min = c.minute;
    g_shown_hour = c.hour;
    g_shown_mday = c.mday;
    g_shown_colon = colonOn ? 1 : 0;
    return true;
  }
  if (g_shown_colon != (colonOn ? 1 : 0)) {
    drawColon(colonOn);
    g_shown_colon = colonOn ? 1 : 0;
    return true;
  }
  return false;
}

static bool paintWeather(const AmbientClock &c) {
  int state = c.weather_valid ? 1 : (c.weather_failed ? 2 : 0);
  if (state == 0 && g_shown_wstate == 0) return false;
  if (state == 2 && g_shown_wstate == 2) return false;
  if (state == 1 && g_shown_wstate == 1 && c.temp == g_shown_temp && c.unit == g_shown_unit &&
      c.code == g_shown_code && c.has_range == g_shown_range && c.temp_lo == g_shown_lo &&
      c.temp_hi == g_shown_hi && strcmp(c.summary, g_shown_sum) == 0) {
    return false;
  }

  gfx->fillRect(16, 240, 448, 136, 0x0000);
  if (state == 1) {
    drawWeatherBlock(c);
    g_shown_temp = c.temp;
    g_shown_code = c.code;
    g_shown_lo = c.temp_lo;
    g_shown_hi = c.temp_hi;
    g_shown_range = c.has_range;
    g_shown_unit = c.unit;
    strncpy(g_shown_sum, c.summary, sizeof(g_shown_sum) - 1);
    g_shown_sum[sizeof(g_shown_sum) - 1] = '\0';
  } else if (state == 2) {
    drawCentered("Weather unavailable", CLK_TEMP_Y + 16, 0x8410, 2);
  }
  g_shown_wstate = state;
  return true;
}

static bool paintIdleStatus() {
  int state = g_offline ? 2 : (g_muted ? 1 : 0);
  if (state == g_shown_status) return false;
  gfx->fillRect(80, CLK_STATUS_Y - 4, 320, 28, 0x0000);
  if (state == 2) drawCentered("OFFLINE", CLK_STATUS_Y, 0x8410, 2);
  else if (state == 1) drawCentered("MUTED", CLK_STATUS_Y, 0x8410, 2);
  g_shown_status = state;
  return true;
}

static void resetClockStamps() {
  g_shown_min = -1;
  g_shown_hour = -1;
  g_shown_mday = -1;
  g_shown_colon = -1;
  g_shown_wstate = -1;
  g_shown_status = -1;
}

// Idle face: local time and weather while no Cursor session is on screen.
static void drawAmbient(bool full) {
  if (!gfx) return;
  AmbientClock clock;
  ambientRead(clock);
  if (full || !g_clock_face) {
    fillScreen(0x0000);
    g_clock_face = true;
    resetClockStamps();
  }
  bool drew = paintClock(clock);
  if (paintWeather(clock)) drew = true;
  if (paintIdleStatus()) drew = true;
  if (drew && disp) disp->flush();
}

static void drawHero() {
  if (g_sessions.empty()) {
    g_focus = BuddyState::Idle;
    g_can_act = false;
    drawAmbient(!g_clock_face);
    return;
  }
  g_clock_face = false;

  BuddyState st = g_offline ? BuddyState::Offline : BuddyState::Idle;
  String project = "";
  String message = "Waiting for agents";
  int elapsed = 0;

  if (!g_offline && !g_sessions.empty()) {
    size_t idx = indexOfView();
    g_focus_idx = idx;
    const auto &s = g_sessions[idx];
    st = s.state;
    project = s.project;
    message = s.message.length() ? s.message : stateLabel(st);
    elapsed = s.elapsed_seconds;
    g_can_act = s.can_act;
    g_view_id = s.session_id;
  } else {
    g_can_act = false;
  }
  g_focus = st;
  if (!gfx) return;

  uint16_t accent = colorFor(st);
  fillScreen(0x0000);
  gfx->fillRoundRect(180, 36, 120, 6, 3, accent);

  const char *title = g_offline ? "OFFLINE" : (st == BuddyState::Attention ? "ATTENTION" : BUDDY_DEVICE_NAME);
  drawCentered(title, 58, accent, 2);

  // Default GFX font is ASCII-only; keep these single-byte.
  const char *glyph = ".";
  if (st == BuddyState::Working) glyph = "o";
  else if (st == BuddyState::Attention) glyph = "!";
  else if (st == BuddyState::Complete) glyph = "+";
  else if (st == BuddyState::Error) glyph = "x";
  else if (st == BuddyState::Offline) glyph = "o";
  drawCentered(glyph, 118, accent, 5);

  drawCentered(stateLabel(st), 200, 0xFFFF, 3);

  if (project.length()) {
    char line[64];
    snprintf(line, sizeof(line), "%s  %02d:%02d", project.c_str(), elapsed / 60, elapsed % 60);
    drawCentered(line, 252, 0xC618, 2);
  }

  if (message.length() > 36) message = message.substring(0, 33) + "...";
  drawCentered(message, 276, 0x8410, 1);
  if (!g_offline && g_sessions.size() > 1) {
    char pager[16];
    snprintf(pager, sizeof(pager), "%u / %u", (unsigned)(g_focus_idx + 1), (unsigned)g_sessions.size());
    drawChevron(CHEV_L_X, "<");
    drawCentered(pager, CHEV_Y + 14, 0xC618, 2);
    drawChevron(CHEV_R_X, ">");
  }

  gfx->fillRoundRect(BTN_L_X, BTN_Y, BTN_W, BTN_H, 12, g_can_act ? 0x4000 : (g_muted ? 0x4208 : 0x2104));
  gfx->fillRoundRect(BTN_R_X, BTN_Y, BTN_W, BTN_H, 12, g_can_act ? 0x0320 : 0x2104);
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  const char *leftLabel = g_can_act ? "CANCEL" : (g_muted ? "UNMUTE" : "MUTE");
  const char *rightLabel = g_can_act ? "RUN" : "DISMISS";
  int16_t x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(leftLabel, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(BTN_L_X + (BTN_W - (int)tw) / 2, BTN_Y + 16);
  gfx->print(leftLabel);
  gfx->getTextBounds(rightLabel, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(BTN_R_X + (BTN_W - (int)tw) / 2, BTN_Y + 16);
  gfx->print(rightLabel);
  if (disp) disp->flush();
}

static void touchReset() {
  exioSet(EXIO_TP_RST, false);
  delay(10);
  exioSet(EXIO_TP_RST, true);
  delay(50);
  uint8_t no_sleep = 0xFF;
  Wire.beginTransmission(CST820_ADDR);
  Wire.write(0xFE);
  Wire.write(no_sleep);
  Wire.endTransmission();
}

static bool readTouch(int16_t &x, int16_t &y) {
  if (digitalRead(CST820_INT_PIN) == HIGH) return false;
  Wire.beginTransmission(CST820_ADDR);
  Wire.write(0x01);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)CST820_ADDR, (uint8_t)6) < 6) return false;
  Wire.read();  // gesture
  uint8_t points = Wire.read();
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  if ((points & 0x0F) == 0) return false;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return x >= 0 && x < PANEL_W && y >= 0 && y < PANEL_H;
}

// GFX 1.4.7 keeps the esp_lcd handle private. The mirror matches that class so
// sleep can stop RGB DMA; sizeof is checked, and the framebuffer pointer is
// checked before the handle is deleted.
struct RgbPanelMirror {
  int32_t speed;
  int8_t de, vsync, hsync, pclk;
  int8_t r0, r1, r2, r3, r4;
  int8_t g0, g1, g2, g3, g4, g5;
  int8_t b0, b1, b2, b3, b4;
  uint16_t hsync_polarity;
  uint16_t hsync_front_porch;
  uint16_t hsync_pulse_width;
  uint16_t hsync_back_porch;
  uint16_t vsync_polarity;
  uint16_t vsync_front_porch;
  uint16_t vsync_pulse_width;
  uint16_t vsync_back_porch;
  uint16_t pclk_active_neg;
  int32_t prefer_speed;
  bool use_big_endian;
  uint16_t de_idle_high;
  uint16_t pclk_idle_high;
  esp_lcd_panel_handle_t panel_handle;
  esp_rgb_panel_t *rgb_panel;
};

static_assert(sizeof(RgbPanelMirror) == sizeof(Arduino_ESP32RGBPanel),
              "Arduino_ESP32RGBPanel layout changed; update RgbPanelMirror");
static_assert(alignof(RgbPanelMirror) == alignof(Arduino_ESP32RGBPanel),
              "Arduino_ESP32RGBPanel alignment changed; update RgbPanelMirror");

static bool startPanel() {
  if (disp) return true;
  rgb = new Arduino_ESP32RGBPanel(
      RGB_DE, RGB_VSYNC, RGB_HSYNC, RGB_PCLK,
      RGB_R1, RGB_R2, RGB_R3, RGB_R4, RGB_R5,
      RGB_G0, RGB_G1, RGB_G2, RGB_G3, RGB_G4, RGB_G5,
      RGB_B1, RGB_B2, RGB_B3, RGB_B4, RGB_B5,
      1, 50, 8, 10,
      1, 8, 3, 8,
      0, 12 * 1000 * 1000, false, 0, 0);
  disp = new Arduino_RGB_Display(PANEL_W, PANEL_H, rgb, 0, false);
  gfx = disp;
  if (!gfx->begin()) {
    Serial.println("[ui] RGB panel begin failed");
    delete disp;
    disp = nullptr;
    gfx = nullptr;
    delete rgb;
    rgb = nullptr;
    return false;
  }
  Serial.printf("[ui] RGB panel ready, PSRAM %u\n", ESP.getPsramSize());
  gfx->setRotation(0);
  return true;
}

static bool releaseRgbPanel() {
  if (!rgb || !disp) return disp == nullptr && rgb == nullptr;
  uint16_t *fb = disp->getFramebuffer();
  auto *mirror = reinterpret_cast<RgbPanelMirror *>(rgb);
  uintptr_t panelBits = reinterpret_cast<uintptr_t>(mirror->rgb_panel);
  bool panelOk = mirror->panel_handle && mirror->rgb_panel &&
                 mirror->panel_handle == reinterpret_cast<esp_lcd_panel_handle_t>(mirror->rgb_panel) &&
                 panelBits >= 0x3C000000u && panelBits < 0x40000000u &&
                 fb && mirror->rgb_panel->fb == reinterpret_cast<uint8_t *>(fb);
  if (!panelOk) return false;
  esp_lcd_panel_del(mirror->panel_handle);
  mirror->panel_handle = nullptr;
  mirror->rgb_panel = nullptr;
  delete disp;
  disp = nullptr;
  gfx = nullptr;
  delete rgb;
  rgb = nullptr;
  return true;
}

void uiBegin() {
  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, HIGH);
  pinMode(CST820_INT_PIN, INPUT_PULLUP);

  if (!st7701Init()) {
    Serial.println("[ui] ST7701 init failed");
  }

  startPanel();
  touchReset();
  g_last_input = millis();
  drawHero();
}

void uiSetOffline(bool offline) {
  if (g_offline != offline) {
    g_offline = offline;
    drawHero();
  }
}

void uiApplySnapshot(const std::vector<BuddySession> &sessions, bool muted, const String &focused_id) {
  // A new complete / error / attention pulls the panel to that conversation.
  // A strictly more urgent alert already on screen keeps the panel there.
  int bestRank = 100;
  String bestId;
  for (const auto &s : sessions) {
    if (!isAlert(s.state)) continue;
    bool changed = true;
    for (const auto &old : g_sessions) {
      if (old.session_id == s.session_id) {
        changed = old.state != s.state || old.message != s.message;
        break;
      }
    }
    if (!changed) continue;
    int rank = alertRank(s.state);
    if (rank < bestRank) {
      bestRank = rank;
      bestId = s.session_id;
    }
  }

  int shownRank = 100;
  for (const auto &s : sessions) {
    if (s.session_id == g_view_id) {
      shownRank = alertRank(s.state);
      break;
    }
  }
  bool jumped = false;
  if (bestId.length() && !(shownRank < bestRank)) {
    g_view_id = bestId;
    jumped = true;
  }

  g_sessions = sessions;
  g_focused_id = focused_id;
  bool stillThere = false;
  for (const auto &s : g_sessions) {
    if (s.session_id == g_view_id) {
      stillThere = true;
      break;
    }
  }
  if (!stillThere) {
    g_view_id = "";
    if (focused_id.length()) {
      for (const auto &s : g_sessions) {
        if (s.session_id == focused_id) {
          g_view_id = focused_id;
          break;
        }
      }
    }
    if (!g_view_id.length() && !g_sessions.empty()) g_view_id = g_sessions[0].session_id;
  }

  g_muted = muted;
  buzzerSetMuted(muted);
  g_offline = false;
  drawHero();

  if (!muted && jumped) chimeFor(g_focus);
}

bool uiPollAck(String &session_id, String &action) {
  if (!g_has_pending) return false;
  session_id = g_pending_session;
  action = g_pending_action;
  g_has_pending = false;
  return true;
}

static void stepView(int dir);

static const int SW_DEBOUNCE_MS = 30;
static const int SW_PINS[] = {SW_PREV_PIN, SW_NEXT_PIN, SW_MUTE_PIN, SW_DISMISS_PIN};
static const int SW_COUNT = 4;
static bool g_sw_armed = false;
static bool g_sw_down[SW_COUNT];
static uint32_t g_sw_since[SW_COUNT];

void uiArmSwitches() {
  Serial.flush();
  Serial.end();
  for (int i = 0; i < SW_COUNT; i++) {
    pinMode(SW_PINS[i], INPUT_PULLUP);
    g_sw_down[i] = digitalRead(SW_PINS[i]) == LOW;
    g_sw_since[i] = 0;
  }
  g_sw_armed = true;
}

static void onSwitch(int index) {
  if (index == 0) stepView(-1);
  else if (index == 1) stepView(1);
  else if (index == 2) {
    g_pending_action = g_muted ? "unmute" : "mute";
    g_pending_session = "";
    g_has_pending = true;
  } else if (index == 3) {
    if (!g_view_id.length()) return;
    g_pending_action = "dismiss";
    g_pending_session = g_view_id;
    g_has_pending = true;
  }
}

static void pollSwitches() {
  if (!g_sw_armed) return;
  uint32_t now = millis();
  for (int i = 0; i < SW_COUNT; i++) {
    bool down = digitalRead(SW_PINS[i]) == LOW;
    if (down) g_last_input = now;
    if (down == g_sw_down[i]) {
      g_sw_since[i] = 0;
      continue;
    }
    if (g_sw_since[i] == 0) {
      g_sw_since[i] = now;
      continue;
    }
    if (now - g_sw_since[i] < SW_DEBOUNCE_MS) continue;
    if (down && g_has_pending) continue;
    g_sw_down[i] = down;
    g_sw_since[i] = 0;
    if (down) onSwitch(i);
  }
}

static void stepView(int dir) {
  if (g_offline || g_sessions.size() < 2) return;
  size_t n = g_sessions.size();
  size_t idx = indexOfView();
  size_t next = dir < 0 ? (idx + n - 1) % n : (idx + 1) % n;
  g_view_id = g_sessions[next].session_id;
  drawHero();
}

static void syncInputs() {
  int16_t x, y;
  touchBegan = readTouch(x, y);
  g_last_input = millis();
  if (!g_sw_armed) return;
  for (int i = 0; i < SW_COUNT; i++) {
    g_sw_down[i] = digitalRead(SW_PINS[i]) == LOW;
    g_sw_since[i] = 0;
  }
}

uint32_t uiIdleMs() { return millis() - g_last_input; }

bool uiQuiesceForSleep() {
  digitalWrite(LCD_BL_PIN, LOW);
  st7701SleepIn();
  if (releaseRgbPanel()) return true;
  st7701Init();
  digitalWrite(LCD_BL_PIN, HIGH);
  g_clock_face = false;
  drawHero();
  return false;
}

void uiRestoreAfterSleep() {
  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, LOW);
  pinMode(CST820_INT_PIN, INPUT_PULLUP);
  st7701Init();
  startPanel();
  touchReset();
  g_clock_face = false;
  drawHero();
  digitalWrite(LCD_BL_PIN, HIGH);
  syncInputs();
  g_last_draw = millis();
}

void uiLoop() {
  int16_t x, y;
  if (readTouch(x, y)) {
    g_last_input = millis();
    if (!touchBegan) {
      touchBegan = true;
      if (!g_offline && g_sessions.size() > 1 && y >= CHEV_Y && y < CHEV_Y + CHEV_H) {
        if (x >= CHEV_L_X && x < CHEV_L_X + CHEV_W) stepView(-1);
        else if (x >= CHEV_R_X && x < CHEV_R_X + CHEV_W) stepView(1);
      } else if (!g_sessions.empty() && y >= BTN_Y && y <= BTN_Y + BTN_H) {
        if (x >= BTN_L_X && x <= BTN_L_X + BTN_W) {
          g_pending_action = g_can_act ? "cancel" : (g_muted ? "unmute" : "mute");
          g_pending_session = g_can_act ? g_view_id : "";
          g_has_pending = true;
        } else if (x >= BTN_R_X && x <= BTN_R_X + BTN_W) {
          g_pending_action = g_can_act ? "run" : "dismiss";
          g_pending_session = g_view_id;
          g_has_pending = true;
        }
      }
    }
  } else {
    touchBegan = false;
  }

  pollSwitches();

  if (g_sessions.empty()) {
    static uint32_t lastClock = 0;
    uint32_t now = millis();
    if (now - lastClock >= 200) {
      lastClock = now;
      drawAmbient(false);
    }
  } else if (millis() - g_last_draw > 5000) {
    g_last_draw = millis();
    if (!g_offline) drawHero();
  }
}
