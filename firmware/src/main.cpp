#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "exio.h"
#include "buzzer.h"
#include "ui.h"
#include "ws_client.h"
#include "power.h"

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

  exioBegin();
  buzzerBegin();
  uiBegin();
  uiSetOffline(true);

  WiFi.mode(WIFI_STA);
  WiFi.begin(BUDDY_WIFI_SSID, BUDDY_WIFI_PASSWORD);
  Serial.printf("WiFi connecting to %s", BUDDY_WIFI_SSID);
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
    Serial.println("WiFi failed — will keep retrying via WS reconnect after connect");
  }

  buddyWsBegin(onSnapshot, onConn);
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
  WiFi.mode(WIFI_STA);
  WiFi.begin(BUDDY_WIFI_SSID, BUDDY_WIFI_PASSWORD);
  buddyWsResume();
  uiSetOffline(true);
}

static void maybeBatterySleep() {
  static uint32_t not_before = 0;
  if (millis() < not_before) return;
  if (uiIdleMs() < BUDDY_BATTERY_IDLE_MS) return;
  if (!powerIsDischarging()) return;

  uint32_t started = millis();
  if (!uiQuiesceForSleep()) {
    not_before = millis() + 15000;
    return;
  }
  suspendRadio();
  bool slept = powerLightSleep();
  resumeRadio();
  uiRestoreAfterSleep();
  // A stuck wake pin returns immediately and would strobe the backlight.
  if (!slept || millis() - started < 1000) not_before = millis() + 15000;
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t last = 0;
    if (millis() - last > 5000) {
      last = millis();
      WiFi.reconnect();
    }
  }
  buddyWsLoop();
  uiLoop();

  String sid, action;
  if (uiPollAck(sid, action)) {
    buddyWsSendAck(sid, action);
    Serial.printf("[ui] ack %s %s\n", action.c_str(), sid.c_str());
  }

  powerSample();
  maybeBatterySleep();
}
