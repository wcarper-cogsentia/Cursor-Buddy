#pragma once

bool st7701Init();
// Panel power-down. Safe to call after init; the RGB bus is left alone.
void st7701SleepIn();
