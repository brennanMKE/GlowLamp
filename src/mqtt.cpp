#include "mqtt.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_log.h>

#include "effects.h"
#include "every_n_millis.h"
#include "lamp.h"
#include "settings.h"
#include "version.h"

static const char *TAG = "MQTT";

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static const uint32_t RETRY_MS = 5000;
static uint32_t retryAt = 0;

// What was last published, so state goes out when something changes rather than
// 60 times a second. The lamp's live color moves constantly under blend; the
// color reported to Home Assistant is the palette's first entry instead, which
// is stable and is what someone picked.
static bool lastPower = false;
static uint8_t lastBrightness = 0;
static String lastEffect;
static uint32_t lastColor = 0xFFFFFFFF;
static bool everPublished = false;

// Stable across renames, because it is the thing Home Assistant keys the entity
// on: renaming a lamp should rename its entity, not create a second one.
static String deviceId() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[32];
    snprintf(buf, sizeof(buf), "glowlamp_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    return String(buf);
}

static String baseTopic() { return "glowlamp/" + settings.hostName(); }
static String stateTopic() { return baseTopic() + "/state"; }
static String commandTopic() { return baseTopic() + "/set"; }
static String availabilityTopic() { return baseTopic() + "/availability"; }

bool mqttEnabled() { return settings.mqttHost().length() > 0; }
bool mqttConnected() { return mqtt.connected(); }

void mqttSettingsChanged() {
    if (mqtt.connected()) {
        // Say goodbye before hanging up. A last will only fires on an
        // UNGRACEFUL disconnect -- a clean one suppresses it -- so turning MQTT
        // off or moving the lamp to another broker would otherwise leave Home
        // Assistant showing an entity that is still "available" and frozen on
        // whatever state was last published. A reboot or a dropped link is
        // covered by the will; this is the case the will cannot see.
        mqtt.publish(availabilityTopic().c_str(), "offline", true);
        mqtt.disconnect();
    }
    retryAt = 0;  // reconnect on the next pass rather than after the backoff
}


// The color to report: the first palette entry of whatever is running. Not the
// live blended color -- that changes every frame, and publishing it would be a
// state message per frame for a value nobody can act on.
static uint32_t reportedColor() {
    uint32_t colors[MAX_COLORS];
    uint8_t count = lampEffectColors(colors);
    return count ? colors[0] : 0;
}

static void publishState(bool force = false) {
    if (!mqtt.connected()) return;

    bool power = lampPower();
    uint8_t brightness = lampBrightness();
    String effect = lampEffectName();
    uint32_t color = reportedColor();

    if (!force && everPublished && power == lastPower && brightness == lastBrightness &&
        effect == lastEffect && color == lastColor) {
        return;
    }

    JsonDocument doc;
    // Home Assistant's JSON light schema. "state" is the word ON/OFF, not a
    // boolean -- a boolean here is accepted silently and never turns the entity
    // on, which is a miserable thing to debug.
    doc["state"] = power ? "ON" : "OFF";
    doc["brightness"] = brightness;
    doc["color_mode"] = "rgb";
    JsonObject rgb = doc["color"].to<JsonObject>();
    rgb["r"] = (color >> 16) & 0xFF;
    rgb["g"] = (color >> 8) & 0xFF;
    rgb["b"] = color & 0xFF;
    doc["effect"] = effect;

    String payload;
    serializeJson(doc, payload);

    // Retained, so Home Assistant knows the lamp's state the moment it
    // restarts rather than showing "unknown" until something changes.
    if (!mqtt.publish(stateTopic().c_str(), payload.c_str(), true)) {
        ESP_LOGE(TAG, "state publish to %s FAILED (%u bytes) -- see setBufferSize()",
                 stateTopic().c_str(), (unsigned)payload.length());
        return;
    }

    lastPower = power;
    lastBrightness = brightness;
    lastEffect = effect;
    lastColor = color;
    everPublished = true;
}

// The retained discovery config that makes the lamp an entity on its own. Keyed
// by unique_id, so republishing after a rename updates the existing entity
// instead of leaving a duplicate behind.
static void publishDiscovery() {
    String id = deviceId();

    JsonDocument doc;
    doc["schema"] = "json";
    doc["name"] = settings.displayName();
    doc["unique_id"] = id;
    doc["state_topic"] = stateTopic();
    doc["command_topic"] = commandTopic();
    doc["availability_topic"] = availabilityTopic();
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";
    doc["brightness"] = true;
    doc["supported_color_modes"][0] = "rgb";
    doc["effect"] = true;
    for (uint8_t i = 0; i < EFFECT_COUNT; i++) doc["effect_list"][i] = EFFECT_NAMES[i];

    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0] = id;
    device["name"] = settings.displayName();
    device["manufacturer"] = "Glow Lamp";
    device["model"] = "Ring of 8";
    device["sw_version"] = FIRMWARE_VERSION;
    device["configuration_url"] = "http://" + settings.hostName() + ".local/";

    String topic = "homeassistant/light/" + id + "/config";
    String payload;
    serializeJson(doc, payload);

    if (!mqtt.publish(topic.c_str(), payload.c_str(), true)) {
        ESP_LOGE(TAG, "discovery publish FAILED (%u bytes) -- raise setBufferSize()",
                 (unsigned)payload.length());
        return;
    }
    ESP_LOGI(TAG, "discovery published to %s (%u bytes)", topic.c_str(),
             (unsigned)payload.length());
}

// Home Assistant sends the JSON light schema: any of state, brightness, color
// and effect, in one message.
//
// Everything set from here is applied with no expiry. Home Assistant is a
// controller, not someone trying something out -- an effect that reverted after
// five minutes would leave the entity showing a state the lamp no longer has.
static void onMessage(char *topic, byte *payload, unsigned int length) {
    String msg;
    msg.reserve(length);
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    ESP_LOGI(TAG, "%s: %s", topic, msg.c_str());

    JsonDocument doc;
    if (deserializeJson(doc, msg)) {
        ESP_LOGW(TAG, "could not parse command as JSON");
        return;
    }

    if (doc["brightness"].is<int>()) {
        long v = doc["brightness"].as<long>();
        if (v >= 0 && v <= 255) {
            setLampBrightness((uint8_t)v);
            settings.saveBrightness((uint8_t)v);
        }
    }

    // Color and effect both land as "the current effect, with this palette", so
    // they are resolved together before either is applied.
    bool haveColor = doc["color"]["r"].is<int>();
    bool haveEffect = doc["effect"].is<const char *>();
    if (haveColor || haveEffect) {
        uint8_t mode = lampEffectMode();
        if (haveEffect) {
            String wanted = doc["effect"].as<String>();
            for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
                if (wanted == EFFECT_NAMES[i]) mode = i;
            }
        }

        uint32_t colors[MAX_COLORS];
        uint8_t count;
        if (haveColor) {
            // Home Assistant's color picker is one color, so it replaces the
            // palette rather than joining it.
            colors[0] = ((uint32_t)(doc["color"]["r"].as<uint8_t>()) << 16) |
                        ((uint32_t)(doc["color"]["g"].as<uint8_t>()) << 8) |
                        doc["color"]["b"].as<uint8_t>();
            count = 1;
        } else {
            count = lampEffectColors(colors);
        }
        setLampEffect(mode, colors, count, 0);
    }

    // State last: turning on and setting a color in one message should end up
    // on, whatever order the fields arrived in.
    if (doc["state"].is<const char *>()) {
        String state = doc["state"].as<String>();
        state.toUpperCase();
        bool on = state == "ON";
        setLampPower(on);
        settings.savePower(on);
    }

    publishState(true);
}

void setupMqtt() {
    mqtt.setCallback(onMessage);
}

void loopMqtt() {
    if (!mqttEnabled()) return;

    if (mqtt.connected()) {
        mqtt.loop();
        publishState();
        return;
    }

    if (WiFi.status() != WL_CONNECTED) return;

    // Non-blocking: one attempt every RETRY_MS, never a wait loop. A broker
    // that is down must not cost the LED loop a single frame.
    uint32_t now = millis();
    if (!timeReached(now, retryAt)) return;
    retryAt = now + RETRY_MS;

    mqtt.setServer(settings.mqttHost().c_str(), settings.mqttPort());

    // PubSubClient defaults to 256 bytes for the WHOLE packet -- topic, payload
    // and overhead -- and silently drops anything larger: no callback, no log,
    // no error. The discovery config is ~600 bytes on its own. This has to be
    // set on every connect, because unlike setServer() and setCallback() it
    // does not survive a PubSubClient reconnect. GlowKitchen learned this the
    // hard way with SET_EFFECT payloads.
    mqtt.setBufferSize(1024);

    String clientId = deviceId();
    ESP_LOGI(TAG, "connecting to %s:%u", settings.mqttHost().c_str(), settings.mqttPort());

    // The last will is what makes the entity go unavailable when a lamp drops
    // off, rather than sitting there showing its last known state forever.
    bool ok;
    String avail = availabilityTopic();
    if (settings.mqttUser().length()) {
        ok = mqtt.connect(clientId.c_str(), settings.mqttUser().c_str(),
                          settings.mqttPass().c_str(), avail.c_str(), 0, true, "offline");
    } else {
        ok = mqtt.connect(clientId.c_str(), nullptr, nullptr, avail.c_str(), 0, true, "offline");
    }

    if (!ok) {
        ESP_LOGW(TAG, "connect failed, rc=%d", mqtt.state());
        return;
    }

    ESP_LOGI(TAG, "connected");
    mqtt.publish(avail.c_str(), "online", true);
    mqtt.subscribe(commandTopic().c_str());
    ESP_LOGI(TAG, "subscribed to %s", commandTopic().c_str());

    publishDiscovery();
    publishState(true);
}
