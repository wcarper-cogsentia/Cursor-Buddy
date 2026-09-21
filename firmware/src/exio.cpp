#include <Wire.h>
#include "exio.h"
#include "board.h"

static const uint8_t REG_OUTPUT = 0x01;
static const uint8_t REG_CONFIG = 0x03;
static bool g_ready = false;
static uint8_t g_output = 0x7F;  // EXIO8 (buzzer) low, other outputs high

static bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool exioBegin() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  delay(20);
  if (!writeReg(REG_CONFIG, 0x00)) {
    Serial.println("[exio] TCA9554 not found");
    g_ready = false;
    return false;
  }
  if (!writeReg(REG_OUTPUT, g_output)) {
    Serial.println("[exio] output write failed");
    g_ready = false;
    return false;
  }
  g_ready = true;
  Serial.println("[exio] TCA9554 ready");
  return true;
}

bool exioReady() { return g_ready; }

void exioSet(uint8_t pin1to8, bool high) {
  if (pin1to8 < 1 || pin1to8 > 8) return;
  uint8_t mask = static_cast<uint8_t>(1u << (pin1to8 - 1));
  if (high) {
    g_output |= mask;
  } else {
    g_output = static_cast<uint8_t>(g_output & ~mask);
  }
  if (g_ready) {
    writeReg(REG_OUTPUT, g_output);
  }
}
