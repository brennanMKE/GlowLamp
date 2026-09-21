# REST API

Every lamp serves this reference itself at `http://<lamp>/help`, and the same
thing as JSON at `/api` — those always describe the firmware actually running,
where this file describes the firmware in this repo. They should agree; the lamp
is right when they do not.

## Conventions

**Values** may be sent as a query parameter, a form field, or a JSON body.

**A JSON body is only read when the request sets `Content-Type: application/json`.**
Curl's default content type is form-encoded, and a JSON document sent that way
is discarded by the HTTP server before the firmware sees it — the field has no
`=` in it, so the form parser drops the whole body. This is silent:

```sh
curl -X POST http://lamp.local/api/power -d '{"on":false}'      # sends nothing
curl -X POST 'http://lamp.local/api/power?on=false'             # works
curl -X POST -H 'Content-Type: application/json' \
     -d '{"on":false}' http://lamp.local/api/power              # works
```

Query parameters never have that problem, which is why they lead in the
examples below.

**Anything that changes the lamp is POST only**, so nothing a browser can
prefetch can switch a lamp off or reflash it.

**A missing or unreadable value is refused** with a 400 and a JSON `error`,
rather than falling back to a default.

**Every mutating call returns the same object as `GET /api/status`**, so the new
state never needs a second request.

**There is no authentication.** These are LAN devices; anything that can reach
one can control it.

## Endpoints

| Method | Path | Body / parameters |
|---|---|---|
| GET | `/api` | — |
| GET | `/api/status` | `?pretty=0` for compact JSON |
| POST | `/api/power` | `{"on": true \| false \| "toggle"}` |
| POST | `/api/brightness` | `{"value": 0-255}` |
| POST | `/api/identify` | `{"seconds": 1-60}` |
| POST | `/api/alert` | `{"seconds": 30}` |
| POST | `/api/alert/clear` | — |
| GET | `/api/effects` | — |
| POST | `/api/effect` | `{"effect": "...", "colors": [...], "seconds": 300}` |
| POST | `/api/effect/reset` | — |
| POST | `/api/name` | `{"name": "...", "hostname": "..."}` |
| POST | `/api/broker` | `{"host": "...", "port": 1883, "user": "", "pass": ""}` |
| POST | `/api/ota/check` | — |
| POST | `/api/ota/install` | — |
| POST | `/api/ota/update` | — |
| POST | `/api/reboot` | — |

### Power

`on` accepts `true`/`false`, `"toggle"`, and the words that turn up in shell
scripts: `on`/`off`, `1`/`0`, `yes`/`no`. Persists across a reboot, so a lamp
Home Assistant switched off at night stays off through a power cut.

### Brightness

`value` is 0–255. Persists. Independent of power: setting it on a lamp that is
off changes what it comes back on at.

### Identify

Blinks the ring white for a few seconds at a floor brightness of 160, so a lamp
dimmed to 5 still announces itself. Overrides power and the effect, and restores
whatever was showing — identifying a lamp that is off leaves it off.

### Alert

```sh
curl -X POST 'http://lamp.local/api/alert?seconds=30'
curl -X POST http://lamp.local/api/alert/clear
```

A red pulse to get attention. Overrides the running effect, the brightness
setting and the power state — a lamp that is switched off is exactly the one an
alert needs to reach — then restores all three. Defaults to 30 s, capped at one
hour, and always expires.

This is the REST twin of the MQTT alert topic, which is how Home Assistant fires
one; see [mqtt.md](mqtt.md). It exists so an alert can be tested and fired with
no broker involved, including when the broker is the thing that has gone wrong.

### Effects

```sh
curl http://lamp.local/api/effects
curl -X POST 'http://lamp.local/api/effect?effect=neon&colors=ff0000,00ff00,0000ff'
curl -X POST http://lamp.local/api/effect/reset
```

| Effect | What it does |
|---|---|
| `blend` | The whole ring holds one color and eases to the next. The default. |
| `loop` | Walks the palette steadily, never resting on a color. |
| `flicker` | Similar colors mixing and guttering, like a flame. |
| `neon` | One color at full brightness, held steady, switching cleanly to the next. |

**The ring shows one color at a time.** It is one light behind a diffuser, not
eight addressable pixels: several colors an inch apart mix into white. So
`blend`, `loop` and `neon` light the whole ring a single color, and differ in
how that color changes over time rather than in where it sits on the ring.

`flicker` is the deliberate exception, and relies on that mixing — give it a few
colors that are already close together, like reds and ambers, and the LEDs
blurring into each other at different brightnesses read as fire. Five colors
from opposite sides of the wheel will go white.

`colors` is up to 5 hex strings. The `#` is optional, which matters in a URL
where it would otherwise start a fragment. Omitting `colors` keeps the palette
the lamp is already showing, so switching effect is a one-field call.

`seconds` defaults to **300** and is capped at **28800** (8 hours); a larger
value is clamped rather than refused. `0` means until the lamp reboots.

**Effects are deliberately temporary.** When one expires the lamp returns to
blending its five default colors, and nothing persists an effect across a
reboot — a lamp that loses power comes back showing what it is supposed to show
rather than whatever someone was trying last week.

Colors are rendered as hue and saturation, not RGB, and transitions travel the
short way around the color wheel. An RGB blend between two colors far apart on
the wheel passes through gray and reads as dirty white on a WS2812B; a hue blend
never leaves full saturation. The colors you send are echoed back unchanged —
`status.effect.colors` reports what you set, not what the conversion made of it.

### Names

`name` is free text and takes effect immediately. `hostname` is lowercase
letters, digits and hyphens, and needs a reboot: mDNS has already published the
old one by the time the request is answered. A hostname that sanitizes away to
nothing is a 400.

### MQTT broker

```sh
curl -X POST 'http://lamp.local/api/broker?host=192.168.1.10&port=1883'
curl -X POST 'http://lamp.local/api/broker?host='      # turns MQTT off
```

An empty `host` disables MQTT; an empty `pass` leaves the stored one alone. The
password is never echoed back in `status`. Takes effect without a reboot. See
[mqtt.md](mqtt.md).

### Firmware

`/api/ota/check` asks GitHub and installs nothing — read the result from `ota`
in the status a second or two later. `/api/ota/install` takes the latest release
unconditionally. `/api/ota/update` does both: check, and install only if the tag
differs. That last one is the scheduler's endpoint — it is a no-op on a lamp
that is current, so it is safe to fire at every lamp nightly.

Comparison is by tag equality, not ordering, so a lamp running firmware newer
than the latest release will install the older one.

A lamp answers the request first, then stops responding for 10–30 s while it
downloads, then reboots.

## Status

```json
{
  "name": "Castor",
  "hostname": "glow-lamp",
  "mdns": "glow-lamp-051860.local",
  "version": "0.0.5",
  "power": "on",
  "on": true,
  "brightness": 64,
  "color": "#6f9044",
  "identifying": false,
  "effect": {
    "name": "blend",
    "default": true,
    "expires_in": -1,
    "colors": ["#ff0000", "#ab6a00", "#00ff00", "#0036ca", "#6a0096"]
  },
  "network": {"online": true, "ssid": "...", "ip": "...", "rssi": -63, "mac": "..."},
  "ota": {"state": "idle", "latest": "0.0.5", "available": false, "checked": 29, "error": ""},
  "uptime": 50
}
```

| Field | Meaning |
|---|---|
| `color` | What the ring is showing right now, before brightness scaling. Reports the color the effect is on even while the lamp is off. |
| `effect.expires_in` | Seconds until it reverts. `-1` when the default is running or the effect lasts until reboot. |
| `ota.state` | `idle`, `checking`, `installing` or `error`. |
| `ota.checked` | Seconds since the last completed check, `-1` if none this boot. |

## Finding lamps

Lamps announce `_glowlamp._tcp` on port 80. The TXT records carry `name`,
`device` (the hostname), `fw`, `api` and `help`.

```sh
./scripts/glowlamp.py discover
./scripts/find_devices.sh
```

mDNS does not cross VLANs or a guest network. An unprovisioned lamp is not on
the network at all — look for its setup AP, named `<hostname>-Setup-<mac>`.
