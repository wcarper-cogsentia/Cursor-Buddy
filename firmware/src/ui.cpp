/**
 * Minimal 480×480 status UI for Waveshare ESP32-S3-Touch-LCD-2.1.
 *
 * Uses Arduino_GFX when available. If the panel does not initialize with the
 * generic ST7701 wiring below, replace uiBegin()'s bus/panel setup with the
 * exact init from Waveshare's official Arduino demo for this board — keep the
 * draw*() helpers unchanged.
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "ui.h"
#include "buzzer.h"

#ifndef BUDDY_DEVICE_NAME
#define BUDDY_DEVICE_NAME "CURSOR BUDDY"
#endif

// --- Waveshare ESP32-S3-Touch-LCD-2.1 typical RGB/SPI pins (verify against wiki) ---
#define LCD_BL 38

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *gfx = nullptr;

static std::vector<BuddySession> g_sessions;
static bool g_muted = false;
static bool g_offline = true;
static BuddyState g_focus = BuddyState::Idle;
static String g_pending_session;
static String g_pending_action;
static bool g_has_pending = false;
static uint32_t g_last_draw = 0;

static uint16_t colorFor(BuddyState st) {
  switch (st) {
    case BuddyState::Working: return 0x047F;     // blue
    case BuddyState::Attention: return 0xFD20;   // orange
    case BuddyState::Complete: return 0x07E0;    // green
    case BuddyState::Error: return 0xF800;       // red
    case BuddyState::Offline: return 0x8410;     // gray
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
  int x = (480 - (int)w) / 2;
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
    // Prefer attention/error
    size_t idx = 0;
    for (size_t i = 0; i < g_sessions.size(); i++) {
      if (g_sessions[i].state == BuddyState::Attention || g_sessions[i].state == BuddyState::Error) {
        idx = i;
        break;
      }
    }
    const auto &s = g_sessions[idx];
    st = s.state;
    project = s.project;
    message = s.message.length() ? s.message : stateLabel(st);
    sid = s.session_id;
    elapsed = s.elapsed_seconds;
  }
  g_focus = st;

  uint16_t bg = 0x0000;
  uint16_t accent = colorFor(st);
  fillScreen(bg);
  gfx->fillRect(0, 0, 480, 8, accent);

  const char *title = g_offline ? "OFFLINE" : (st == BuddyState::Attention ? "NEEDS ATTENTION" : BUDDY_DEVICE_NAME);
  drawCentered(title, 40, accent, 2);

  // Big glyph
  const char *glyph = "·";
  if (st == BuddyState::Working) glyph = "●";
  else if (st == BuddyState::Attention) glyph = "!";
  else if (st == BuddyState::Complete) glyph = "✓";
  else if (st == BuddyState::Error) glyph = "✕";
  else if (st == BuddyState::Offline) glyph = "○";
  drawCentered(glyph, 140, accent, 6);

  drawCentered(stateLabel(st), 230, 0xFFFF, 3);

  if (project.length()) {
    char line[64];
    snprintf(line, sizeof(line), "%s · %02d:%02d", project.c_str(), elapsed / 60, elapsed % 60);
    drawCentered(line, 290, 0xC618, 2);
  }

  drawCentered(message, 340, 0x8410, 1);

  // Soft buttons regions (visual only; touch mapped in uiLoop)
  gfx->fillRoundRect(40, 400, 180, 56, 12, g_muted ? 0x4208 : 0x2104);
  gfx->fillRoundRect(260, 400, 180, 56, 12, 0x2104);
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(70, 418);
  gfx->print(g_muted ? "UNMUTE" : "MUTE");
  gfx->setCursor(290, 418);
  gfx->print("DISMISS");

  // stash dismiss target
  g_pending_session = sid;
}

void uiBegin() {
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  // Generic SPI bus placeholder — replace with Waveshare RGB panel init if needed.
  bus = new Arduino_ESP32SPI(2 /*DC*/, 15 /*CS*/, 14 /*SCK*/, 13 /*MOSI*/, GFX_NOT_DEFINED /*MISO*/, HSPI);
  gfx = new Arduino_ST7789(bus, 1 /*RST*/, 0 /*rotation*/, true /*IPS*/, 480, 480);
  if (!gfx->begin()) {
    // Fallback: keep running for Serial/WS even if panel fails
    Serial.println("[ui] display begin failed — using serial-only UI");
  } else {
    gfx->setRotation(0);
    fillScreen(0x0000);
  }
  drawHero();
}

void uiSetOffline(bool offline) {
  if (g_offline != offline) {
    g_offline = offline;
    drawHero();
  }
}

void uiApplySnapshot(const std::vector<BuddySession> &sessions, bool muted) {
  BuddyState prevFocus = g_focus;
  bool prevMuted = g_muted;
  g_sessions = sessions;
  g_muted = muted;
  buzzerSetMuted(muted);
  g_offline = false;
  drawHero();

  // Chime on state transitions into attention/complete/error
  if (!muted && g_focus != prevFocus) {
    if (g_focus == BuddyState::Attention) buzzerAttention();
    else if (g_focus == BuddyState::Complete) buzzerComplete();
    else if (g_focus == BuddyState::Error) buzzerError();
  }
  (void)prevMuted;
}

bool uiPollAck(String &session_id, String &action) {
  if (!g_has_pending) return false;
  session_id = g_pending_session;
  action = g_pending_action;
  g_has_pending = false;
  return true;
}

// Simple capacitive touch stub: Waveshare CST816 on I2C.
// If touch IC differs, replace readTouch() using Waveshare's Touch example.
#include <Wire.h>
#define TOUCH_SDA 11
#define TOUCH_SCL 10
#define TOUCH_ADDR 0x15

static bool touchBegan = false;

static bool readTouch(int16_t &x, int16_t &y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)TOUCH_ADDR, (uint8_t)5) < 5) return false;
  uint8_t points = Wire.read();
  if ((points & 0x0F) == 0) return false;
  uint8_t b1 = Wire.read();
  uint8_t b2 = Wire.read();
  uint8_t b3 = Wire.read();
  uint8_t b4 = Wire.read();
  x = ((b1 & 0x0F) << 8) | b2;
  y = ((b3 & 0x0F) << 8) | b4;
  return true;
}

void uiLoop() {
  static bool wireReady = false;
  if (!wireReady) {
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    wireReady = true;
  }

  int16_t x, y;
  if (readTouch(x, y)) {
    if (!touchBegan) {
      touchBegan = true;
      if (y >= 400 && y <= 460) {
        if (x >= 40 && x <= 220) {
          g_pending_action = g_muted ? "unmute" : "mute";
          g_pending_session = "";
          g_has_pending = true;
        } else if (x >= 260 && x <= 440) {
          g_pending_action = "dismiss";
          g_has_pending = true;
        }
      }
    }
  } else {
    touchBegan = false;
  }

  // Refresh elapsed clock occasionally
  if (millis() - g_last_draw > 5000) {
    g_last_draw = millis();
    if (!g_offline) drawHero();
  }
}
