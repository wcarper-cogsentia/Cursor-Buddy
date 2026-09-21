#pragma once

#include "buddy_protocol.h"
#include <vector>

void uiBegin();
// Release UART0 (GPIO43/44) and arm the four header switches. Call after boot logs.
void uiArmSwitches();
void uiLoop();
// Milliseconds since the last screen touch or button press.
uint32_t uiIdleMs();
// Backlight off, panel sleep, RGB DMA stopped. False leaves the UI running.
bool uiQuiesceForSleep();
void uiRestoreAfterSleep();
void uiSetOffline(bool offline);
void uiApplySnapshot(const std::vector<BuddySession> &sessions, bool muted, const String &focused_id);
// Returns true if user requested an action this frame
bool uiPollAck(String &session_id, String &action);
