#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "ws_client.h"
#include "config.h"

static WebSocketsClient webSocket;
static SnapshotHandler g_onSnapshot = nullptr;
static ConnHandler g_onConn = nullptr;
static bool g_connected = false;

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
      if (g_onConn) g_onConn(true);
      break;
    case WStype_TEXT:
      handlePayload(payload, length);
      break;
    default:
      break;
  }
}

void buddyWsBegin(SnapshotHandler onSnapshot, ConnHandler onConn) {
  g_onSnapshot = onSnapshot;
  g_onConn = onConn;
  webSocket.begin(BUDDY_HOST, BUDDY_WS_PORT, BUDDY_WS_PATH);
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(3000);
}

void buddyWsLoop() { webSocket.loop(); }

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
