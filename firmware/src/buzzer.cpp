#include <Arduino.h>
#include "buzzer.h"
#include "board.h"
#include "exio.h"

static bool g_muted = false;

void buzzerBegin() {
  exioSet(EXIO_BUZZER, false);
}

void buzzerSetMuted(bool muted) { g_muted = muted; }

bool buzzerIsMuted() { return g_muted; }

static void pulse(uint32_t on_ms, uint32_t off_ms = 0) {
  if (g_muted) return;
  exioSet(EXIO_BUZZER, true);
  delay(on_ms);
  exioSet(EXIO_BUZZER, false);
  if (off_ms) delay(off_ms);
}

void buzzerAttention() {
  pulse(80, 60);
  pulse(80);
}

void buzzerComplete() {
  pulse(90, 40);
  pulse(90, 40);
  pulse(140);
}

void buzzerError() {
  pulse(250, 40);
  pulse(300);
}
