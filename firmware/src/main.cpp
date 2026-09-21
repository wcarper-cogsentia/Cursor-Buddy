#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "board.h"
#include "exio.h"
#include "buzzer.h"
#include "ui.h"
#include "ambient.h"
#include "ws_client.h"
#include "power.h"
#include "settings.h"
#include "setup_portal.h"

static bool g_portal = false;

static bool bootChordHeld() {
  pinMode(SW_MUTE_PIN, INPUT_PULLUP);
  pinMode(SW_DISMISS_PIN, INPUT_PULLUP);
  delay(10);
  if (digitalRead(SW_MUTE_PIN) != LOW || digitalRead(SW_DISMISS_PIN) != LOW) return false;
  Serial.println("[setup] hold Mute and Dismiss");
  uint32_t start = millis();
  while (millis() - start < 600) {
    if (digitalRead(SW_MUTE_PIN) != LOW || digitalRead(SW_DISMISS_PIN) != LOW) return false;
    delay(20);
  }
  Serial.println("[setup] opening setup");
  return true;
}

static void joinNetwork() {
  char hostname[16];
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  settingsRadioName(hostname, sizeof(hostname), true);
  WiFi.setHostname(hostname);

  wifi_config_t conf = {};
  const char *ssid = settings().ssid.c_str();
  const char *pass = settings().password.c_str();
  memcpy(conf.sta.ssid, ssid, strnlen(ssid, sizeof(conf.sta.ssid)));
  if (pass[0]) {
    memcpy(conf.sta.password, pass, strnlen(pass, sizeof(conf.sta.password)));
    conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    conf.sta.threshold.authmode = WIFI_AUTH_OPEN;
  }
  conf.sta.pmf_cfg.capable = true;
  // WiFi.begin() refuses to apply a new network unless a disconnect succeeds,
  // and disconnect fails when the radio is not already associated.
  esp_wifi_set_config(WIFI_IF_STA, &conf);
  esp_wifi_connect();
}

static void onSnapshot(const std::vector<BuddySession> &sessions, bool muted, const String &focused_id) {
  uiApplySnapshot(sessions, muted, focused_id);
}

static void onConn(bool connected) {
  uiSetOffline(!connected);
  Serial.printf("[ws] %s\n", connected ? "connected" : "disconnected");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Cursor Buddy ESP32");
  Serial.printf("PSRAM: %u free %u\n", ESP.getPsramSize(), ESP.getFreePsram());

  bool held = bootChordHeld();
  settingsBegin();
  bool portal = !settings().configured() || settingsTakeSetupRequest() || held;

  exioBegin();
  buzzerBegin();
  uiBegin(portal);

  if (portal) {
    const char *ap = setupPortalStart();
    uiShowSetup(ap, setupPortalUrl());
    uiArmSwitches();
    g_portal = true;
    return;
  }

  uiSetOffline(true);
  joinNetwork();
  Serial.printf("WiFi connecting to %s", settings().ssid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed");
  }

  ambientBegin();
  buddyWsBegin(settings().host, settings().port, settings().path, onSnapshot, onConn);
  powerBegin();
  uiArmSwitches();
}

static void suspendRadio() {
  buddyWsSuspend();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
  delay(50);
}

static void resumeRadio() {
  joinNetwork();
  buddyWsResume();
  uiSetOffline(true);
}

static void maybeBatterySleep() {
  static uint32_t not_before = 0;
  if (millis() < not_before) return;
  if (uiIdleMs() < settings().batteryIdleMs) return;
  if (!powerIsDischarging()) return;

  if (!ambientQuiesce()) {
    not_before = millis() + 15000;
    return;
  }
  uint32_t started = millis();
  if (!uiQuiesceForSleep()) {
    ambientResume();
    not_before = millis() + 15000;
    return;
  }
  suspendRadio();
  bool slept = powerLightSleep();
  resumeRadio();
  uiRestoreAfterSleep();
  ambientResume();
  // A stuck wake pin returns immediately and would strobe the backlight.
  if (!slept || millis() - started < 1000) not_before = millis() + 15000;
}

void loop() {
  if (g_portal) {
    setupPortalLoop();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t last = 0;
    if (millis() - last > 5000) {
      last = millis();
      esp_wifi_connect();
    }
  }
  buddyWsLoop();
  uiLoop();
  if (uiPollSetup()) {
    if (settingsRequestSetup()) ESP.restart();
  }

  String sid, action;
  if (uiPollAck(sid, action)) {
    buddyWsSendAck(sid, action);
    Serial.printf("[ui] ack %s %s\n", action.c_str(), sid.c_str());
  }

  powerSample();
  maybeBatterySleep();
}
