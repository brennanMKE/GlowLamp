# What Home Assistant can actually do with a lamp

[home-assistant.md](home-assistant.md) shows the configuration. This one is
about the constraint behind it: what Home Assistant can do with a lamp with no
setup at all, what each way in costs, and why MQTT is probably the answer.

## Out of the box: nothing

A lamp on the network is invisible to Home Assistant. It is not discovered, it
has no entity, and nothing in the UI can reach it. That is not a gap in the REST
API — it is that **Home Assistant ships no built-in service for making an HTTP
request**. The two integrations that can are `rest_command` and `shell_command`,
and both are YAML-only:

```
rest_command     -> YAML ONLY
shell_command    -> YAML ONLY
command_line     -> YAML ONLY
rest             -> YAML ONLY
```

YAML-only means they are read from `configuration.yaml` at startup. No API
creates them, the UI cannot add them, and a first-time addition needs a full
restart rather than a reload. So every REST route requires filesystem access to
the Home Assistant config directory — SSH, Samba, or the File Editor add-on.

This is worth stating plainly because it is the whole difference between a lamp
and a device that "just works" in Home Assistant, and it has nothing to do with
how good the REST API is.

## Three ways in

### 1. `rest_command`, one entry per lamp

The lamp's URL is written into `configuration.yaml`. Direct, no dependencies,
and it can be paired with a REST switch to get state by polling
`/api/status`.

The cost is that the address is now configuration. Every entry hardcodes a
hostname or an IP, so renaming a lamp or letting DHCP move it breaks Home
Assistant silently, and adding a third lamp is a config edit and a restart.

### 2. `shell_command` calling `glowlamp.py`

```yaml
shell_command:
  lamps_off: "/config/scripts/glowlamp.py off --all --timeout 3"
  castor_off: "/config/scripts/glowlamp.py off --name Castor --timeout 3"
```

Better, because `--name` matches a lamp discovered over mDNS. Names are stable
in a way hostnames are not, so renaming a host or a DHCP move changes nothing,
and `--all` picks up a new lamp with no config change.

Two things it gives up. **No state**: `shell_command` is fire-and-forget, so the
lamps never become entities and Home Assistant never knows whether a command
landed. And it depends on **mDNS reaching Home Assistant**, which is link-local
and does not cross subnets without a reflector. If Home Assistant sits on a
different VLAN from a lamp, discovery quietly returns fewer lamps than exist and
`--all` acts on a subset without reporting it.

There is no way around that second problem while keeping the first benefit: a
REST switch needs a fixed `resource:` URL, which is exactly what name-based
addressing avoids. **State and stable addressing are mutually exclusive over
REST.**

### 3. MQTT

The lamp connects to a broker and subscribes to a command topic. Home Assistant
publishes to it with `mqtt.publish`, a service that already exists wherever the
MQTT integration is set up.

If the lamp also publishes a **discovery config** on boot — retained, to
`homeassistant/light/<id>/config` — Home Assistant creates the entity itself.
No YAML, no API call, no restart, no filesystem access. `light.castor` appears,
with state, brightness and color, and works in automations, scenes, dashboards
and voice like any other light.

## Side by side

| | `rest_command` | `shell_command` | MQTT + discovery |
|---|---|---|---|
| Needs HA filesystem access | yes | yes | **no** |
| Needs an HA restart | yes | yes | **no** |
| Survives a rename or DHCP move | no | yes | yes |
| Works across subnets | yes | **mDNS-dependent** | yes, unicast |
| Lamp becomes an entity | switch only | no | **yes, a light** |
| Reports state back | polled | no | yes |
| Adding a third lamp | config edit | free with `--all` | free |
| Firmware change | none | none | **yes** |

## Recommendation: MQTT, in addition to REST

MQTT is the only option where Home Assistant needs nothing done to it. Everything
moves into firmware, which is the thing this project controls, and out of a
config file on a machine that may not even be reachable.

It also resolves the two weaknesses the REST routes cannot:

- **Addressing.** A broker is a fixed unicast address the lamp is configured
  with. No mDNS, no reflector, no assumption that the lamp and Home Assistant
  share a link. A lamp on another VLAN behaves identically to one in the same
  room.
- **State.** Discovery makes the lamp a real `light` entity. Brightness and
  color round-trip, availability is reported through a last will, and the lamp
  can participate in automations written against lights generally rather than
  automations written against this lamp specifically.

That last point is the one that compounds. A house-wide "turn everything off"
automation picks up a discovered lamp for free; it will never pick up a
`shell_command`.

### What the firmware would need

- An MQTT client. `PubSubClient` is the usual pick and is small — the ESP32-C3
  build has roughly 500 KB of app partition spare under `min_spiffs.csv`, so it
  fits without repartitioning.
- Broker host, port and credentials in settings, alongside the WiFi config.
- Reconnect handling that never blocks the LED loop, and a last will so the
  entity goes unavailable when a lamp drops.
- A retained discovery config published on boot, plus state topics for power,
  brightness and color.

**Do not retain command messages.** Retain discovery and availability; never
commands. A retained command replays on every reconnect, which pins a device to
whatever value was last sent — a mistake already made and diagnosed on the
GlowKitchen strips, where stale retained brightness messages kept resetting the
fleet.

### What stays REST

All of it. The REST API remains the right interface for `glowlamp.py`, for
`identify` when standing in front of a lamp, for OTA, and for anything driven
from a laptop — none of which should require a broker to be running. MQTT is an
addition, not a replacement, and a lamp should be fully usable with no broker
configured at all.

## If MQTT is not on the table

Use `shell_command` with `glowlamp.py --name`, not `rest_command`. Accept that
there is no state, and before trusting any `--all` automation, run discovery
**on the Home Assistant machine** rather than from a laptop:

```sh
/config/scripts/glowlamp.py discover --timeout 5
```

Every lamp must appear. A lamp missing from that list is a lamp Home Assistant
cannot control, and nothing about the automation will say so.
