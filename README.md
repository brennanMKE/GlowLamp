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
| `/` | Status, and the controls: on/off, brightness, identify |
| `/effects` | The everyday page: effect, colors, presets |
| `/settings` | Firmware check and update, names, MQTT broker, reboot |
| `/logs` | Diagnostics: why the ring is dark, reset reason, heap, and the device log |
| `/help` | The REST API, documented by the lamp itself |
| `/api` | The same reference as JSON |
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

## Controlling a lamp

Everything the web UI does is a REST call, so a script can do the same:

```sh
curl http://glow-lamp-051860.local/api/status
curl -X POST http://glow-lamp-051860.local/api/power -d '{"on":false}'
curl -X POST http://glow-lamp-051860.local/api/brightness -d '{"value":128}'
curl -X POST http://glow-lamp-051860.local/api/identify -d '{"seconds":4}'
```

Values are accepted as a JSON body, a form field or a query parameter, and every
mutating call answers with the same object `GET /api/status` returns — the new
state never needs a second request. Anything that changes the lamp is POST only,
so nothing a browser can prefetch can switch a lamp off or reflash it. There is
no authentication: these are LAN devices, and anything that can reach one can
control it.

A JSON body is only read when the request sets
`Content-Type: application/json` — curl's default is form-encoded, and the HTTP
server discards a JSON document sent that way before the firmware sees it. Query
parameters never have that problem, which is why they lead here.

[docs/rest-api.md](docs/rest-api.md) is the full reference.
`http://<lamp>/help` is the same thing served by the lamp, so it always
describes the firmware actually running, and `/api` is that as JSON for an agent
that would rather not read HTML.

### Alerts

```sh
mosquitto_pub -h broker -t glowlamp/all/alert -m '{"seconds": 30}'
./scripts/glowlamp.py alert --all --seconds 30
curl -X POST 'http://castor-lamp.local/api/alert?seconds=30'
```

A red pulse to get attention, for Home Assistant to fire at something that needs
a person. It overrides the effect, the brightness and the power state, then puts
all three back, and always expires. See [docs/mqtt.md](docs/mqtt.md#subscribe-glowlamphostnamealert-and-glowlampallalert).

### Effects

```sh
curl -X POST 'http://glow-lamp-051860.local/api/effect?effect=neon&colors=ff0000,00ff00,0000ff'
curl -X POST http://glow-lamp-051860.local/api/effect/reset
```

| Effect | What it does |
|---|---|
| `blend` | The whole ring holds one color and eases to the next. The default. |
| `loop` | Walks the palette steadily, never resting on a color. |
| `flicker` | Similar colors mixing and guttering, like a flame. The one effect that lights the ring several colors at once. |
| `neon` | One color at full brightness, held steady, switching cleanly to the next. |

Up to 5 colors. An effect reverts to the default after 5 minutes unless you say
otherwise, is capped at 8 hours, and does not survive a reboot — see
[docs/rest-api.md](docs/rest-api.md#effects) for why that is the default.

### Identify

`POST /api/identify` blinks the ring white for a few seconds at a floor
brightness, so a lamp dimmed to 5 in a bright room still announces itself. It
overrides power and restores whatever was showing, so identifying a lamp that is
switched off leaves it switched off.

### Names

A lamp has two:

| | |
|---|---|
| **Name** | What you call it — "Living Room". Free text, announced over mDNS, takes effect immediately. Nothing is addressed by it. |
| **Hostname** | The mDNS label. Lowercase, digits and hyphens. The lamp answers at `<hostname>-<mac>.local`, and its setup AP is named from it. Needs a reboot. |

## Home Assistant

Set an MQTT broker on the lamp and it appears as a light entity by itself — with
brightness, a color picker and the effect list — with nothing added to
`configuration.yaml` and no restart:

```sh
./scripts/glowlamp.py broker 192.168.1.10 --host castor-lamp.local
```

See [docs/mqtt.md](docs/mqtt.md) for the topics and commands, and
[docs/home-assistant-control.md](docs/home-assistant-control.md) for why MQTT
rather than REST from Home Assistant. MQTT is optional: a lamp with no broker
never connects and is fully usable over REST.

`scripts/glowlamp.py` finds the lamps and drives them over REST, using only the
standard library so it runs under Home Assistant's Python:

```sh
./scripts/glowlamp.py discover
./scripts/glowlamp.py off --all
./scripts/glowlamp.py brightness 128 --name "Living Room"
./scripts/glowlamp.py identify --host glow-lamp-051860.local
```

See [docs/home-assistant.md](docs/home-assistant.md) for the REST switch and
`shell_command` configuration, including turning every lamp off with the rest of
the house.

[docs/home-assistant-control.md](docs/home-assistant-control.md) covers what
Home Assistant can and cannot do with a lamp, what each way in costs, and why
MQTT is the recommended route.

## Finding the lamps

```sh
./scripts/find_devices.sh            # scan and report
./scripts/find_devices.sh --check    # ask each lamp to check GitHub first
./scripts/find_devices.sh --update   # install on every lamp with an update
```

Each lamp announces `_glowlamp._tcp` over mDNS, so the scan finds them without
knowing an address. For each one it prints the URL, the resolved IP, the running
firmware, the latest release that lamp knows about, and whether an update is
waiting.

Names are resolved through the mDNS responder rather than left to curl. A cold
mDNS cache regularly takes longer to answer than curl's timeout, which made the
first scan after a reboot report every lamp as unreachable and the second one
work — indistinguishable from a flaky lamp, and not the lamp's fault.

An unprovisioned lamp is not on the network at all and will not appear here.
Look for its setup AP instead.

## OTA updates

A lamp can find out about a new release three ways:

| Trigger | What it does |
|---|---|
| 15 s after joining WiFi | Checks, and installs if the tag differs |
| Daily at 15:00 local | Same |
| `POST /api/ota/update` | Same, on someone else's schedule |
| The settings page, or `POST /api/ota/check` | Checks only, and reports |

The first two are unattended, so they install on their own — nobody is watching,
and there is no point holding an update back for an empty room. A check you
asked for never installs anything: it tells you what is available and offers a
button, so you can see what you are about to take.

Mid-afternoon is deliberate rather than overnight: an update that goes wrong
reboots the lamp, and 15:00 is when someone is around to notice. There is no
rollback, so the hour is the only safety margin.

Comparison is by tag equality, not ordering, so re-tagging an older release is a
supported way to roll back — and a lamp running firmware *newer* than the latest
release will install the older one. Cut the release before flashing a new
version by hand, or the next check quietly undoes it.

### From a script

Both endpoints answer JSON and take POST, never GET — a link a browser can
prefetch should not be able to reflash a lamp.

```sh
curl -X POST http://glow-lamp-051860.local/api/ota/check    # ask GitHub
curl -s http://glow-lamp-051860.local/api/status            # read the answer
curl -X POST http://glow-lamp-051860.local/api/ota/install  # take it

curl -X POST http://glow-lamp-051860.local/api/ota/update   # both, if there is one
```

The last one is the scheduler's endpoint: it checks, installs only if the
release differs, and does nothing at all on a lamp that is current. Home
Assistant can fire it at every lamp nightly — see
[docs/home-assistant.md](docs/home-assistant.md).

`status.json` carries the firmware block the scan and the status page both read:

```json
{"version":"0.0.2",
 "ota":{"state":"idle","latest":"0.0.3","available":true,"checked":12,"error":""}}
```

`state` is `idle`, `checking`, `installing` or `error`, and `checked` is seconds
since the last completed check (`-1` if none this boot).

While a lamp installs, it answers nothing — the download blocks its loop for
10–30 s and the ring holds its last color — and then it reboots on the new
version. The status page says so before it starts and recovers on its own.

### Cutting a release

```sh
# bump FIRMWARE_VERSION in src/version.h first, and commit
./scripts/release.sh 0.0.3
./scripts/find_devices.sh --update     # or let them find it themselves
```

The script refuses to tag if the version does not match `src/version.h`, if the
working tree is dirty, or if the binary would overflow the 1,966,080-byte OTA
slot — a build that overflows uploads fine over USB and then fails silently over
the air.

## Docs

| | |
|---|---|
| [docs/rest-api.md](docs/rest-api.md) | The REST API: endpoints, status fields, effects |
| [docs/mqtt.md](docs/mqtt.md) | The MQTT message API: topics, payloads, discovery |
| [docs/home-assistant.md](docs/home-assistant.md) | Home Assistant configuration |
| [docs/home-assistant-control.md](docs/home-assistant-control.md) | Why MQTT rather than REST from Home Assistant |
| [docs/wiki.mediawiki](docs/wiki.mediawiki) | The whole project as one page, in MediaWiki syntax, for pasting into a wiki |

Each lamp also serves its own REST reference at `http://<lamp>/help`, and the
same thing as JSON at `/api` — those describe the firmware actually running.

## License

MIT. See [LICENSE](LICENSE).
