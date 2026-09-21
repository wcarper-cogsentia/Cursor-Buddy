#pragma once

#include <Arduino.h>

bool exioBegin();
void exioSet(uint8_t pin1to8, bool high);
bool exioReady();
