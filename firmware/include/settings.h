#pragma once

#include <Arduino.h>

// Network, service, and clock settings stored on the puck.
// config.h is copied in once when this storage is empty, then left alone.

enum class BuddyTempUnit : uint8_t { Auto = 0, Fahrenheit = 1, Celsius = 2 };

struct BuddySettings {
  String ssid;
  String password;
  String host;
  uint16_t port = 8787;
  String path;
  String name;
  String tz;
  bool hasLocation = false;
  float latitude = 0;
  float longitude = 0;
  BuddyTempUnit tempUnit = BuddyTempUnit::Auto;
  bool clock24h = false;
  uint32_t batteryIdleMs = 30UL * 60UL * 1000UL;

  bool configured() const { return ssid.length() > 0 && host.length() > 0; }
};

void settingsBegin();
const BuddySettings &settings();
bool settingsSave(const BuddySettings &next);
// Drops the saved network and service host. The config.h copy is not imported again.
bool settingsErase();
// Next boot opens the setup page, including when a network is already saved.
bool settingsRequestSetup();
bool settingsTakeSetupRequest();
// "Buddy-A1B2", or "buddy-a1b2" when lower is set. Call after WiFi.mode.
void settingsRadioName(char *out, size_t n, bool lower);
