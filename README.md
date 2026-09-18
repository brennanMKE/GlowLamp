# Glow Lamp

A ring of 8 addressable LEDs that slowly blends between three vibrant colors,
driven by an ESP32-C3. WiFi is provisioned through a captive portal, and the
firmware updates itself from GitHub releases.

Version 0.0.1 is deliberately minimal: the ring is one light, not eight
pixels — every LED always shows the same color — and the only thing it does is
cycle. Everything beyond that is for later versions.

## Hardware

| Part | Detail |
|---|---|
| Board | ESP32-C3 (`esp32-c3-devkitm-1`) |
| Ring | WS2812B, 8 LEDs |
| Data pin | GPIO 4 |
| Power | 5 V, capped in firmware at 500 mA |

Wire the ring's data line to GPIO 4 and share ground between the ring's supply
and the ESP32-C3. At the default brightness of 64 the 8 LEDs are well inside the
500 mA cap, so USB power is fine; run it near 255 and you will want a separate
5 V supply.

## Behavior

The ring holds no state and needs no network to light up. It walks the palette
in order, easing in and out of each color so it lingers on the pure colors
rather than sweeping past them into the muddy midpoint. One full cycle is
`BLEND_MS × 3` — 18 seconds at the default.

### Palette

Saturated and far apart in hue on purpose: WS2812B washes pastels out to
near-white, and two neighboring hues read as one slowly shifting color instead
of as a cycle.

| Color | RGB |
|---|---|
| Magenta-red | `(255, 0, 40)` |
| Azure | `(0, 120, 255)` |
| Spring green | `(0, 255, 90)` |

## Tuning

The constants at the top of `src/main.cpp`:

| Constant | Default | Meaning |
|---|---|---|
| `DATA_PIN` | 4 | GPIO driving the ring |
| `NUM_LEDS` | 8 | LEDs in the ring |
| `PALETTE` | 3 colors | What it blends between; add a fourth and the cycle lengthens on its own |
| `BLEND_MS` | 6000 | Milliseconds to cross from one color to the next |
| `FRAME_MS` | 16 | ~60 fps |
| `DEFAULT_BRIGHTNESS` | 64 | Until the Settings page saves one |

Brightness is the one value that does not need a reflash — it lives in NVS and
is set from the Settings page.

## WiFi setup

WiFi is handled by [EasyWiFi](https://github.com/brennanMKE/EasyWiFi).
Credentials are never compiled into the firmware, so a release binary is safe to
publish and any board can be re-provisioned in the field.

1. Power on an unprovisioned board. It raises an access point named
   `glow-lamp-Setup-XXXXXX`.
2. Join it; the captive portal opens.
3. Pick the network, enter the password, save. The lamp reboots and joins.

Once joined it announces `_glowlamp._tcp` over mDNS:

```sh
./scripts/find_devices.sh
```

That prints the `.local` address and a direct link to the Settings page. The web
UI lives at:

| Path | What |
|---|---|
| `/` | Status: current color, brightness, network, firmware version |
| `/settings` | Hostname, brightness, update check, reboot |
| `/wifi` | EasyWiFi's own setup pages |

## Building

```sh
pio run                      # release, the build that ships
pio run -e esp32c3-debug     # verbose logging, USB only
pio run -t upload            # flash over USB
pio device monitor           # 115200
```

Copy `lib/Config/Config.h.sample` to `lib/Config/Config.h` before the first
build — it is gitignored and holds the device name and the OTA repo. There are
no secrets in it.

## OTA updates

The lamp checks `https://github.com/<OTA_GITHUB_REPO>/releases/latest` for a
release tag that differs from its own `FIRMWARE_VERSION`, downloads
`firmware.bin` from it and reboots. Checks happen 15 seconds after joining WiFi,
once a day at 15:00 local, and whenever the Settings page asks.

Comparison is by tag equality, not ordering, so re-tagging an older release is a
supported way to roll back.

To cut a release:

```sh
# bump FIRMWARE_VERSION in src/version.h first, and commit
./scripts/release.sh 0.0.2
```

The script refuses to tag if the version does not match `src/version.h`, if the
working tree is dirty, or if the binary would overflow the 1,966,080-byte OTA
slot — a build that overflows uploads fine over USB and then fails silently over
the air.

Until the GitHub repo has its first release the check logs an HTTP 404 and
carries on. Nothing else changes.

## License

MIT. See [LICENSE](LICENSE).
