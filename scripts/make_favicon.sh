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

import struct

def carray_bytes(data, name):
    rows = ["    " + " ".join(f"0x{b:02x}," for b in data[i:i + 16])
            for i in range(0, len(data), 16)]
    return len(data), f"static const uint8_t {name}[] PROGMEM = {{\n" + "\n".join(rows) + "\n};\n"

def carray(path, name):
    return carray_bytes(open(path, 'rb').read(), name)

def ico_wrap(png_path, side):
    """A one-image .ico containing the PNG as-is.

    An .ico may hold PNG data directly rather than a BMP, which every browser
    still in use understands, and it keeps this to a 22-byte header instead of
    a second copy of the image in another format.
    """
    png = open(png_path, 'rb').read()
    header = struct.pack('<HHH', 0, 1, 1)                       # reserved, type=icon, count
    entry = struct.pack('<BBBBHHII',
                        side if side < 256 else 0,              # width, 0 means 256
                        side if side < 256 else 0,              # height
                        0, 0,                                   # palette, reserved
                        1, 32,                                  # planes, bits per pixel
                        len(png), 22)                           # size, offset past the header
    return header + entry + png

n32, a32 = carray(sys.argv[1], "FAVICON_32_PNG")
n180, a180 = carray(sys.argv[2], "FAVICON_180_PNG")
nico, aico = carray_bytes(ico_wrap(sys.argv[1], 32), "FAVICON_ICO")

header = '''#ifndef FAVICON_H
#define FAVICON_H

// The lamp's icon, baked into the firmware.
//
// Generated from FavIcon.png in the project root by scripts/make_favicon.sh --
// edit that image and re-run the script rather than editing the bytes here.
//
// Three forms: a 32x32 PNG for the tab, a 180x180 PNG for iOS when someone
// adds the lamp to a home screen, and the 32x32 wrapped in an .ico container.
//
// The .ico is not redundant. Safari asks for /favicon.ico on its own and wants
// an icon file there; PNG bytes under an .ico name are quietly ignored, which
// is what happened when this shipped without one. The wrapper is 22 bytes
// around the same PNG, not a second copy in another format.
//
// Served from flash with no filesystem involved, so there is nothing to upload
// alongside the firmware and nothing to go missing after an OTA.

#include <Arduino.h>

'''
header += f"// {n32} bytes\n" + a32 + "\n"
header += f"// {n180} bytes\n" + a180 + "\n"
header += f"// {nico} bytes -- the 32px PNG in an .ico container\n" + aico + "\n"
header += "#endif  // FAVICON_H\n"
open("src/favicon.h", "w").write(header)
print(f"src/favicon.h: 32px {n32} B, 180px {n180} B, ico {nico} B, "
      f"{n32 + n180 + nico} B total")
PY
