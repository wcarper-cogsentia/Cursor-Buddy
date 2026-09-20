#include <Arduino.h>
#include "buzzer.h"

// Waveshare ESP32-S3-Touch-LCD-2.1 buzzer (active/passive per board rev)
#ifndef BUDDY_BUZZER_PIN
#define BUDDY_BUZZER_PIN 42
#endif

static bool g_muted = false;

void buzzerBegin() {
  pinMode(BUDDY_BUZZER_PIN, OUTPUT);
  digitalWrite(BUDDY_BUZZER_PIN, LOW);
}

void buzzerSetMuted(bool muted) { g_muted = muted; }

bool buzzerIsMuted() { return g_muted; }

static void beep(int freq, int ms) {
  if (g_muted) return;
  tone(BUDDY_BUZZER_PIN, freq, ms);
  delay(ms + 20);
  noTone(BUDDY_BUZZER_PIN);
}

void buzzerAttention() {
  // Short double-beep
  beep(1800, 80);
  delay(60);
  beep(1800, 80);
}

void buzzerComplete() {
  // Ascending
  beep(880, 90);
  beep(1175, 90);
  beep(1568, 120);
}

void buzzerError() {
  // Lower distinctive tone
  beep(220, 250);
  delay(40);
  beep(180, 300);
}
