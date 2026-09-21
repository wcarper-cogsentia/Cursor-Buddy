#include "settings.h"
#include "config.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>

static BuddySettings g_settings;
static bool g_loaded = false;

static String clip(const char *text, size_t maxLen) {
  String out = text ? text : "";
  if (out.length() > maxLen) out.remove(maxLen);
  return out;
}

static BuddySettings makeDefaults() {
  BuddySettings s;
  s.port = 8787;
  s.path = "/ws";
  s.name = "CURSOR BUDDY";
  s.tempUnit = BuddyTempUnit::Auto;
  s.clock24h = false;
  s.hasLocation = false;
  s.batteryIdleMs = 30UL * 60UL * 1000UL;
  return s;
}

static void clampIdle(BuddySettings &s) {
  const uint32_t minMs = 60UL * 1000UL;
  const uint32_t maxMs = 240UL * 60UL * 1000UL;
  if (s.batteryIdleMs < minMs) s.batteryIdleMs = minMs;
  if (s.batteryIdleMs > maxMs) s.batteryIdleMs = maxMs;
}

static void applyCompileTime(BuddySettings &s) {
  bool ssidReal = BUDDY_WIFI_SSID[0] != '\0' && strcmp(BUDDY_WIFI_SSID, "YOUR_SSID") != 0;
  if (ssidReal) {
    s.ssid = clip(BUDDY_WIFI_SSID, 32);
    if (strcmp(BUDDY_WIFI_PASSWORD, "YOUR_PASSWORD") != 0) s.password = clip(BUDDY_WIFI_PASSWORD, 64);
    if (BUDDY_HOST[0] != '\0' && strcmp(BUDDY_HOST, "YOUR_HOST") != 0) s.host = clip(BUDDY_HOST, 63);
  }
  if (BUDDY_WS_PORT >= 1 && BUDDY_WS_PORT <= 65535) s.port = (uint16_t)BUDDY_WS_PORT;
  if (BUDDY_WS_PATH[0] == '/') s.path = clip(BUDDY_WS_PATH, 32);
  if (BUDDY_DEVICE_NAME[0] != '\0') s.name = clip(BUDDY_DEVICE_NAME, 16);
#ifdef BUDDY_TZ
  s.tz = clip(BUDDY_TZ, 48);
#endif
#if defined(BUDDY_LATITUDE) && defined(BUDDY_LONGITUDE)
  s.hasLocation = true;
  s.latitude = (float)BUDDY_LATITUDE;
  s.longitude = (float)BUDDY_LONGITUDE;
#endif
#if defined(BUDDY_TEMP_C)
  s.tempUnit = BUDDY_TEMP_C ? BuddyTempUnit::Celsius : BuddyTempUnit::Fahrenheit;
#endif
#if defined(BUDDY_24H)
  s.clock24h = BUDDY_24H ? true : false;
#endif
#ifdef BUDDY_BATTERY_IDLE_MS
  s.batteryIdleMs = BUDDY_BATTERY_IDLE_MS;
#endif
  clampIdle(s);
}

static bool writeCfg(Preferences &prefs, const BuddySettings &s) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["ssid"] = s.ssid;
  doc["pass"] = s.password;
  doc["host"] = s.host;
  doc["port"] = s.port;
  doc["path"] = s.path;
  doc["name"] = s.name;
  doc["tz"] = s.tz;
  if (s.hasLocation) {
    doc["lat"] = s.latitude;
    doc["lon"] = s.longitude;
  }
  const char *temp = "auto";
  if (s.tempUnit == BuddyTempUnit::Fahrenheit) temp = "f";
  else if (s.tempUnit == BuddyTempUnit::Celsius) temp = "c";
  doc["temp"] = temp;
  doc["h24"] = s.clock24h;
  doc["idle"] = s.batteryIdleMs;
  String raw;
  if (serializeJson(doc, raw) == 0 || raw.length() > 1400) return false;
  return prefs.putString("cfg", raw) == raw.length();
}

static bool readCfg(Preferences &prefs, BuddySettings &s) {
  String raw = prefs.getString("cfg", "");
  if (!raw.length() || raw.length() > 1400) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  s = makeDefaults();
  s.ssid = clip(doc["ssid"] | "", 32);
  s.password = clip(doc["pass"] | "", 64);
  s.host = clip(doc["host"] | "", 63);
  int port = doc["port"] | 8787;
  if (port >= 1 && port <= 65535) s.port = (uint16_t)port;
  String path = clip(doc["path"] | "", 32);
  if (path.length() && path[0] == '/') s.path = path;
  String name = clip(doc["name"] | "", 16);
  if (name.length()) s.name = name;
  s.tz = clip(doc["tz"] | "", 48);
  JsonVariant lat = doc["lat"];
  JsonVariant lon = doc["lon"];
  if (!lat.isNull() && !lon.isNull()) {
    s.latitude = lat.as<float>();
    s.longitude = lon.as<float>();
    s.hasLocation = s.latitude >= -90.f && s.latitude <= 90.f && s.longitude >= -180.f && s.longitude <= 180.f;
  }
  const char *temp = doc["temp"] | "auto";
  if (strcmp(temp, "f") == 0) s.tempUnit = BuddyTempUnit::Fahrenheit;
  else if (strcmp(temp, "c") == 0) s.tempUnit = BuddyTempUnit::Celsius;
  else s.tempUnit = BuddyTempUnit::Auto;
  s.clock24h = doc["h24"] | false;
  s.batteryIdleMs = doc["idle"] | (30UL * 60UL * 1000UL);
  clampIdle(s);
  return true;
}

void settingsBegin() {
  if (g_loaded) return;
  g_loaded = true;
  g_settings = makeDefaults();
  Preferences prefs;
  if (!prefs.begin("buddy", false)) {
    Serial.println("[settings] storage unavailable");
    return;
  }
  if (!prefs.getBool("seeded", false)) {
    applyCompileTime(g_settings);
    if (writeCfg(prefs, g_settings) && prefs.putBool("seeded", true) > 0) {
      Serial.println(g_settings.configured() ? "[settings] saved config.h on this puck" : "[settings] setup required");
    } else {
      Serial.println("[settings] could not store settings");
    }
  } else if (!prefs.isKey("cfg")) {
    g_settings = makeDefaults();
    Serial.println("[settings] setup required");
  } else if (!readCfg(prefs, g_settings)) {
    g_settings = makeDefaults();
    Serial.println("[settings] saved settings unreadable");
  } else if (g_settings.configured()) {
    Serial.printf("[settings] %s -> %s:%u%s\n", g_settings.ssid.c_str(), g_settings.host.c_str(), g_settings.port,
                  g_settings.path.c_str());
  } else {
    Serial.println("[settings] setup required");
  }
  prefs.end();
}

const BuddySettings &settings() {
  if (!g_loaded) settingsBegin();
  return g_settings;
}

bool settingsSave(const BuddySettings &next) {
  Preferences prefs;
  if (!prefs.begin("buddy", false)) return false;
  bool ok = writeCfg(prefs, next);
  if (ok) {
    prefs.putBool("seeded", true);
    g_settings = next;
  }
  prefs.end();
  return ok;
}

bool settingsErase() {
  Preferences prefs;
  if (!prefs.begin("buddy", false)) return false;
  prefs.remove("cfg");
  prefs.putBool("seeded", true);
  prefs.remove("setup");
  prefs.end();
  g_settings = makeDefaults();
  return true;
}

bool settingsRequestSetup() {
  Preferences prefs;
  if (!prefs.begin("buddy", false)) return false;
  size_t n = prefs.putBool("setup", true);
  prefs.end();
  return n > 0;
}

bool settingsTakeSetupRequest() {
  Preferences prefs;
  if (!prefs.begin("buddy", false)) return false;
  bool on = prefs.getBool("setup", false);
  if (on) prefs.putBool("setup", false);
  prefs.end();
  return on;
}

void settingsRadioName(char *out, size_t n, bool lower) {
  if (!out || !n) return;
  out[0] = '\0';
  if (n < 12) return;
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  WiFi.macAddress(mac);
  bool blank = true;
  for (int i = 0; i < 6; i++) {
    if (mac[i]) blank = false;
  }
  if (blank) {
    uint64_t efuse = ESP.getEfuseMac();
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)((efuse >> (8 * i)) & 0xFF);
  }
  snprintf(out, n, lower ? "buddy-%02x%02x" : "Buddy-%02X%02X", mac[4], mac[5]);
}
