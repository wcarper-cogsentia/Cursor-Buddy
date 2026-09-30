#pragma once

#include <Arduino.h>

void powerBegin();
// Call from loop. Samples pack voltage about once a minute.
void powerSample();
// True when a Li-ion pack is present and its voltage has been falling,
// which is how this board shows it is running on battery rather than USB.
bool powerIsDischarging();
// 0..4 segments from the latest pack voltage. -1 before the first valid sample.
int powerBatteryBars();
// True when the pack is rising or held at the charger voltage.
bool powerIsCharging();
// Light sleep until the screen or any header button is touched.
// False when a wake pin is already active, so sleep would return immediately.
bool powerLightSleep();
