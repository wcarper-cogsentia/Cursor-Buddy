#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>
#include "power.h"
#include "board.h"

// No VBUS pin on this board. USB keeps the pack from falling; battery-only
// operation shows up as a real drop on the GPIO4 divider.
static const float kMinPackMv = 3000.f;
static const float kMaxPackMv = 4600.f;
static const float kSlopeDropMv = 30.f;
static const uint32_t kSamplePeriodMs = 60UL * 1000UL;
static const uint32_t kSlopeMinAgeMs = 8UL * 60UL * 1000UL;
static const uint32_t kSlopeMaxAgeMs = 14UL * 60UL * 1000UL;

static const int kHist = 20;
static float g_mv[kHist];
static uint32_t g_at[kHist];
static int g_count = 0;
static int g_next = 0;
static uint32_t g_last_sample = 0;

static const gpio_num_t kWakePins[] = {
    GPIO_NUM_16,  // CST820 INT, active low
    GPIO_NUM_19,  // mute
    GPIO_NUM_20,  // dismiss
    GPIO_NUM_43,  // previous (not an RTC pin; light sleep only)
    GPIO_NUM_44,  // next
};

static float readPackMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BAT_ADC_PIN);
  return (sum / 16.0f) * 3.0f;
}

void powerBegin() {
  pinMode(BAT_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  g_last_sample = millis() - kSamplePeriodMs;
}

void powerSample() {
  uint32_t now = millis();
  if (g_count && now - g_last_sample < kSamplePeriodMs) return;
  g_last_sample = now;
  float mv = readPackMv();
  g_mv[g_next] = mv;
  g_at[g_next] = now;
  g_next = (g_next + 1) % kHist;
  if (g_count < kHist) g_count++;
}

bool powerIsDischarging() {
  if (g_count < 10) return false;
  int newest = (g_next + kHist - 1) % kHist;
  float nowMv = g_mv[newest];
  if (nowMv < kMinPackMv || nowMv > kMaxPackMv) return false;

  float thenMv = -1.f;
  uint32_t bestDt = UINT32_MAX;
  for (int i = 0; i < g_count; i++) {
    int idx = (newest + kHist - i) % kHist;
    uint32_t age = g_at[newest] - g_at[idx];
    if (age < kSlopeMinAgeMs || age > kSlopeMaxAgeMs) continue;
    uint32_t target = 10UL * 60UL * 1000UL;
    uint32_t dt = age > target ? age - target : target - age;
    if (dt < bestDt) {
      bestDt = dt;
      thenMv = g_mv[idx];
    }
  }
  return thenMv > 0.f && thenMv - nowMv >= kSlopeDropMv;
}

static bool wakePinsReleased() {
  for (gpio_num_t pin : kWakePins) {
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_pullup_en(pin);
    gpio_pulldown_dis(pin);
    if (gpio_get_level(pin) == 0) return false;
  }
  return true;
}

bool powerLightSleep() {
  if (!wakePinsReleased()) return false;

  for (gpio_num_t pin : kWakePins) {
    gpio_wakeup_disable(pin);
    if (gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL) != ESP_OK) return false;
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (esp_sleep_enable_gpio_wakeup() != ESP_OK) return false;
  // Hold RTC pull-ups on the touch and mute/dismiss pins.
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  bool wdt = esp_task_wdt_delete(nullptr) == ESP_OK;
  esp_err_t err = esp_light_sleep_start();
  if (wdt) esp_task_wdt_add(nullptr);

  for (gpio_num_t pin : kWakePins) gpio_wakeup_disable(pin);
  return err == ESP_OK;
}
