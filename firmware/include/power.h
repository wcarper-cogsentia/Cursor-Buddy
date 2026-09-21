#pragma once

#include <Arduino.h>

// Idle time with no touch or button before battery sleep. Override in config.h.
#ifndef BUDDY_BATTERY_IDLE_MS
#define BUDDY_BATTERY_IDLE_MS (30UL * 60UL * 1000UL)
#endif

void powerBegin();
// Call from loop. Samples pack voltage about once a minute.
void powerSample();
// True when a Li-ion pack is present and its voltage has been falling,
// which is how this board shows it is running on battery rather than USB.
bool powerIsDischarging();
// Light sleep until the screen or any header button is touched.
// False when a wake pin is already active, so sleep would return immediately.
bool powerLightSleep();
