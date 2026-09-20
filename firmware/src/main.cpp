#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "buzzer.h"
#include "ui.h"
#include "ws_client.h"

static void onSnapshot(const std::vector<BuddySession> &sessions, bool muted) {
  uiApplySnapshot(sessions, muted);
}

static void onConn(bool connected) {
  uiSetOffline(!connected);
  Serial.printf("[ws] %s\n", connected ? "connected" : "disconnected");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("Cursor Buddy ESP32");

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
}
