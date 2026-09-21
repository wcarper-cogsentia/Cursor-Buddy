#pragma once

// Local clock and weather for the idle face. Time is synced over NTP.
// Weather comes from Open-Meteo. With no lat/long in config.h, the location
// is the network's public IP.

struct AmbientClock {
  bool valid;
  int hour;
  int minute;
  int second;
  int wday;
  int mon;
  int mday;
  bool weather_valid;
  bool weather_failed;
  int temp;
  int code;
  bool has_range;
  int temp_lo;
  int temp_hi;
  char unit;
  char summary[24];
};

void ambientBegin();
// Take the network away from an in-flight lookup. False: a fetch is running.
bool ambientQuiesce();
void ambientResume();
void ambientRead(AmbientClock &out);
