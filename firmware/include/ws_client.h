#pragma once

#include "buddy_protocol.h"
#include <vector>

using SnapshotHandler = void (*)(const std::vector<BuddySession> &sessions, bool muted);
using ConnHandler = void (*)(bool connected);

void buddyWsBegin(SnapshotHandler onSnapshot, ConnHandler onConn);
void buddyWsLoop();
void buddyWsSendAck(const String &session_id, const String &action);
bool buddyWsConnected();
