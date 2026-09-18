#ifndef OTA_H
#define OTA_H

#include <Arduino.h>

void loopOta();

// Queued rather than run inline: the web handler that calls this is inside the
// server's request path, and flashing from there reboots the device with the
// browser's socket mid-response. loopOta() picks it up on the next pass.
void requestOtaCheck();

#endif  // OTA_H
