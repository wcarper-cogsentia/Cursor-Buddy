#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "ws_client.h"
#include "settings.h"

static WebSocketsClient webSocket;
static SnapshotHandler g_onSnapshot = nullptr;
static ConnHandler g_onConn = nullptr;
static bool g_connected = false;
static bool g_mdns = false;
static String g_host;
static uint16_t g_port = 8787;
static String g_path = "/ws";
static uint32_t g_down_since = 0;

static void handlePayload(uint8_t *payload, size_t length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) return;
  if (doc["type"] != "snapshot") return;

  bool muted = doc["muted"] | false;
  String focused_id = doc["focused_session_id"] | "";
  std::vector<BuddySession> sessions;
  JsonArray arr = doc["sessions"].as<JsonArray>();
  for (JsonObject o : arr) {
    BuddySession s;
    s.session_id = o["session_id"] | "";
    s.project = o["project"] | "";
    s.state = parseState(o["state"] | "idle");
    s.message = o["message"] | "";
    s.elapsed_seconds = o["elapsed_seconds"] | 0;
    s.can_act = o["can_act"] | false;
    if (s.session_id.length()) sessions.push_back(s);
  }
  if (g_onSnapshot) g_onSnapshot(sessions, muted, focused_id);
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      g_connected = false;
      if (g_onConn) g_onConn(false);
      break;
    case WStype_CONNECTED:
      g_connected = true;
      g_down_since = 0;
      if (g_onConn) g_onConn(true);
      break;
    case WStype_TEXT:
      handlePayload(payload, length);
      break;
    default:
      break;
  }
}

static bool isLocalName(const String &host) {
  if (host.length() < 7) return false;
  String tail = host.substring(host.length() - 6);
  tail.toLowerCase();
  return tail == ".local";
}

static bool ensureMdns() {
  if (g_mdns) return true;
  char name[16];
  settingsRadioName(name, sizeof(name), true);
  g_mdns = MDNS.begin(name);
  return g_mdns;
}

static void connectSocket() {
  if (!g_host.length()) return;
  String host = g_host;
  if (isLocalName(host) && WiFi.status() == WL_CONNECTED && ensureMdns()) {
    String name = host.substring(0, host.length() - 6);
    IPAddress ip = MDNS.queryHost(name.c_str(), 2000);
    if (ip) {
      Serial.printf("[ws] %s is %s\n", g_host.c_str(), ip.toString().c_str());
      host = ip.toString();
    } else {
      Serial.printf("[ws] no answer for %s\n", g_host.c_str());
    }
  }
  webSocket.disconnect();
  webSocket.begin(host.c_str(), g_port, g_path.c_str());
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(3000);
  g_down_since = millis();
}

void buddyWsBegin(const String &host, uint16_t port, const String &path, SnapshotHandler onSnapshot, ConnHandler onConn) {
  g_host = host;
  g_port = port ? port : 8787;
  g_path = path.length() ? path : "/ws";
  g_onSnapshot = onSnapshot;
  g_onConn = onConn;
  connectSocket();
}

void buddyWsLoop() {
  webSocket.loop();
  if (!isLocalName(g_host)) return;
  if (g_connected) {
    g_down_since = 0;
    return;
  }
  if (!g_down_since) g_down_since = millis();
  if ((int32_t)(millis() - g_down_since) < 20000) return;
  connectSocket();
}

void buddyWsSuspend() {
  webSocket.disconnect();
  g_connected = false;
  g_down_since = 0;
}

void buddyWsResume() { connectSocket(); }

bool buddyWsConnected() { return g_connected; }

void buddyWsSendAck(const String &session_id, const String &action) {
  if (!g_connected) return;
  JsonDocument doc;
  doc["type"] = "ack";
  doc["session_id"] = session_id;
  doc["action"] = action;
  String out;
  serializeJson(doc, out);
  webSocket.sendTXT(out);
}
