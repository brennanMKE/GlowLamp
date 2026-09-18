#!/usr/bin/env bash
# Regenerate src/favicon.h from FavIcon.png.
#
#   ./scripts/make_favicon.sh [source.png]
#
# The icon is compiled into the firmware rather than served from a filesystem,
# so there is nothing to upload alongside a build and nothing to go missing
# after an OTA. The cost is that changing the image means regenerating a header,
# which is what this does.

set -euo pipefail
cd "$(dirname "$0")/.."

SRC="${1:-FavIcon.png}"
[[ -f "$SRC" ]] || { echo "no such file: $SRC" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# 32 for the browser tab, 180 for an iOS home screen. sips ships with macOS;
# on Linux use `convert -resize` from ImageMagick instead.
sips -s format png -Z 32 "$SRC" --out "$TMP/fav32.png" >/dev/null
sips -s format png -Z 180 "$SRC" --out "$TMP/fav180.png" >/dev/null

python3 - "$TMP/fav32.png" "$TMP/fav180.png" <<'PY'
import sys

def carray(path, name):
    data = open(path, 'rb').read()
    rows = ["    " + " ".join(f"0x{b:02x}," for b in data[i:i + 16])
            for i in range(0, len(data), 16)]
    return len(data), f"static const uint8_t {name}[] PROGMEM = {{\n" + "\n".join(rows) + "\n};\n"

n32, a32 = carray(sys.argv[1], "FAVICON_32_PNG")
n180, a180 = carray(sys.argv[2], "FAVICON_180_PNG")

header = '''#ifndef FAVICON_H
#define FAVICON_H

// The lamp's icon, baked into the firmware.
//
// Generated from FavIcon.png in the project root by scripts/make_favicon.sh --
// edit that image and re-run the script rather than editing the bytes here.
//
// Two sizes, and no more: 32x32 for the browser tab, and 180x180 for iOS when
// someone adds the lamp to a home screen, which is the likeliest way anyone
// reaches this UI twice. Both are PNG; every browser that matters has taken
// PNG favicons for a decade, and an .ico would cost more bytes for the same
// pixels. Served from flash with no filesystem involved, so there is nothing
// to upload alongside the firmware and nothing to go missing after an OTA.

#include <Arduino.h>

'''
header += f"// {n32} bytes\n" + a32 + "\n"
header += f"// {n180} bytes\n" + a180 + "\n"
header += "#endif  // FAVICON_H\n"
open("src/favicon.h", "w").write(header)
print(f"src/favicon.h: 32px {n32} B, 180px {n180} B, {n32 + n180} B total")
PY
