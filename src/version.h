#ifndef VERSION_H
#define VERSION_H

// Bumped by scripts/release.sh, which also tags the repo. The GitHub OTA check
// compares this against the latest release tag, so a build whose version does
// not match a real tag will re-download on every check.
#define FIRMWARE_VERSION "0.0.3"

#endif  // VERSION_H
