#include "mqtt.h"

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_log.h>

#include "effects.h"
#include "every_n_millis.h"
#include "lamp.h"
#include "ota.h"
#include "sequences.h"
#include "settings.h"
#include "version.h"

static const char *TAG = "MQTT";

static WiFiClient wifiClient;
static PubSubClient mqtt(wifiClient);

static const uint32_t RETRY_MS = 5000;
static uint32_t retryAt = 0;

// What was last published, so state goes out when something changes rather than
// 60 times a second. The lamp's live color moves constantly under blend; the
// color reported to Home Assistant is the sequence's own color instead, which
// is stable and is what someone picked.
// Why the last connect attempt failed. Kept because the alternative is a serial
// cable: a broker that refuses a lamp does so silently from the network's point
// of view, and "not connected" on the settings page says nothing about whether
// the address is wrong, the credentials are, or the broker simply is not there.
static int lastRc = 0;

// So the alert's own state topic is published on the edges rather than every
// pass through the loop.
static bool lastAlerting = false;

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
static String alertTopic() { return baseTopic() + "/alert"; }
static String alertStateTopic() { return baseTopic() + "/alert/state"; }
static String sequenceTopic() { return baseTopic() + "/sequence"; }
static String pulseTopic() { return baseTopic() + "/pulse"; }
static String otaTopic() { return baseTopic() + "/ota"; }

// One publish that reaches every lamp. Home Assistant firing an alert almost
// never means "that lamp" -- it means "whoever is in the building" -- and a
// broadcast keeps an automation from having to name each lamp and be edited
// when a third one arrives.
static const char *ALERT_ALL_TOPIC = "glowlamp/all/alert";

// The same for sequences and pulses. A lamp only hears "all" from its own
// broker, so lamps in another building are a separate "all".
static const char *SEQUENCE_ALL_TOPIC = "glowlamp/all/sequence";
static const char *PULSE_ALL_TOPIC = "glowlamp/all/pulse";
static const char *OTA_ALL_TOPIC = "glowlamp/all/ota";

bool mqttEnabled() { return settings.mqttHost().length() > 0; }
bool mqttConnected() { return mqtt.connected(); }

int mqttLastRc() { return lastRc; }

// PubSubClient's state codes, which are otherwise a number to go and look up.
const char *mqttLastError() {
    switch (lastRc) {
        case 0: return "";
        case -4: return "timed out waiting for the broker";
        case -3: return "connection lost";
        case -2: return "could not reach the broker (check the address and port)";
        case -1: return "disconnected";
        case 1: return "broker refused the protocol version";
        case 2: return "broker rejected the client id";
        case 3: return "broker unavailable";
        case 4: return "broker rejected the username or password";
        case 5: return "broker refused authorization";
        default: return "unknown error";
    }
}

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
static void publishState(bool force = false) {
    if (!mqtt.connected()) return;

    bool power = lampPower();
    uint8_t brightness = lampBrightness();
    // The sequence, which is what Home Assistant knows as the effect -- even
    // while a custom effect from the effects page is showing over it -- and
    // the one color that stands for it.
    String effect = lampSequenceName();
    uint32_t color = lampSequenceColor();

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

// Separate from the light's state, because an alert is not a property of the
// light -- it is something happening to it, and Home Assistant reads this as a
// binary sensor rather than as part of the entity.
//
// Not retained: a retained "on" would come back after a broker restart and
// describe an alert that finished hours ago.
static void publishAlertState(bool force = false) {
    if (!mqtt.connected()) return;
    bool now = lampAlerting();
    if (!force && now == lastAlerting) return;
    mqtt.publish(alertStateTopic().c_str(), now ? "on" : "off", false);
    lastAlerting = now;
}

// The retained discovery config that makes the lamp an entity on its own. Keyed
// by unique_id, so republishing after a rename updates the existing entity
// instead of leaving a duplicate behind.
static void publishDiscovery() {
    String id = deviceId();

    JsonDocument doc;
    doc["schema"] = "json";
    // null, not the display name: Home Assistant then names the entity after
    // the device alone. Repeating the device name here doubled it in every
    // generated entity_id -- light.castor_castor.
    doc["name"] = nullptr;
    doc["unique_id"] = id;
    doc["state_topic"] = stateTopic();
    doc["command_topic"] = commandTopic();
    doc["availability_topic"] = availabilityTopic();
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";
    doc["brightness"] = true;
    doc["supported_color_modes"][0] = "rgb";
    doc["effect"] = true;
    // The sequences, not the custom effects: those need a palette, which a
    // light entity's one color picker cannot give them.
    for (uint8_t i = 0; i < SEQUENCE_COUNT; i++) doc["effect_list"][i] = SEQUENCES[i].name;

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

// Select a sequence, saving it. Used by the effect field and the sequence topics.
static void selectSequence(uint8_t index, uint32_t fadeMs) {
    setLampSequence(index, fadeMs);
    settings.saveSequence(SEQUENCES[index].name);
}

// glowlamp/<host>/sequence and glowlamp/all/sequence: a bare name, or
// {"sequence":"ocean","fade":3}. Fade is in seconds, 0-30.
static void onSequence(const String &msg) {
    String name = msg;
    name.trim();
    long fade = 3;

    JsonDocument doc;
    if (name.startsWith("{")) {
        if (deserializeJson(doc, name)) {
            ESP_LOGW(TAG, "could not parse sequence command as JSON");
            return;
        }
        name = doc["sequence"].is<const char *>() ? doc["sequence"].as<const char *>()
                                                    : (doc["name"] | "");
        if (doc["fade"].is<long>()) fade = doc["fade"].as<long>();
    }
    if (fade < 0) fade = 0;
    if (fade > 30) fade = 30;

    uint8_t index = findSequence(name.c_str());
    if (index == SEQUENCE_COUNT) {
        ESP_LOGW(TAG, "no sequence called \"%s\"", name.c_str());
        return;
    }
    selectSequence(index, (uint32_t)fade * 1000UL);
}

// glowlamp/<host>/pulse and glowlamp/all/pulse. Forgiving like the alert:
// empty for the defaults, a bare number for milliseconds, or
// {"ms":800,"brightness":255}.
static void onPulse(const String &msg) {
    String body = msg;
    body.trim();
    uint32_t ms = 0;     // 0 means the default, decided in main.cpp
    uint8_t level = 0;   // 0 means full

    JsonDocument doc;
    if (body.startsWith("{") && !deserializeJson(doc, body)) {
        if (doc["ms"].is<long>() && doc["ms"].as<long>() > 0) ms = doc["ms"].as<long>();
        if (doc["brightness"].is<long>()) {
            long v = doc["brightness"].as<long>();
            if (v >= 1 && v <= 255) level = (uint8_t)v;
        }
    } else if (body.toInt() > 0) {
        ms = (uint32_t)body.toInt();
    }
    pulseLamp(ms, level);
}

// glowlamp/<host>/ota and glowlamp/all/ota. Queued, like the REST calls: the
// work happens in loopOta(), not inside the MQTT callback.
//
//   "" or "update"  check, and install only if the release differs
//   "check"         ask GitHub and report; installs nothing
//   "install"       take the latest release regardless -- ONE lamp only
//
// "install" on the all topic is treated as "update". Reinstalling every lamp at
// once is never what an automation means, and "update" already installs
// wherever there is something new.
static void onOta(const String &msg, bool all) {
    String body = msg;
    body.trim();
    body.toLowerCase();
    if (body == "check") {
        ESP_LOGI(TAG, "OTA check requested");
        requestOtaCheck();
    } else if (body == "install" && !all) {
        ESP_LOGI(TAG, "OTA install requested");
        requestOtaInstall();
    } else {
        ESP_LOGI(TAG, "OTA update requested");
        requestOtaUpdate();
    }
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

    String t(topic);
    if (t.endsWith("/ota")) {
        onOta(msg, t == OTA_ALL_TOPIC);
        return;
    }
    if (t.endsWith("/pulse")) {
        onPulse(msg);
        return;
    }
    if (t.endsWith("/sequence")) {
        onSequence(msg);
        publishState(true);
        return;
    }
    if (t.endsWith("/alert")) {
        // Deliberately forgiving about the payload. An alert is fired from an
        // automation in a hurry, and the difference between {"seconds":30} and
        // an empty message should not decide whether anyone is warned.
        String body = msg;
        body.trim();
        body.toLowerCase();

        if (body == "off" || body == "clear" || body == "0" || body.indexOf("\"off\"") >= 0) {
            clearLampAlert();
        } else {
            uint32_t seconds = 0;  // 0 means "the default", decided in lamp.cpp
            JsonDocument alertDoc;
            if (!deserializeJson(alertDoc, msg) && alertDoc["seconds"].is<long>()) {
                long v = alertDoc["seconds"].as<long>();
                if (v <= 0) {
                    clearLampAlert();
                    publishState(true);
                    return;
                }
                seconds = (uint32_t)v;
            } else if (body.length() && body.toInt() > 0) {
                // A bare number, for an automation that would rather not build
                // JSON: `mqtt.publish` with payload "60".
                seconds = (uint32_t)body.toInt();
            }
            triggerLampAlert(seconds);
        }
        publishState(true);
        return;
    }

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

    // The effect is a sequence. A color is breathe's color, and on its own --
    // Home Assistant's color picker -- it also selects breathe, because that is
    // the sequence built around one color someone picked.
    bool haveColor = doc["color"]["r"].is<int>();
    uint8_t index = SEQUENCE_COUNT;
    if (doc["effect"].is<const char *>()) {
        index = findSequence(doc["effect"].as<const char *>());
        if (index == SEQUENCE_COUNT) ESP_LOGW(TAG, "unknown effect ignored");
    }
    if (haveColor) {
        uint32_t rgb = ((uint32_t)(doc["color"]["r"].as<uint8_t>()) << 16) |
                       ((uint32_t)(doc["color"]["g"].as<uint8_t>()) << 8) |
                       doc["color"]["b"].as<uint8_t>();
        setLampBreatheColor(rgb);
        settings.saveBreatheColor(rgb);
        if (index == SEQUENCE_COUNT && !doc["effect"].is<const char *>()) index = SEQ_BREATHE;
    }
    if (index < SEQUENCE_COUNT) selectSequence(index, 3000);

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
        publishAlertState();
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
        lastRc = mqtt.state();
        // ESP_LOGE, not LOGW: the release build logs nothing below ERROR, and a
        // lamp that cannot reach its broker is exactly the thing someone is
        // trying to find out about when they plug a serial cable in.
        ESP_LOGE(TAG, "connect to %s:%u failed, rc=%d (%s)", settings.mqttHost().c_str(),
                 settings.mqttPort(), lastRc, mqttLastError());
        return;
    }

    lastRc = 0;
    ESP_LOGI(TAG, "connected");
    mqtt.publish(avail.c_str(), "online", true);
    mqtt.subscribe(commandTopic().c_str());
    mqtt.subscribe(alertTopic().c_str());
    mqtt.subscribe(ALERT_ALL_TOPIC);
    mqtt.subscribe(sequenceTopic().c_str());
    mqtt.subscribe(SEQUENCE_ALL_TOPIC);
    mqtt.subscribe(pulseTopic().c_str());
    mqtt.subscribe(PULSE_ALL_TOPIC);
    mqtt.subscribe(otaTopic().c_str());
    mqtt.subscribe(OTA_ALL_TOPIC);
    ESP_LOGI(TAG,
             "subscribed to %s/{set,alert,sequence,pulse,ota} and "
             "glowlamp/all/{alert,sequence,pulse,ota}",
             baseTopic().c_str());

    publishDiscovery();
    publishState(true);
    publishAlertState(true);
}
