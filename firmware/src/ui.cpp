/**
 * Waveshare ESP32-S3-Touch-LCD-2.1 UI:
 * ST7701 RGB panel + CST820 touch + TCA9554 expander.
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <display/Arduino_RGB_Display.h>
#include <Wire.h>
#include "ui.h"
#include "buzzer.h"
#include "board.h"
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

static Arduino_ESP32RGBPanel *rgb = nullptr;
static Arduino_RGB_Display *disp = nullptr;
static Arduino_GFX *gfx = nullptr;

static std::vector<BuddySession> g_sessions;
static bool g_muted = false;
static bool g_offline = true;
static bool g_can_act = false;
static BuddyState g_focus = BuddyState::Idle;
static String g_focused_id;
static size_t g_focus_idx = 0;
static String g_pending_session;
static String g_pending_action;
static bool g_has_pending = false;
static uint32_t g_last_draw = 0;
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

static void drawHero() {
  BuddyState st = g_offline ? BuddyState::Offline : BuddyState::Idle;
  String project = "";
  String message = "Waiting for agents";
  String sid = "";
  int elapsed = 0;

  if (!g_offline && !g_sessions.empty()) {
    size_t idx = 0;
    if (g_focused_id.length()) {
      for (size_t i = 0; i < g_sessions.size(); i++) {
        if (g_sessions[i].session_id == g_focused_id) {
          idx = i;
          break;
        }
      }
    } else {
      for (size_t i = 0; i < g_sessions.size(); i++) {
        if (g_sessions[i].state == BuddyState::Attention || g_sessions[i].state == BuddyState::Error) {
          idx = i;
          break;
        }
      }
    }
    g_focus_idx = idx;
    const auto &s = g_sessions[idx];
    st = s.state;
    project = s.project;
    message = s.message.length() ? s.message : stateLabel(st);
    sid = s.session_id;
    elapsed = s.elapsed_seconds;
    g_can_act = s.can_act;
  } else {
    g_can_act = false;
  }
  g_focus = st;
  g_pending_session = sid;
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
  drawCentered(message, 292, 0x8410, 1);
  if (g_sessions.size() > 1) {
    char pager[16];
    snprintf(pager, sizeof(pager), "%u / %u", (unsigned)(g_focus_idx + 1), (unsigned)g_sessions.size());
    drawCentered(pager, 318, 0xC618, 1);
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

void uiBegin() {
  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, HIGH);
  pinMode(CST820_INT_PIN, INPUT_PULLUP);

  if (!st7701Init()) {
    Serial.println("[ui] ST7701 init failed");
  }

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
  } else {
    Serial.printf("[ui] RGB panel ready, PSRAM %u\n", ESP.getPsramSize());
    gfx->setRotation(0);
  }

  touchReset();
  drawHero();
}

void uiSetOffline(bool offline) {
  if (g_offline != offline) {
    g_offline = offline;
    drawHero();
  }
}

void uiApplySnapshot(const std::vector<BuddySession> &sessions, bool muted, const String &focused_id) {
  BuddyState prevFocus = g_focus;
  g_sessions = sessions;
  g_focused_id = focused_id;
  g_muted = muted;
  buzzerSetMuted(muted);
  g_offline = false;
  drawHero();

  if (!muted && g_focus != prevFocus) {
    if (g_focus == BuddyState::Attention) buzzerAttention();
    else if (g_focus == BuddyState::Complete) buzzerComplete();
    else if (g_focus == BuddyState::Error) buzzerError();
  }
}

bool uiPollAck(String &session_id, String &action) {
  if (!g_has_pending) return false;
  session_id = g_pending_session;
  action = g_pending_action;
  g_has_pending = false;
  return true;
}

void uiLoop() {
  int16_t x, y;
  if (readTouch(x, y)) {
    if (!touchBegan) {
      touchBegan = true;
      if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
        if (x >= BTN_L_X && x <= BTN_L_X + BTN_W) {
          g_pending_action = g_can_act ? "cancel" : (g_muted ? "unmute" : "mute");
          if (!g_can_act) g_pending_session = "";
          g_has_pending = true;
        } else if (x >= BTN_R_X && x <= BTN_R_X + BTN_W) {
          g_pending_action = g_can_act ? "run" : "dismiss";
          g_has_pending = true;
        }
      } else if (!g_sessions.empty() && y < BTN_Y) {
        size_t next = (g_focus_idx + 1) % g_sessions.size();
        g_pending_action = "focus";
        g_pending_session = g_sessions[next].session_id;
        g_has_pending = true;
      }
    }
  } else {
    touchBegan = false;
  }

  if (millis() - g_last_draw > 5000) {
    g_last_draw = millis();
    if (!g_offline) drawHero();
  }
}
