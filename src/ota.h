#ifndef OTA_H
#define OTA_H

#include <Arduino.h>

void loopOta();

// Queued rather than run inline: both are reached from a web request handler,
// and doing this work there is wrong twice over. The install reboots the device
// with the browser's socket mid-response; even the check blocks for a second or
// two of TLS handshake. loopOta() picks these up on the next pass.
//
// The two are deliberately separate. A check only asks GitHub what the latest
// release is and reports back -- it never flashes anything. Installing is an
// explicit second step, so someone looking at the status page can see what is
// available before deciding to take it.
void requestOtaCheck();
void requestOtaInstall();

// Check, and install only if the latest release differs from what is running.
// The same thing the nightly timer does, exposed so Home Assistant can drive
// the schedule instead -- a check that installs nothing when there is nothing
// new is safe to fire at every lamp every night.
void requestOtaUpdate();

// ===== What the status page reads =====

// "idle", "checking", "installing" or "error".
const char *otaState();

// The tag of the latest GitHub release, with any leading "v" stripped. Empty
// until a check has succeeded.
String otaLatestTag();

// True when the latest known release differs from the running firmware. Tag
// equality, not ordering: re-tagging an older release is a supported rollback,
// and this reports that as an available update too.
bool otaUpdateAvailable();

// Why the last check or install failed. Empty when the last one succeeded.
String otaLastError();

// Seconds since the last completed check, or -1 if none has finished this boot.
int32_t otaSecondsSinceCheck();

#endif  // OTA_H
