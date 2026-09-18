#!/usr/bin/env bash
# Cut a GitHub release the firmware's OTA check can install.
#
#   ./scripts/release.sh 0.0.2
#
# The tag must match FIRMWARE_VERSION in src/version.h, because that string is
# what the running device compares against the latest release tag. A mismatch
# means every device re-downloads on every check and never settles.
#
# The size gate is the point of this script. A build that overflows the OTA slot
# uploads fine over USB and then fails silently over the air -- an artifact no
# device can install. Checking here is the only place that failure is cheap.

set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
    echo "usage: $0 <version>   e.g. $0 0.0.2" >&2
    exit 1
fi
VERSION="${VERSION#v}"

PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
ENV_NAME="esp32c3"
BIN=".pio/build/${ENV_NAME}/firmware.bin"

# Every OTA slot in min_spiffs.csv. Keep in step with board_build.partitions.
SLOT_BYTES=1966080

# --- version must match what the firmware will report -----------------------
SRC_VERSION="$(sed -n 's/^#define FIRMWARE_VERSION "\(.*\)"/\1/p' src/version.h)"
if [[ "$SRC_VERSION" != "$VERSION" ]]; then
    echo "src/version.h says $SRC_VERSION, you asked for $VERSION." >&2
    echo "Update FIRMWARE_VERSION first so the device can tell it is up to date." >&2
    exit 1
fi

# --- working tree must be clean ---------------------------------------------
if [[ -n "$(git status --porcelain)" ]]; then
    echo "working tree is dirty; commit before releasing." >&2
    exit 1
fi

# --- build the release env, not whatever was built last ---------------------
echo "==> building $ENV_NAME"
"$PIO" run -e "$ENV_NAME"

SIZE="$(stat -f%z "$BIN" 2>/dev/null || stat -c%s "$BIN")"
SPARE=$(( SLOT_BYTES - SIZE ))
echo "==> firmware.bin is $SIZE bytes, OTA slot is $SLOT_BYTES ($SPARE spare)"
if (( SPARE < 0 )); then
    echo "TOO LARGE for the OTA slot by $(( -SPARE )) bytes. No device could install this." >&2
    exit 1
fi
if (( SPARE < 65536 )); then
    echo "WARNING: under 64 KB of headroom. Trim before the next feature lands." >&2
fi

# --- tag and publish --------------------------------------------------------
echo "==> tagging v$VERSION"
git tag -a "v$VERSION" -m "Glow Lamp v$VERSION"
git push origin "v$VERSION"

echo "==> creating GitHub release"
# The asset MUST be named firmware.bin: the device downloads
# releases/latest/download/firmware.bin by that exact name.
gh release create "v$VERSION" "$BIN" \
    --title "v$VERSION" \
    --notes "Glow Lamp firmware v$VERSION ($SIZE bytes, $SPARE spare in the OTA slot)"

echo "==> done. Devices pick this up on their next check."
