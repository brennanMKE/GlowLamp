# MQTT

The lamp speaks MQTT so Home Assistant can treat it as an ordinary light. This
is the interface to use from Home Assistant;
[docs/home-assistant-control.md](home-assistant-control.md) is the reasoning,
and the short version is that MQTT is the only option where **Home Assistant
needs nothing done to it** — no YAML, no restart, no filesystem access.

MQTT is optional. A lamp with no broker configured never connects and is fully
usable over [REST](rest-api.md), which stays the interface for `glowlamp.py`,
identify, and OTA.

## Turning it on

Settings page → **Home Assistant** → broker address. Or:

```sh
./scripts/glowlamp.py broker 192.168.1.10 --host castor-lamp.local
curl -X POST 'http://castor-lamp.local/api/broker?host=192.168.1.10&port=1883'
```

An empty host turns MQTT off. Changes take effect immediately — no reboot. The
stored password is never echoed back, and sending an empty one leaves it alone,
so the host can be changed without knowing it.

The lamp appears in Home Assistant within a second or two as `light.<name>`,
with brightness, a color picker and the effect list. Nothing has to be added to
`configuration.yaml`.

## Topics

| Topic | Retained | What |
|---|---|---|
| `homeassistant/light/glowlamp_<mac>/config` | yes | Discovery. Published on every connect. |
| `glowlamp/<hostname>/state` | yes | Current state, as the JSON light schema. |
| `glowlamp/<hostname>/set` | **no** | Commands in. |
| `glowlamp/<hostname>/availability` | yes | `online` / `offline`. |

**Commands are never retained, and must not be.** A retained command replays on
every reconnect, which pins the lamp to whatever was last sent — the same
mistake diagnosed on the GlowKitchen strips, where stale retained brightness
messages kept resetting the fleet. Discovery, state and availability are
retained, so Home Assistant knows the lamp the moment it restarts.

The discovery topic is keyed by MAC, not by name, so renaming a lamp updates the
existing entity instead of leaving a duplicate behind.

## Commands

Home Assistant's JSON light schema. Any subset of the fields, in one message:

```sh
mosquitto_pub -h broker -t glowlamp/castor-lamp/set -m '{"state":"OFF"}'
mosquitto_pub -h broker -t glowlamp/castor-lamp/set -m '{"state":"ON","brightness":200}'
mosquitto_pub -h broker -t glowlamp/castor-lamp/set -m '{"state":"ON","effect":"neon"}'
mosquitto_pub -h broker -t glowlamp/castor-lamp/set -m '{"color":{"r":0,"g":0,"b":255}}'
```

| Field | Effect |
|---|---|
| `state` | `"ON"` or `"OFF"`. The word, not a boolean — a boolean is accepted silently and never turns the entity on. |
| `brightness` | 0–255. |
| `effect` | One of `blend`, `loop`, `flicker`, `neon`. Keeps the current palette. |
| `color` | `{"r":,"g":,"b":}`. Home Assistant's picker is one color, so it **replaces** the palette rather than joining it. |

State is applied last, so "turn on and set a color" in one message ends up on
whichever order the fields arrived in.

**Nothing set over MQTT expires.** An effect set from the REST API reverts to
the default after five minutes by default; one set over MQTT does not. Home
Assistant is a controller, not someone trying something out, and an effect that
reverted on its own would leave the entity showing a state the lamp no longer
has.

## State

Published on connect and whenever power, brightness, effect or the palette's
first color changes — not per frame. The live color moves continuously under
`blend`, and publishing that would be a message every 16 ms for a value nothing
can act on, so the color reported is the palette's first entry: stable, and the
one someone actually picked.

```json
{"state":"ON","brightness":90,"color_mode":"rgb","color":{"r":255,"g":32,"b":0},"effect":"flicker"}
```

Changes made from the web UI or the REST API publish too, so Home Assistant
follows along when someone turns a lamp off at the lamp.

## Availability

The lamp carries a last will, so a power cut or a dropped link marks the entity
unavailable rather than leaving it frozen on its last known state.

A last will only fires on an **ungraceful** disconnect. Turning MQTT off, or
moving a lamp to another broker, is a clean disconnect and suppresses it — so
the lamp publishes `offline` itself before hanging up. Without that, switching
brokers left a permanently "available" ghost entity behind.

## Notes

- **No TLS.** The connection is plain MQTT on the LAN, like the REST API.
- `PubSubClient` defaults to a 256-byte limit for the *whole* packet and
  silently drops anything larger — no callback, no log, no error. The discovery
  config alone is ~600 bytes. The buffer is raised on every connect, because
  unlike the server and callback it does not survive a reconnect.
- Reconnection is one attempt every 5 seconds and never blocks: a broker that is
  down must not cost the LED loop a frame.
