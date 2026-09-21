#pragma once

// Open access point and the page at http://192.168.4.1.
// Call start once. Call loop from the main loop until the puck restarts.
const char *setupPortalStart();
const char *setupPortalUrl();
void setupPortalLoop();
