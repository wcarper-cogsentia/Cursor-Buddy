#include "setup_portal.h"
#include "settings.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

static const IPAddress kApIp(192, 168, 4, 1);
static const char kUrl[] = "192.168.4.1";
static const uint32_t kGiveUpMs = 15UL * 60UL * 1000UL;
static const int kNetMax = 12;

static const char kCss[] = R"css(
:root { color-scheme: dark; }
* { box-sizing: border-box; }
body { margin: 0; font: 16px/1.45 system-ui, sans-serif; background: #0e0f12; color: #f3f5f8; }
main { max-width: 28rem; margin: 0 auto; padding: 1.25rem 1.1rem 2.5rem; }
h1 { font-size: 1.45rem; font-weight: 600; margin: 0 0 .35rem; }
p { margin: .6rem 0; }
.lead, .hint { color: #9aa3b2; }
.hint { font-size: .9rem; }
.error { background: #3b161c; color: #ffc7c7; padding: .75rem .9rem; border-radius: 12px; }
a { color: #9ec1ff; }
.net { display: flex; align-items: center; gap: .7rem; margin: .4rem 0; padding: .65rem .75rem; background: #171a20; border-radius: 12px; }
.net input { width: 1.15rem; height: 1.15rem; margin: 0; }
.tag { margin-left: auto; color: #9aa3b2; font-size: .85rem; }
.field { display: block; margin-top: 1rem; color: #c5cad3; font-size: .85rem; }
input[type="text"], input[type="password"], select {
  display: block; width: 100%; margin-top: .35rem; padding: .7rem .8rem;
  font: 1rem/1.3 system-ui, sans-serif; color: #f3f5f8;
  background: #171a20; border: 1px solid #2c313a; border-radius: 12px;
}
button { display: block; width: 100%; margin-top: 1rem; padding: .85rem 1rem; font: 1rem/1.2 system-ui, sans-serif; font-weight: 600; color: white; background: #3b82f6; border: 0; border-radius: 12px; }
button.secondary { background: #22262e; color: #e7e9ee; }
input:focus, select:focus, button:focus { outline: 2px solid #9ec1ff; outline-offset: 1px; }
details { margin-top: 1.2rem; }
summary { cursor: pointer; color: #d5d8e0; }
)css";

struct SeenNet {
  char ssid[33];
  int8_t rssi;
  bool open;
};

static DNSServer g_dns;
static WebServer g_server(80);
static char g_ap_ssid[16] = "Buddy";
static SeenNet g_nets[kNetMax];
static int g_net_count = 0;
static bool g_scan_started = false;
static bool g_scan_done = false;
static uint32_t g_scan_started_ms = 0;
static uint32_t g_up_at = 0;
static uint32_t g_last_http = 0;
static uint32_t g_restart_at = 0;
static bool g_task = false;
static String g_html;
static String g_error;
static String g_pass_echo;
static BuddySettings g_draft;

static String trimmed(String s) {
  s.trim();
  return s;
}

static bool isAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static void addEsc(const String &in) {
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') g_html += F("&amp;");
    else if (c == '<') g_html += F("&lt;");
    else if (c == '>') g_html += F("&gt;");
    else if (c == '"') g_html += F("&quot;");
    else g_html += c;
  }
}

static void pageOpen(bool refresh) {
  g_html = "";
  g_html.reserve(8000);
  g_html += F("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">");
  g_html += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  if (refresh) g_html += F("<meta http-equiv=\"refresh\" content=\"2;url=/\">");
  g_html += F("<title>Cursor Buddy</title><style>");
  g_html += kCss;
  g_html += F("</style></head><body><main><h1>Cursor Buddy</h1>");
}

static void pageClose() { g_html += F("</main></body></html>"); }

static void sendHtml() {
  g_last_http = millis();
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.sendHeader("Connection", "close");
  g_server.send(200, "text/html; charset=utf-8", g_html);
}

static void restartSoon() {
  if (!g_restart_at) g_restart_at = millis() + 1200;
}

static void startScan() {
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  g_scan_done = false;
  g_net_count = 0;
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
  g_scan_started = true;
  g_scan_started_ms = millis();
}

static void rememberNet(const String &ssid, int8_t rssi, bool open) {
  for (int i = 0; i < g_net_count; i++) {
    if (ssid == g_nets[i].ssid) {
      if (rssi > g_nets[i].rssi) {
        g_nets[i].rssi = rssi;
        g_nets[i].open = open;
      }
      return;
    }
  }
  int slot = g_net_count;
  if (g_net_count >= kNetMax) {
    slot = 0;
    for (int i = 1; i < g_net_count; i++) {
      if (g_nets[i].rssi < g_nets[slot].rssi) slot = i;
    }
    if (rssi <= g_nets[slot].rssi) return;
  } else {
    g_net_count++;
  }
  strncpy(g_nets[slot].ssid, ssid.c_str(), sizeof(g_nets[slot].ssid) - 1);
  g_nets[slot].ssid[sizeof(g_nets[slot].ssid) - 1] = '\0';
  g_nets[slot].rssi = rssi;
  g_nets[slot].open = open;
}

static void sortNets() {
  for (int i = 1; i < g_net_count; i++) {
    SeenNet key = g_nets[i];
    int j = i - 1;
    while (j >= 0 && g_nets[j].rssi < key.rssi) {
      g_nets[j + 1] = g_nets[j];
      j--;
    }
    g_nets[j + 1] = key;
  }
}

static void pollScan() {
  if (!g_scan_started || g_scan_done) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    if (millis() - g_scan_started_ms > 8000) {
      WiFi.scanDelete();
      g_scan_done = true;
      g_scan_started = false;
    }
    return;
  }
  g_scan_done = true;
  g_scan_started = false;
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length() || ssid.length() > 32 || ssid == g_ap_ssid) continue;
      bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
      rememberNet(ssid, (int8_t)WiFi.RSSI(i), open);
    }
    sortNets();
  }
  WiFi.scanDelete();
}

static int netIndex(const String &ssid) {
  for (int i = 0; i < g_net_count; i++) {
    if (ssid == g_nets[i].ssid) return i;
  }
  return -1;
}

static bool validSsid(const String &ssid) {
  if (!ssid.length() || ssid.length() > 32) return false;
  for (unsigned i = 0; i < ssid.length(); i++) {
    unsigned char c = ssid[i];
    if (c < 32 || c == 127) return false;
  }
  return true;
}

static bool validHost(const String &host) {
  if (!host.length() || host.length() > 63) return false;
  if (host.startsWith(".") || host.endsWith(".")) return false;
  for (unsigned i = 0; i < host.length(); i++) {
    char c = host[i];
    bool ok = isAlnum(c) || c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

static bool validPath(const String &path) {
  if (!path.length() || path.length() > 32 || path[0] != '/') return false;
  for (unsigned i = 0; i < path.length(); i++) {
    char c = path[i];
    bool ok = isAlnum(c) || c == '/' || c == '.' || c == '_' || c == '-' || c == '~';
    if (!ok) return false;
  }
  return true;
}

static bool validName(const String &name) {
  if (!name.length() || name.length() > 16) return false;
  if (name[0] == ' ' || name[name.length() - 1] == ' ') return false;
  for (unsigned i = 0; i < name.length(); i++) {
    char c = name[i];
    if (!(isAlnum(c) || c == ' ' || c == '-')) return false;
  }
  return true;
}

static bool validTz(const String &tz) {
  if (!tz.length()) return true;
  if (tz.length() > 48) return false;
  for (unsigned i = 0; i < tz.length(); i++) {
    char c = tz[i];
    bool ok = isAlnum(c) || c == '_' || c == '+' || c == '-' || c == '/' || c == ',' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static bool allDigits(const String &text) {
  if (!text.length()) return false;
  for (unsigned i = 0; i < text.length(); i++) {
    if (text[i] < '0' || text[i] > '9') return false;
  }
  return true;
}

static bool parseCoord(const String &text, float &out) {
  if (!text.length() || text.length() > 16) return false;
  char *end = nullptr;
  out = strtof(text.c_str(), &end);
  return end && end != text.c_str() && *end == '\0';
}

static bool readForm(BuddySettings &next, String &passEcho, String &err) {
  next = settings();
  String pick = g_server.arg("ssid");
  String other = trimmed(g_server.arg("ssid_other"));
  String ssid = pick.length() ? pick : other;
  String pass = g_server.arg("pass");
  passEcho = pass;
  String host = trimmed(g_server.arg("host"));
  String portText = trimmed(g_server.arg("port"));
  String path = trimmed(g_server.arg("path"));
  String name = trimmed(g_server.arg("name"));
  String tz = trimmed(g_server.arg("tz"));
  String latText = trimmed(g_server.arg("lat"));
  String lonText = trimmed(g_server.arg("lon"));
  String temp = g_server.arg("temp");
  String clock = g_server.arg("clock");
  String idleText = trimmed(g_server.arg("idle"));

  if (!portText.length()) portText = "8787";
  if (!path.length()) path = "/ws";
  if (!name.length()) name = "CURSOR BUDDY";
  if (!idleText.length()) idleText = "30";

  next.ssid = ssid;
  next.host = host;
  next.path = path;
  next.name = name;
  next.tz = tz;
  next.clock24h = clock == "24";
  if (temp == "f") next.tempUnit = BuddyTempUnit::Fahrenheit;
  else if (temp == "c") next.tempUnit = BuddyTempUnit::Celsius;
  else next.tempUnit = BuddyTempUnit::Auto;

  if (!ssid.length()) {
    err = "Choose a Wi-Fi network, or type its name.";
    return false;
  }
  if (!validSsid(ssid)) {
    err = "That network name is too long.";
    return false;
  }
  if (ssid == g_ap_ssid) {
    err = "Choose a network other than this puck.";
    return false;
  }
  if (pass.length() > 64) {
    err = "Wi-Fi password is too long.";
    return false;
  }
  int idx = netIndex(ssid);
  bool secure = idx >= 0 && !g_nets[idx].open;
  bool keep = !pass.length() && ssid == settings().ssid && settings().password.length();
  if (!pass.length() && !keep && secure) {
    err = "Enter the Wi-Fi password, or leave it blank to keep the saved password for this network.";
    return false;
  }
  next.password = keep ? settings().password : pass;

  if (host.equalsIgnoreCase("localhost") || host == "127.0.0.1" || host == kUrl) {
    err = "That address is this puck, not the computer running Buddy.";
    return false;
  }
  if (!validHost(host)) {
    err = "Enter the service host as a name or an IP address. Put the port in its own field.";
    return false;
  }
  if (!allDigits(portText)) {
    err = "Enter a port from 1 to 65535.";
    return false;
  }
  long port = portText.toInt();
  if (port < 1 || port > 65535) {
    err = "Enter a port from 1 to 65535.";
    return false;
  }
  next.port = (uint16_t)port;
  if (!validPath(path)) {
    err = "Enter a path that starts with /.";
    return false;
  }
  if (!validName(name)) {
    err = "Use letters, numbers, spaces, and hyphens for the name, up to 16 characters.";
    return false;
  }
  if (!validTz(tz)) {
    err = "The timezone should be a POSIX string, such as MST7MDT,M3.2.0,M11.1.0.";
    return false;
  }
  if (latText.length() || lonText.length()) {
    float lat = 0, lon = 0;
    if (!latText.length() || !lonText.length() || !parseCoord(latText, lat) || !parseCoord(lonText, lon)) {
      err = "Enter both latitude and longitude, or leave both blank.";
      return false;
    }
    if (lat < -90.f || lat > 90.f) {
      err = "Latitude must be between -90 and 90.";
      return false;
    }
    if (lon < -180.f || lon > 180.f) {
      err = "Longitude must be between -180 and 180.";
      return false;
    }
    next.hasLocation = true;
    next.latitude = lat;
    next.longitude = lon;
  } else {
    next.hasLocation = false;
    next.latitude = 0;
    next.longitude = 0;
  }
  if (!allDigits(idleText)) {
    err = "Enter sleep time as minutes from 1 to 240.";
    return false;
  }
  long mins = idleText.toInt();
  if (mins < 1 || mins > 240) {
    err = "Enter sleep time as minutes from 1 to 240.";
    return false;
  }
  next.batteryIdleMs = (uint32_t)mins * 60UL * 1000UL;
  err = "";
  return true;
}

static void addRadio(const String &value, const String &label, bool checked, bool showOpen) {
  g_html += F("<label class=\"net\"><input type=\"radio\" name=\"ssid\" value=\"");
  addEsc(value);
  g_html += '"';
  if (checked) g_html += F(" checked");
  g_html += '>';
  addEsc(label);
  if (showOpen) g_html += F("<span class=\"tag\">open</span>");
  g_html += F("</label>");
}

static void addOption(const char *value, const char *label, bool selected) {
  g_html += F("<option value=\"");
  g_html += value;
  g_html += '"';
  if (selected) g_html += F(" selected");
  g_html += '>';
  g_html += label;
  g_html += F("</option>");
}

static void addField(const char *label, const char *name, const String &value, const char *type, int maxLen, const char *placeholder, const char *extra) {
  g_html += F("<label class=\"field\">");
  g_html += label;
  g_html += F("<input type=\"");
  g_html += type;
  g_html += F("\" name=\"");
  g_html += name;
  g_html += '"';
  if (maxLen > 0) {
    g_html += F(" maxlength=\"");
    g_html += String(maxLen);
    g_html += '"';
  }
  if (placeholder && placeholder[0]) {
    g_html += F(" placeholder=\"");
    g_html += placeholder;
    g_html += '"';
  }
  if (extra && extra[0]) {
    g_html += ' ';
    g_html += extra;
  }
  g_html += F(" value=\"");
  addEsc(value);
  g_html += F("\"></label>");
}

static void renderWaiting() {
  pageOpen(true);
  g_html += F("<p class=\"lead\">Looking for Wi-Fi networks.</p>");
  g_html += F("<p><a href=\"/?ready=1\">Continue without waiting</a></p>");
  pageClose();
}

static void renderMessage(const char *text) {
  pageOpen(false);
  g_html += F("<p>");
  addEsc(text);
  g_html += F("</p>");
  pageClose();
}

static void renderForm() {
  pageOpen(false);
  g_html += F("<p class=\"lead\">Choose the Wi-Fi network and the computer that runs Buddy.</p>");
  if (g_error.length()) {
    g_html += F("<p class=\"error\">");
    addEsc(g_error);
    g_html += F("</p>");
  }
  g_html += F("<form method=\"post\" action=\"/save\" autocomplete=\"off\" spellcheck=\"false\">");
  bool matched = false;
  for (int i = 0; i < g_net_count; i++) {
    if (g_draft.ssid == g_nets[i].ssid) matched = true;
  }
  if (!g_net_count) g_html += F("<p class=\"hint\">No networks showed up. Type the name below, or scan again.</p>");
  for (int i = 0; i < g_net_count; i++) {
    addRadio(g_nets[i].ssid, g_nets[i].ssid, g_draft.ssid == g_nets[i].ssid, g_nets[i].open);
  }
  addRadio("", "Other", !matched, false);
  addField("Network name", "ssid_other", matched ? String() : g_draft.ssid, "text", 32, "Hidden network", "autocapitalize=\"off\" autocorrect=\"off\"");
  g_html += F("<p class=\"hint\"><a href=\"/?rescan=1\">Scan again</a></p>");
  g_html += F("<label class=\"field\">Wi-Fi password<input type=\"password\" name=\"pass\" maxlength=\"64\" autocomplete=\"new-password\" value=\"");
  addEsc(g_pass_echo);
  g_html += F("\"></label>");
  if (settings().password.length()) {
    g_html += F("<p class=\"hint\">Leave the password blank to keep the saved one for the current network.</p>");
  }
  addField("Service host", "host", g_draft.host, "text", 63, "192.168.1.42 or macbook.local", "autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\" required");
  g_html += F("<p class=\"hint\">An IP address or a hostname. A name ending in .local follows the computer when its address changes.</p>");
  addField("Port", "port", String(g_draft.port), "text", 5, "8787", "inputmode=\"numeric\"");
  addField("Path", "path", g_draft.path, "text", 32, "/ws", "autocapitalize=\"off\" spellcheck=\"false\"");
  addField("Name on the puck", "name", g_draft.name, "text", 16, "CURSOR BUDDY", "");

  bool advanced = g_draft.tz.length() || g_draft.hasLocation || g_draft.tempUnit != BuddyTempUnit::Auto || g_draft.clock24h ||
                  g_draft.batteryIdleMs != 30UL * 60UL * 1000UL;
  g_html += advanced ? F("<details open><summary>Clock, weather, and sleep</summary>")
                     : F("<details><summary>Clock, weather, and sleep</summary>");
  addField("Timezone", "tz", g_draft.tz, "text", 48, "MST7MDT,M3.2.0,M11.1.0", "autocapitalize=\"off\" spellcheck=\"false\"");
  addField("Latitude", "lat", g_draft.hasLocation ? String(g_draft.latitude, 4) : String(), "text", 16, "39.7392", "inputmode=\"decimal\"");
  addField("Longitude", "lon", g_draft.hasLocation ? String(g_draft.longitude, 4) : String(), "text", 16, "-104.9903", "inputmode=\"decimal\"");
  g_html += F("<p class=\"hint\">Leave the timezone and coordinates blank to use the network location. Automatic temperature follows the country, and Fahrenheit when the country is unknown.</p>");
  g_html += F("<label class=\"field\">Temperature<select name=\"temp\">");
  addOption("auto", "Automatic", g_draft.tempUnit == BuddyTempUnit::Auto);
  addOption("f", "Fahrenheit", g_draft.tempUnit == BuddyTempUnit::Fahrenheit);
  addOption("c", "Celsius", g_draft.tempUnit == BuddyTempUnit::Celsius);
  g_html += F("</select></label>");
  g_html += F("<label class=\"field\">Clock<select name=\"clock\">");
  addOption("12", "12-hour", !g_draft.clock24h);
  addOption("24", "24-hour", g_draft.clock24h);
  g_html += F("</select></label>");
  uint32_t mins = g_draft.batteryIdleMs / 60000UL;
  addField("Sleep on battery (minutes)", "idle", String(mins), "text", 3, "30", "inputmode=\"numeric\"");
  g_html += F("</details>");
  g_html += F("<button type=\"submit\">Save and connect</button></form>");
  if (settings().configured()) {
    g_html += F("<form method=\"post\" action=\"/cancel\"><button class=\"secondary\" type=\"submit\">Cancel</button></form>");
    g_html += F("<p class=\"hint\"><a href=\"/erase\">Erase saved settings</a></p>");
  }
  g_html += F("<p class=\"hint\">To open this page again, tap SETUP on the clock, or hold Mute and Dismiss for two seconds.</p>");
  pageClose();
}

static void renderErase() {
  pageOpen(false);
  g_html += F("<p>This forgets the Wi-Fi network and service host stored on this puck. You set them again on the next page.</p>");
  g_html += F("<form method=\"post\" action=\"/erase\"><button type=\"submit\">Erase and start over</button></form>");
  g_html += F("<form method=\"get\" action=\"/\"><button class=\"secondary\" type=\"submit\">Back</button></form>");
  pageClose();
}

static bool waitingForScan() {
  if (g_server.hasArg("ready")) return false;
  if (g_scan_done) return false;
  if (!g_scan_started) return true;
  return millis() - g_scan_started_ms < 8000;
}

static void handleRoot() {
  if (g_restart_at) return;
  if (g_server.hasArg("rescan")) startScan();
  g_error = "";
  g_pass_echo = "";
  g_draft = settings();
  if (waitingForScan()) renderWaiting();
  else renderForm();
  sendHtml();
}

static void handleSave() {
  if (g_restart_at) return;
  g_last_http = millis();
  BuddySettings next;
  String passEcho;
  String err;
  if (!readForm(next, passEcho, err)) {
    g_error = err;
    g_pass_echo = passEcho;
    g_draft = next;
    renderForm();
    sendHtml();
    return;
  }
  if (!settingsSave(next)) {
    g_error = "Could not store settings on this puck.";
    g_pass_echo = passEcho;
    g_draft = next;
    renderForm();
    sendHtml();
    return;
  }
  renderMessage("Saved. This puck is restarting to join your Wi-Fi. Connect this phone back to that network.");
  sendHtml();
  restartSoon();
}

static void handleCancel() {
  if (g_restart_at) return;
  if (!settings().configured()) {
    handleRoot();
    return;
  }
  renderMessage("Leaving setup. Saved settings stay as they are.");
  sendHtml();
  restartSoon();
}

static void handleEraseGet() {
  if (g_restart_at) return;
  renderErase();
  sendHtml();
}

static void handleErasePost() {
  if (g_restart_at) return;
  if (!settingsErase()) {
    renderMessage("Could not erase settings on this puck.");
    sendHtml();
    return;
  }
  renderMessage("Settings erased. This puck is restarting into setup.");
  sendHtml();
  restartSoon();
}

static void handleNotFound() {
  if (g_restart_at) return;
  if (g_server.uri() == "/favicon.ico") {
    g_last_http = millis();
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(204);
    return;
  }
  if (g_server.method() == HTTP_GET) {
    handleRoot();
    return;
  }
  g_server.send(404, "text/plain", "Not found");
}

static void portalTask(void *);

const char *setupPortalUrl() { return kUrl; }

const char *setupPortalStart() {
  g_up_at = g_last_http = millis();
  WiFi.mode(WIFI_AP_STA);
  esp_wifi_disconnect();
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  settingsRadioName(g_ap_ssid, sizeof(g_ap_ssid), false);
  WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(g_ap_ssid)) {
    delay(200);
    if (!WiFi.softAP(g_ap_ssid)) Serial.println("[setup] access point failed");
  }
  g_dns.start(53, "*", kApIp);
  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/save", HTTP_POST, handleSave);
  g_server.on("/cancel", HTTP_POST, handleCancel);
  g_server.on("/erase", HTTP_GET, handleEraseGet);
  g_server.on("/erase", HTTP_POST, handleErasePost);
  g_server.onNotFound(handleNotFound);
  g_server.begin();
  Serial.printf("[setup] join %s (open)  http://%s\n", g_ap_ssid, kUrl);
  g_task = xTaskCreate(portalTask, "setup", 16384, nullptr, 1, nullptr) == pdPASS;
  if (!g_task) Serial.println("[setup] portal task failed");
  return g_ap_ssid;
}

static void servicePortal() {
  if (g_restart_at && (int32_t)(millis() - g_restart_at) >= 0) ESP.restart();
  if (settings().configured() && !g_restart_at && millis() - g_last_http > kGiveUpMs) ESP.restart();
  if (!g_scan_started && !g_scan_done && (int32_t)(millis() - g_up_at) > 400) startScan();
  pollScan();
  g_dns.processNextRequest();
  g_server.handleClient();
}

static void portalTask(void *) {
  for (;;) {
    servicePortal();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void setupPortalLoop() {
  if (g_task) {
    delay(50);
    return;
  }
  servicePortal();
  delay(2);
}
