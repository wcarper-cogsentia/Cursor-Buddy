#pragma once

#include <Arduino.h>

enum class BuddyState : uint8_t {
  Idle = 0,
  Working,
  Attention,
  Complete,
  Error,
  Offline
};

struct BuddySession {
  String session_id;
  String project;
  BuddyState state = BuddyState::Idle;
  String message;
  int elapsed_seconds = 0;
  bool can_act = false;
};

static inline BuddyState parseState(const String &s) {
  if (s == "working") return BuddyState::Working;
  if (s == "attention") return BuddyState::Attention;
  if (s == "complete") return BuddyState::Complete;
  if (s == "error") return BuddyState::Error;
  if (s == "idle") return BuddyState::Idle;
  return BuddyState::Idle;
}

static inline const char *stateLabel(BuddyState st) {
  switch (st) {
    case BuddyState::Working: return "WORKING";
    case BuddyState::Attention: return "NEEDS ATTENTION";
    case BuddyState::Complete: return "COMPLETE";
    case BuddyState::Error: return "ERROR";
    case BuddyState::Offline: return "OFFLINE";
    default: return "IDLE";
  }
}
