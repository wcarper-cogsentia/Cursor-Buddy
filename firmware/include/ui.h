#pragma once

#include "buddy_protocol.h"
#include <vector>

void uiBegin();
void uiLoop();
void uiSetOffline(bool offline);
void uiApplySnapshot(const std::vector<BuddySession> &sessions, bool muted);
// Returns true if user requested an action this frame
bool uiPollAck(String &session_id, String &action);
