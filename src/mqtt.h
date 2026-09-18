#ifndef MQTT_H
#define MQTT_H

#include <Arduino.h>

// MQTT, for Home Assistant. Optional: a lamp with no broker configured never
// connects and is fully usable over REST, which stays the interface for
// glowlamp.py, identify, and OTA. See docs/home-assistant-control.md for why
// this exists alongside REST rather than instead of it.
//
// The lamp publishes a retained Home Assistant discovery config on connect, so
// it appears as a light entity with no YAML, no restart, and no filesystem
// access to the Home Assistant host.

void setupMqtt();
void loopMqtt();

// True when a broker is configured; connected() is whether we are talking to it
// right now.
bool mqttEnabled();
bool mqttConnected();

// Drop the connection so the next loop reconnects with current settings. Called
// when the broker settings are saved.
void mqttSettingsChanged();

#endif  // MQTT_H
