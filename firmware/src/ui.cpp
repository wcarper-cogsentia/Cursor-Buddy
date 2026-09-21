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

static void drawHero() {
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
      } else if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
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

  if (millis() - g_last_draw > 5000) {
    g_last_draw = millis();
    if (!g_offline) drawHero();
  }
}
