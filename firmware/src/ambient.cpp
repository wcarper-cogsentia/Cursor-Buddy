#include "ambient.h"
#include "settings.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>
#include <time.h>

static const uint32_t kWeatherEveryMs = 15UL * 60UL * 1000UL;
static const uint32_t kWeatherRetryMs = 20UL * 1000UL;
static const uint32_t kGeoEveryMs = 6UL * 60UL * 60UL * 1000UL;
static const uint32_t kGeoRetryMs = 45UL * 1000UL;
static const uint32_t kTaskStack = 24576;

static SemaphoreHandle_t g_mu = nullptr;
static volatile bool g_paused = false;
static bool g_holding = false;

static bool g_clock_started = false;
static bool g_offset_set = false;
static int g_offset = 0;
static uint32_t g_next_geo = 0;
static uint32_t g_next_weather = 0;

static bool g_geo_ok = false;
static float g_lat = 0;
static float g_lon = 0;
static char g_cc[4] = "";

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_weather_valid = false;
static bool g_weather_failed = false;
static int g_temp = 0;
static int g_code = -1;
static bool g_has_range = false;
static int g_lo = 0;
static int g_hi = 0;
static char g_unit = 'F';
static char g_summary[24] = "";

static bool due(uint32_t at) { return (int32_t)(millis() - at) >= 0; }

static bool useFahrenheit() {
  BuddyTempUnit unit = settings().tempUnit;
  if (unit == BuddyTempUnit::Celsius) return false;
  if (unit == BuddyTempUnit::Fahrenheit) return true;
  return g_cc[0] == '\0' || strcmp(g_cc, "US") == 0;
}

static bool fixedLocation(float &lat, float &lon) {
  const BuddySettings &saved = settings();
  if (!saved.hasLocation) return false;
  lat = saved.latitude;
  lon = saved.longitude;
  return true;
}

static bool coords(float &lat, float &lon) {
  if (fixedLocation(lat, lon)) return true;
  if (!g_geo_ok) return false;
  lat = g_lat;
  lon = g_lon;
  return true;
}

static const char *weatherText(int code) {
  switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45:
    case 48: return "Fog";
    case 51:
    case 53:
    case 55: return "Drizzle";
    case 56:
    case 57: return "Freezing drizzle";
    case 61:
    case 63:
    case 65: return "Rain";
    case 66:
    case 67: return "Freezing rain";
    case 71:
    case 73:
    case 75:
    case 77: return "Snow";
    case 80:
    case 81:
    case 82: return "Showers";
    case 85:
    case 86: return "Snow showers";
    case 95:
    case 96:
    case 99: return "Thunderstorm";
    default: return "";
  }
}

static int roundTemp(float raw) { return (int)(raw >= 0 ? raw + 0.5f : raw - 0.5f); }

static void publishWeather(bool ok, int temp, char unit, int code, const char *summary, bool hasRange, int lo, int hi) {
  portENTER_CRITICAL(&g_mux);
  if (ok) {
    g_temp = temp;
    g_unit = unit;
    g_code = code;
    g_has_range = hasRange;
    g_lo = lo;
    g_hi = hi;
    strncpy(g_summary, summary ? summary : "", sizeof(g_summary) - 1);
    g_summary[sizeof(g_summary) - 1] = '\0';
    g_weather_valid = true;
    g_weather_failed = false;
  } else if (!g_weather_valid) {
    g_weather_failed = true;
  }
  portEXIT_CRITICAL(&g_mux);
}

static void applyOffset(int offsetSec) {
  if (settings().tz.length()) return;
  if (g_offset_set && g_offset == offsetSec) return;
  configTime(offsetSec, 0, "pool.ntp.org", "time.nist.gov");
  g_offset = offsetSec;
  g_offset_set = true;
  g_clock_started = true;
  Serial.printf("[ambient] utc offset %d\n", offsetSec);
}

static void ensureClock() {
  const String &tz = settings().tz;
  if (!tz.length() || g_clock_started) return;
  configTzTime(tz.c_str(), "pool.ntp.org", "time.nist.gov");
  g_clock_started = true;
  Serial.printf("[ambient] timezone %s\n", tz.c_str());
}

// Open-Meteo is public forecast data. The root bundle is not enabled in this
// firmware build, so the TLS check is skipped rather than failing closed.
static bool httpGet(const char *label, const char *url, String &payload) {
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  http.setTimeout(8000);
  bool begun;
  if (strncmp(url, "https://", 8) == 0) {
    secure.setInsecure();
    begun = http.begin(secure, url);
  } else {
    begun = http.begin(plain, url);
  }
  if (!begun) {
    Serial.printf("[ambient] %s begin failed\n", label);
    return false;
  }
  http.addHeader("User-Agent", "cursor-buddy");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ambient] %s HTTP %d\n", label, code);
    http.end();
    return false;
  }
  payload = http.getString();
  http.end();
  if (payload.isEmpty() || payload.length() > 3072) {
    Serial.printf("[ambient] %s bad body\n", label);
    return false;
  }
  return true;
}

static bool fetchGeo() {
  String body;
  if (!httpGet("geo", "http://ip-api.com/json/?fields=status,lat,lon,offset,countryCode", body)) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const char *status = doc["status"] | "";
  if (strcmp(status, "success") != 0) return false;
  g_lat = doc["lat"] | 0.f;
  g_lon = doc["lon"] | 0.f;
  const char *cc = doc["countryCode"] | "";
  strncpy(g_cc, cc, sizeof(g_cc) - 1);
  g_cc[sizeof(g_cc) - 1] = '\0';
  g_geo_ok = true;
  if (!doc["offset"].isNull()) applyOffset(doc["offset"].as<int>());
  Serial.printf("[ambient] location %.2f, %.2f\n", g_lat, g_lon);
  return true;
}

static bool fetchWeather() {
  float lat = 0, lon = 0;
  if (!coords(lat, lon)) return false;
  char url[256];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code"
           "&daily=temperature_2m_max,temperature_2m_min&forecast_days=1"
           "&temperature_unit=%s&timezone=auto",
           lat, lon, useFahrenheit() ? "fahrenheit" : "celsius");
  String body;
  if (!httpGet("weather", url, body)) {
    publishWeather(false, 0, 'F', -1, "", false, 0, 0);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    publishWeather(false, 0, 'F', -1, "", false, 0, 0);
    return false;
  }
  if (!doc["utc_offset_seconds"].isNull()) applyOffset(doc["utc_offset_seconds"].as<int>());

  JsonObject current = doc["current"];
  if (current.isNull() || current["temperature_2m"].isNull() || current["weather_code"].isNull()) {
    publishWeather(false, 0, 'F', -1, "", false, 0, 0);
    return false;
  }
  int temp = roundTemp(current["temperature_2m"].as<float>());
  int code = current["weather_code"].as<int>();
  char unit = useFahrenheit() ? 'F' : 'C';
  bool hasRange = false;
  int lo = 0, hi = 0;
  JsonVariant loV = doc["daily"]["temperature_2m_min"][0];
  JsonVariant hiV = doc["daily"]["temperature_2m_max"][0];
  if (!loV.isNull() && !hiV.isNull()) {
    lo = roundTemp(loV.as<float>());
    hi = roundTemp(hiV.as<float>());
    hasRange = true;
  }
  const char *summary = weatherText(code);
  publishWeather(true, temp, unit, code, summary, hasRange, lo, hi);
  if (hasRange) Serial.printf("[ambient] %d %c %s %d/%d\n", temp, unit, summary, lo, hi);
  else Serial.printf("[ambient] %d %c %s\n", temp, unit, summary);
  return true;
}

static void service() {
  ensureClock();
  float lat, lon;
  if (!fixedLocation(lat, lon) && due(g_next_geo)) {
    if (fetchGeo()) g_next_geo = millis() + kGeoEveryMs;
    else g_next_geo = millis() + kGeoRetryMs;
  }
  if (!coords(lat, lon) || !due(g_next_weather)) return;
  if (fetchWeather()) g_next_weather = millis() + kWeatherEveryMs;
  else g_next_weather = millis() + (g_weather_valid ? 120UL * 1000UL : kWeatherRetryMs);
}

static void ambientTask(void *) {
  for (;;) {
    if (g_mu && xSemaphoreTake(g_mu, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (!g_paused && WiFi.status() == WL_CONNECTED) service();
      xSemaphoreGive(g_mu);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void ambientBegin() {
  if (g_mu) return;
  g_mu = xSemaphoreCreateMutex();
  if (!g_mu) {
    Serial.println("[ambient] mutex failed");
    return;
  }
  if (xTaskCreate(ambientTask, "ambient", kTaskStack, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("[ambient] task failed");
  }
}

bool ambientQuiesce() {
  g_paused = true;
  if (!g_mu) return true;
  if (xSemaphoreTake(g_mu, 0) != pdTRUE) {
    g_paused = false;
    return false;
  }
  g_holding = true;
  return true;
}

void ambientResume() {
  g_next_weather = millis();
  g_paused = false;
  if (g_holding && g_mu) {
    g_holding = false;
    xSemaphoreGive(g_mu);
  }
}

void ambientRead(AmbientClock &out) {
  out = {};
  time_t now = time(nullptr);
  struct tm tm{};
  localtime_r(&now, &tm);
  if (tm.tm_year > (2016 - 1900)) {
    out.valid = true;
    out.hour = tm.tm_hour;
    out.minute = tm.tm_min;
    out.second = tm.tm_sec;
    out.wday = tm.tm_wday;
    out.mon = tm.tm_mon;
    out.mday = tm.tm_mday;
  }
  portENTER_CRITICAL(&g_mux);
  out.weather_valid = g_weather_valid;
  out.weather_failed = g_weather_failed;
  out.temp = g_temp;
  out.code = g_code;
  out.has_range = g_has_range;
  out.temp_lo = g_lo;
  out.temp_hi = g_hi;
  out.unit = g_unit;
  memcpy(out.summary, g_summary, sizeof(out.summary));
  portEXIT_CRITICAL(&g_mux);
}
