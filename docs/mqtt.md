# MQTT message API

The complete pub/sub interface. [home-assistant-control.md](home-assistant-control.md)
is the reasoning for having it; the short version is that MQTT is the only
option where **Home Assistant needs nothing done to it** — no YAML, no restart,
no filesystem access to the Home Assistant host.

MQTT is optional and off by default. A lamp with no broker configured never
connects and is fully usable over [REST](rest-api.md), which stays the interface
for `glowlamp.py`, identify, and OTA.

## Enabling

Settings page → **Home Assistant** → broker address. Or:

```sh
./scripts/glowlamp.py broker 192.168.1.10 --host castor-lamp.local
curl -X POST 'http://castor-lamp.local/api/broker?host=192.168.1.10&port=1883'
curl -X POST 'http://castor-lamp.local/api/broker?host='        # turns MQTT off
```

Takes effect immediately, no reboot. The stored password is never echoed back in
`/api/status`, and sending an empty one leaves it alone, so the broker address
can be changed without knowing the password.

The lamp appears in Home Assistant within a second or two as `light.<name>`.

## Connection

| | |
|---|---|
| Client ID | `glowlamp_<mac>`, e.g. `glowlamp_44b176051860` |
| Auth | Optional username/password; anonymous when no username is set |
| TLS | **None.** Plain MQTT on the LAN, like the REST API |
| QoS | 0, publish and subscribe |
| Keepalive | PubSubClient's default, 15 s |
| Reconnect | One attempt every 5 s, never blocking the LED loop |
| Max packet | 1024 bytes, set on every connect |

A lamp reconnects on its own after a broker restart and republishes discovery,
availability and state each time — so a broker without persistence, which loses
its retained messages on restart, recovers without anyone touching the lamps.

## Topics

`<hostname>` is the lamp's mDNS hostname, e.g. `castor-lamp`. `<mac>` is its MAC
with no separators.

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `homeassistant/light/glowlamp_<mac>/config` | publish | **yes** | Discovery config |
| `glowlamp/<hostname>/state` | publish | **yes** | Current state |
| `glowlamp/<hostname>/availability` | publish | **yes** | `online` / `offline` |
| `glowlamp/<hostname>/set` | **subscribe** | **never** | Command |
| `glowlamp/<hostname>/alert` | **subscribe** | **never** | Fire or clear an alert |
| `glowlamp/all/alert` | **subscribe** | **never** | The same, to every lamp at once |
| `glowlamp/<hostname>/alert/state` | publish | no | `on` / `off` while an alert runs |
| `glowlamp/<hostname>/sequence` | **subscribe** | **never** | Select a sequence |
| `glowlamp/all/sequence` | **subscribe** | **never** | The same, to every lamp at once |
| `glowlamp/<hostname>/pulse` | **subscribe** | **never** | A dip and a white flash |
| `glowlamp/all/pulse` | **subscribe** | **never** | The same, to every lamp at once |
| `glowlamp/<hostname>/ota` | **subscribe** | **never** | Firmware update from GitHub |
| `glowlamp/all/ota` | **subscribe** | **never** | The same, to every lamp at once |

`all` means every lamp on **this broker**. Lamps in another building on another
broker are a separate `all` — but they still show the same colors, because sync
comes from the clock, not from the broker. See [Sequences](#sequences).

**Never retain a command.** A retained command replays on every reconnect, which
pins the lamp to whatever was last sent and makes it impossible to control from
anywhere else. This is not hypothetical — it is the stale-brightness bug already
diagnosed on the GlowKitchen strips, where retained messages kept resetting the
fleet. Discovery, state and availability are retained on purpose, so Home
Assistant knows the lamp the moment it restarts.

## Subscribe: `glowlamp/<hostname>/set`

Home Assistant's JSON light schema. Any subset of the fields, in one message.

```json
{"state": "ON", "brightness": 200, "effect": "ocean"}
```

| Field | Type | Meaning |
|---|---|---|
| `state` | `"ON"` / `"OFF"` | Power. Persisted, so it survives a power cut. |
| `brightness` | 0–255 | Persisted. Independent of power. |
| `effect` | A [sequence](#sequences) name | Fades to that sequence over 3 s. Persisted. |
| `color` | `{"r":,"g":,"b":}` | Sets **breathe**'s color, persisted. Sent without `effect`, it also selects breathe. |

```sh
mosquitto_pub -h broker -t glowlamp/castor-lamp/set -m '{"state":"OFF"}'
mosquitto_pub -h broker -t glowlamp/castor-lamp/set -m '{"state":"ON","brightness":200}'
mosquitto_pub -h broker -t glowlamp/castor-lamp/set -m '{"effect":"sunset"}'
mosquitto_pub -h broker -t glowlamp/castor-lamp/set -m '{"color":{"r":0,"g":0,"b":255}}'
```

Ordering within a message: `brightness` first, then `color` and `effect`
together, then `state`. So "turn on and set a color" in one message ends up on,
whichever order the fields were written in.

A state publish follows every accepted command, so a caller never has to ask
what happened.

### Validation

Every field is validated independently, and anything unusable is ignored rather
than partially applied. A bad field never prevents the good fields in the same
message from landing.

| Sent | Result |
|---|---|
| Malformed JSON | Whole message ignored, logged |
| `{}` | Nothing changes |
| `{"state": true}` | **Ignored** — see below |
| `{"effect": "chase"}` | Sequence unchanged; other fields still apply |
| `{"brightness": 999}` | Brightness unchanged; other fields still apply |

`state` must be the **word** `"ON"` or `"OFF"`, not a boolean. `{"state": true}`
is accepted silently and never turns the lamp on, which is a miserable thing to
debug — it is the single most common mistake with the JSON light schema.

### Not supported

`transition`, `flash`, `color_temp`, `white`, `hs`/`xy` color, and the custom
effects (`blend`, `loop`, `flicker`, `neon` with your own palette of up to five
colors). Those are REST and effects-page only, because Home Assistant's light
entity has one color picker. See [rest-api.md](rest-api.md#effects).

## Subscribe: `glowlamp/<hostname>/alert` and `glowlamp/all/alert`

A red pulse loud enough to be noticed from across a room, for firing at
something that needs a person: a door left open, a cycle finished, a sensor gone
quiet.

```sh
mosquitto_pub -h broker -t glowlamp/all/alert -m '{"seconds": 30}'
mosquitto_pub -h broker -t glowlamp/all/alert -m '60'        # a bare number works
mosquitto_pub -h broker -t glowlamp/all/alert -m ''          # empty: the 30s default
mosquitto_pub -h broker -t glowlamp/all/alert -m 'off'       # clear it now
```

The payload is deliberately forgiving. An alert is fired from an automation in a
hurry, and the difference between `{"seconds":30}` and an empty message should
not decide whether anyone is warned.

**`glowlamp/all/alert` reaches every lamp with one publish.** Firing an alert
almost never means "that lamp" — it means "whoever is in the building" — and a
broadcast keeps an automation from naming each lamp and being edited when a
third one arrives.

### What an alert overrides

An alert is an overlay, not an effect. It overrides:

* the running **effect**,
* the **brightness** setting — it runs at full, because an alert nobody can see
  at brightness 5 is not an alert,
* the **power state** — a lamp switched off is exactly the one an alert needs to
  reach.

When it ends, all three go back to exactly what they were, including being off.

**It always expires.** 30 seconds by default, one hour at the ceiling. An alert
that stays on forever stops being an alert, and a lamp stuck red because a
broker went down is worse than no alert at all.

Identify wins over an alert, because someone standing at the lamp pressing
identify can see the alert on it either way.

### From Home Assistant

`mqtt.publish` already exists wherever the MQTT integration is set up, so an
alert needs no `rest_command`, no YAML-only integration, and no restart:

```yaml
script:
  alert_lamps:
    alias: Flash the lamps red
    sequence:
      - action: mqtt.publish
        data:
          topic: glowlamp/all/alert
          payload: '{"seconds": 30}'

automation:
  - alias: Warn when the back door is left open
    triggers:
      - trigger: state
        entity_id: binary_sensor.back_door
        to: "on"
        for: "00:05:00"
    actions:
      - action: mqtt.publish
        data:
          topic: glowlamp/all/alert
          payload: '{"seconds": 60}'
```

To stop one early — the door was closed, the thing was dealt with:

```yaml
      - action: mqtt.publish
        data:
          topic: glowlamp/all/alert
          payload: "off"
```

### Testing it

From any machine that can reach the broker:

```sh
mosquitto_pub -h <broker> -u <user> -P <pass> -t glowlamp/all/alert -m '{"seconds": 15}'
```

Both lamps should pulse red within a second, at full brightness, whatever they
were showing and **even if they are switched off**. They return to exactly what
they were doing when it expires.

If nothing happens, check in this order:

1. `curl http://castor-lamp.local/api/status` — is `mqtt.connected` true? If not,
   `mqtt.error` says why.
2. `mosquitto_sub -h <broker> -u <user> -P <pass> -t 'glowlamp/#' -v` in another
   terminal — does the publish appear at all?
3. Fire it over REST instead: `curl -X POST 'http://castor-lamp.local/api/alert?seconds=15'`.
   If that pulses and the MQTT publish does not, the lamp is fine and the broker
   path is the problem.

### Reading alert state

`glowlamp/<hostname>/alert/state` carries `on` or `off`, for a binary sensor:

```yaml
mqtt:
  binary_sensor:
    - name: Castor alerting
      state_topic: glowlamp/castor-lamp/alert/state
      payload_on: "on"
      payload_off: "off"
```

It is **not retained**: a retained `on` would come back after a broker restart
and describe an alert that finished hours ago.

## Sequences

A sequence is a named look: its colors, how it moves between them, and how fast.
One is always selected. It is what Home Assistant lists as the light's effects,
and it survives a power cut.

| Sequence | Colors | Behavior | In step across lamps |
|---|---|---|---|
| `vibrant` | red, gold, green, azure, magenta | Eases from one to the next, 6 s each. The default. | yes |
| `ocean` | teal, azure, blue, indigo | Slow blend, 10 s each, with a shallow 8 s swell | yes |
| `sunset` | magenta, red, orange, gold | Out to gold and back again, 12 s per step | yes |
| `aurora` | green, teal, violet | Uneven legs, 6–14 s, so it wanders | yes |
| `spectrum` | the whole wheel | One turn a minute | yes |
| `party` | the vibrant five | Holds, then snaps to the next, every 1.5 s | yes |
| `breathe` | one color you pick | Rises and falls over 5 s | yes |
| `ember` | red and amber | Flicker, like a fire | **no**, on purpose |

### Selecting one

```sh
mosquitto_pub -h broker -t glowlamp/all/sequence -m 'ocean'
mosquitto_pub -h broker -t glowlamp/castor-lamp/sequence -m '{"sequence":"party","fade":1}'
mosquitto_pub -h broker -t glowlamp/castor-lamp/set -m '{"effect":"aurora"}'   # same thing
```

`fade` is seconds, 0–30, default 3. Names are case-insensitive; an unknown name
is logged and ignored.

### How lamps stay in step

Nothing passes between the lamps. Each one draws its sequence from the wall
clock — NTP time, re-synced every 15 minutes — so two lamps on the same
sequence are showing the same moment of it, whether they share a broker, a
network, or neither. Typical agreement is within a few tens of milliseconds,
invisible on a multi-second blend.

* **Before NTP answers** a lamp runs on its own uptime. When the time arrives it
  steers into step without a visible jump: first by whole cycles, which look
  like nothing, then by running up to half again as fast, or half as fast, until
  it has caught up. `clock.synced` and `clock.error_ms` in `/api/status` show
  where it is.
* **Switching sequence** cross-fades over `fade` seconds, starting on the next
  whole second at least half a second out. A broadcast reaches lamps tens of
  milliseconds apart; starting on the same second makes them fade together.
* **`ember` is not synced.** Two fires flickering in lockstep look like one lamp
  being dimmed. Its colors and pace still match.
* A **custom effect** from the effects page or REST shows over the sequence
  until it expires. It runs on the same clock, so two lamps given the same one
  also match. Home Assistant keeps showing the sequence as the effect meanwhile.

## Subscribe: `glowlamp/<hostname>/pulse` and `glowlamp/all/pulse`

A flash — "something happened" — without the alarm of an alert. Nothing is
saved; the lamp goes straight back to what it was showing.

| Phase | Time (default 750 ms) | What the ring does |
|---|---|---|
| Dip | 263 ms | Dims to a fifth of its brightness |
| Rise | 396 ms | Climbs to the peak, slowly then fast, half the LEDs turning towards white near the top |
| Peak | 16 ms | Full brightness, **every other LED full white** — one frame |
| Return | 75 ms | Back to its color and brightness |

The dip and the white are what make it visible at any brightness, 255 included.
A saturated color lights one or two of each LED's channels; white lights all
three, so the flash outshines anything a sequence can show. Only four of the
eight LEDs go white, so the flash still reads as the sequence's color rather
than as a white bulb. `ms` stretches the
dip and rise; the peak and the return stay fixed.

```sh
mosquitto_pub -h broker -t glowlamp/all/pulse -m ''                    # 750 ms
mosquitto_pub -h broker -t glowlamp/all/pulse -m '1000'                # bare number: ms
mosquitto_pub -h broker -t glowlamp/castor-lamp/pulse -m '{"ms":800,"brightness":200}'
```

| Field | Default | Range |
|---|---|---|
| `ms` | 750 | 300–5000, clamped. The whole pulse. |
| `brightness` | 255 | 1–255, the peak. Never below the lamp's own brightness. |

**A lamp that is off ignores it.** Unlike an alert, a pulse is for a lamp
someone is already looking at. An alert or identify running at the same time
wins.

From Home Assistant:

```yaml
action: mqtt.publish
data:
  topic: glowlamp/all/pulse
  payload: ""
```

## Subscribe: `glowlamp/<hostname>/ota` and `glowlamp/all/ota`

The MQTT twin of `/api/ota/*`: the lamp asks GitHub for the latest release, and
installs it if told to.

```sh
mosquitto_pub -h broker -t glowlamp/all/ota -m ''                 # update every lamp
mosquitto_pub -h broker -t glowlamp/castor-lamp/ota -m 'check'    # ask, install nothing
mosquitto_pub -h broker -t glowlamp/castor-lamp/ota -m 'install'  # take it regardless
```

| Payload | What it does |
|---|---|
| empty or `update` | Check, and install only if the latest release differs from what is running. A no-op on a lamp that is current, so it is safe to fire at every lamp on a schedule. |
| `check` | Ask GitHub and report in `/api/status`; installs nothing |
| `install` | Take the latest release unconditionally. **Per-lamp topic only** — on `all` it is treated as `update`, because reinstalling every lamp at once is never what an automation means. |

An installing lamp drops off for 10–30 s, reboots on the new version, and comes
back with its sequence, brightness and power as they were. Home Assistant shows
it unavailable in between, from the last will.

Comparison is by tag equality, not ordering, so a lamp flashed by hand with a
version newer than the latest release will install the release — see the README.

```yaml
action: mqtt.publish
data:
  topic: glowlamp/all/ota
  payload: ""
```

## Publish: `glowlamp/<hostname>/state`

```json
{"state":"ON","brightness":90,"color_mode":"rgb","color":{"r":255,"g":0,"b":0},"effect":"vibrant"}
```

| Field | Meaning |
|---|---|
| `state` | `"ON"` or `"OFF"` |
| `brightness` | 0–255 |
| `color_mode` | Always `"rgb"` |
| `color` | The sequence's color — breathe's color, or the first step of any other — not the live one |
| `effect` | The selected sequence |

Published on connect, after every accepted command, and whenever power,
brightness, the sequence or its color changes — **not per frame**. The live
color moves continuously, so publishing it would be a message every 16 ms for a
value nothing can act on.

Changes made from the web UI or the REST API publish too, so Home Assistant
follows along when a lamp is changed at the lamp.

## Publish: `glowlamp/<hostname>/availability`

`online` on connect, `offline` otherwise. Registered as the connection's last
will, so a power cut or a dropped link marks the entity unavailable rather than
leaving it frozen on its last known state.

A last will only fires on an **ungraceful** disconnect. Turning MQTT off, or
moving a lamp to another broker, is a clean disconnect and suppresses it — so
the lamp publishes `offline` itself before hanging up. Without that, changing
brokers left a permanently "available" ghost entity behind.

## Publish: `homeassistant/light/glowlamp_<mac>/config`

Published retained on every connect. This is what creates the entity; nothing
needs to be added to `configuration.yaml`.

```json
{
  "schema": "json",
  "name": null,
  "unique_id": "glowlamp_44b176051860",
  "state_topic": "glowlamp/castor-lamp/state",
  "command_topic": "glowlamp/castor-lamp/set",
  "availability_topic": "glowlamp/castor-lamp/availability",
  "payload_available": "online",
  "payload_not_available": "offline",
  "brightness": true,
  "supported_color_modes": ["rgb"],
  "effect": true,
  "effect_list": ["vibrant", "ocean", "sunset", "aurora", "spectrum", "party", "breathe", "ember"],
  "device": {
    "identifiers": ["glowlamp_44b176051860"],
    "name": "Castor",
    "manufacturer": "Glow Lamp",
    "model": "Ring of 8",
    "sw_version": "0.0.5",
    "configuration_url": "http://castor-lamp.local/"
  }
}
```

`unique_id` is the MAC, not the name, so renaming a lamp **updates** the existing
entity rather than creating a second one. `configuration_url` puts a link to the
lamp's own settings page in Home Assistant's device panel.

`name` is `null` so Home Assistant names the entity after the device alone
(`light.castor`). Before 0.1.1 it repeated the device name, which produced
`light.castor_castor`. An entity created by an older version keeps its existing
`entity_id`; rename it in Home Assistant if you want the short form.

### Renaming

The command and state topics are built from the hostname, so changing it moves
them. The lamp republishes discovery pointing at the new topics, and Home
Assistant follows — but the **old retained state topic is left behind** holding a
final stale message. Nothing reads it, and it costs a few bytes on the broker.
To clear it:

```sh
mosquitto_pub -h broker -r -n -t glowlamp/old-name/state
mosquitto_pub -h broker -r -n -t glowlamp/old-name/availability
```

Removing a lamp from Home Assistant entirely means clearing its discovery topic
the same way:

```sh
mosquitto_pub -h broker -r -n -t homeassistant/light/glowlamp_44b176051860/config
```

## MQTT and REST together

Both interfaces drive the same lamp and both publish state, so they never
disagree. One deliberate difference:

**Nothing set over MQTT expires.** An effect set over REST reverts to the default
after five minutes unless told otherwise; one set over MQTT does not. Home
Assistant is a controller, not someone trying something out, and an effect that
reverted on its own would leave the entity showing a state the lamp no longer
has.

## Watching and troubleshooting

```sh
mosquitto_sub -h broker -v -t 'glowlamp/#' -t 'homeassistant/light/glowlamp_+/config'
```

| Symptom | Cause |
|---|---|
| Entity never appears | No retained discovery — check the lamp shows `connected` on its settings page |
| Entity is "unavailable" | Availability says `offline`: lamp is down, or on another broker |
| Commands do nothing | Check `state` is the word `"ON"`, not `true` |
| Lamp reverts after a reconnect | A **retained** command on the `set` topic. Clear it: `mosquitto_pub -r -n -t glowlamp/<host>/set` |
| Nothing publishes at all | A payload over the 1024-byte buffer is dropped silently by PubSubClient; the lamp logs an error at ERROR level, visible in the release build |
