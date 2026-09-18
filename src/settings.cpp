#include "settings.h"

#include <ArduinoJson.h>
#include <Config.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_log.h>

#include "effects.h"
#include "lamp.h"
#include "mqtt.h"
#include "ota.h"
#include "version.h"
#include "wifi_link.h"

static const char *TAG = "SETTINGS";

// Shared with the OTA scheduler's ota_yday key -- one namespace for everything
// this firmware persists.
static const char *NVS_NS = "glowlamp";

// How long the identify flash runs when the caller does not say.
static const uint32_t IDENTIFY_DEFAULT_MS = 4000;
static const uint32_t IDENTIFY_MAX_MS = 60000;

void LampSettings::begin() {
    Preferences p;
    // Read-write, not read-only: opening a namespace that does not exist yet
    // read-only fails with NOT_FOUND. Read-write creates it, so a first boot is
    // silent instead of logging an error before falling back to the defaults.
    p.begin(NVS_NS, false);
    // isKey() before each read: getString() on a missing key logs an ESP_LOGE
    // before returning the default, which would make every first boot look like
    // a failure in the serial log.
    //
    // "name" held the hostname before display names existed, and still does. A
    // lamp that updates from an older build keeps the address it was reachable
    // at; renaming the key here would have silently moved every deployed lamp.
    host = p.isKey("name") ? p.getString("name") : String(DEVICE_NAME);
    label = p.isKey("label") ? p.getString("label") : host;
    if (p.isKey("bright")) bright = p.getUChar("bright");
    if (p.isKey("power")) on = p.getBool("power");
    broker = p.isKey("mqtt_host") ? p.getString("mqtt_host") : String(MQTT_HOST);
    brokerPort = p.isKey("mqtt_port") ? p.getUShort("mqtt_port") : MQTT_PORT;
    brokerUser = p.isKey("mqtt_user") ? p.getString("mqtt_user") : String(MQTT_USER);
    brokerPass = p.isKey("mqtt_pass") ? p.getString("mqtt_pass") : String(MQTT_PASS);
    p.end();

    ESP_LOGI(TAG, "host=%s label=%s brightness=%u power=%s broker=%s", host.c_str(),
             label.c_str(), bright, on ? "on" : "off",
             broker.length() ? broker.c_str() : "(none)");
}

void LampSettings::saveBrightness(uint8_t value) {
    bright = value;
    Preferences p;
    p.begin(NVS_NS, false);
    p.putUChar("bright", bright);
    p.end();
}

void LampSettings::savePower(bool value) {
    on = value;
    Preferences p;
    p.begin(NVS_NS, false);
    p.putBool("power", on);
    p.end();
}

void LampSettings::saveNames(const String &newLabel, const String &newHost) {
    label = newLabel;
    host = newHost;
    Preferences p;
    p.begin(NVS_NS, false);
    p.putString("label", label);
    p.putString("name", host);
    p.end();
}

void LampSettings::saveBroker(const String &newHost, uint16_t newPort, const String &newUser,
                              const String &newPass) {
    broker = newHost;
    brokerPort = newPort;
    brokerUser = newUser;
    brokerPass = newPass;

    Preferences p;
    p.begin(NVS_NS, false);
    p.putString("mqtt_host", broker);
    p.putUShort("mqtt_port", brokerPort);
    p.putString("mqtt_user", brokerUser);
    p.putString("mqtt_pass", brokerPass);
    p.end();

    // Drop the current connection so the new settings take effect without a
    // reboot -- the point of editing them on a running lamp.
    mqttSettingsChanged();
}

String LampSettings::sanitizeHost(const String &in) {
    String out;
    for (unsigned i = 0; i < in.length() && out.length() < 18; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += c;
        } else if ((c == '-' || c == ' ' || c == '_') && out.length() && out[out.length() - 1] != '-') {
            out += '-';
        }
    }
    while (out.length() && out[out.length() - 1] == '-') out.remove(out.length() - 1);
    return out;
}

void LampSettings::registerRoutes() {
    WebServer &server = configServer.getServer();

    // EasyWiFi registers /wifi* and a catch-all 404, but leaves the device root
    // to the application -- its own 404 page even advertises "/" as the device
    // home page. Without this, the bare hostname 404s.
    server.on("/", HTTP_GET, std::bind(&LampSettings::handleHome, this));
    server.on("/settings", HTTP_GET, std::bind(&LampSettings::handleSettingsGet, this));
    server.on("/settings", HTTP_POST, std::bind(&LampSettings::handleSettingsSave, this));
    server.on("/help", HTTP_GET, std::bind(&LampSettings::handleHelp, this));
    server.on("/reboot", HTTP_POST, std::bind(&LampSettings::handleReboot, this));

    // ===== REST API =====
    // Mutating calls are POST, never GET: a link a browser can prefetch should
    // not be able to switch a lamp off or reflash it.
    server.on("/api", HTTP_GET, std::bind(&LampSettings::handleApiIndex, this));
    server.on("/api/status", HTTP_GET, std::bind(&LampSettings::handleApiStatus, this));
    server.on("/api/power", HTTP_POST, std::bind(&LampSettings::handleApiPower, this));
    server.on("/api/brightness", HTTP_POST, std::bind(&LampSettings::handleApiBrightness, this));
    server.on("/api/identify", HTTP_POST, std::bind(&LampSettings::handleApiIdentify, this));
    server.on("/api/effects", HTTP_GET, std::bind(&LampSettings::handleApiEffects, this));
    server.on("/api/effect", HTTP_POST, std::bind(&LampSettings::handleApiEffect, this));
    server.on("/api/effect/reset", HTTP_POST, std::bind(&LampSettings::handleApiEffectReset, this));
    server.on("/api/name", HTTP_POST, std::bind(&LampSettings::handleApiName, this));
    server.on("/api/broker", HTTP_POST, std::bind(&LampSettings::handleApiBroker, this));
    server.on("/api/ota/check", HTTP_POST, std::bind(&LampSettings::handleApiOtaCheck, this));
    server.on("/api/ota/install", HTTP_POST, std::bind(&LampSettings::handleApiOtaInstall, this));
    server.on("/api/ota/update", HTTP_POST, std::bind(&LampSettings::handleApiOtaUpdate, this));
    server.on("/api/reboot", HTTP_POST, std::bind(&LampSettings::handleApiReboot, this));

    // Kept from 0.0.2, which shipped these paths and a find_devices.sh that
    // calls them. A lamp that has not been updated yet is still on the network
    // alongside one that has, and the scan has to work against both.
    server.on("/status.json", HTTP_GET, std::bind(&LampSettings::handleApiStatus, this));
    server.on("/ota/check", HTTP_POST, std::bind(&LampSettings::handleApiOtaCheck, this));
    server.on("/ota/install", HTTP_POST, std::bind(&LampSettings::handleApiOtaInstall, this));
}

// ===========================================================================
// Request parameters
// ===========================================================================
//
// A value may arrive three ways: a JSON body (what an agent or Home Assistant
// sends), a form field (the settings page), or a query parameter (the shortest
// thing to type into curl). All three are accepted everywhere, so no caller has
// to be told which style this lamp expects.
//
// ESP32's WebServer parses form and query parameters into args, and puts an
// unparsed body -- which is what a JSON request is -- into the "plain" arg.
namespace {

class Params {
public:
    explicit Params(WebServer &server) : server(server) {
        if (server.hasArg("plain")) {
            // A body that is not JSON is not an error here: it may simply be a
            // form post, whose fields are already in args.
            deserializeJson(doc, server.arg("plain"));
        }

        // A JSON body only arrives when the request says it is JSON. Curl's
        // default content type is application/x-www-form-urlencoded, and
        // WebServer believes the header: it runs the body through its form
        // parser, which drops any field without an '=' in it -- which is every
        // JSON document. The body is gone before a handler can see it, so
        // there is nothing to recover here.
        //
        // `curl -d '{"on":false}'` therefore sends nothing this API can read.
        // That is why every documented example either passes a query parameter
        // or sets the content type, and why a missing value is now an error
        // rather than a default: the silent version of this had POST
        // /api/power toggling instead of doing what the body said.
    }

    bool has(const char *key) const { return !doc[key].isNull() || server.hasArg(key); }

    String str(const char *key, const String &fallback = String()) const {
        if (!doc[key].isNull()) return doc[key].as<String>();
        if (server.hasArg(key)) return server.arg(key);
        return fallback;
    }

    long num(const char *key, long fallback) const {
        if (doc[key].is<long>()) return doc[key].as<long>();
        String s = str(key);
        return s.length() ? s.toInt() : fallback;
    }

    // JsonArrayConst, not JsonArray: this method is const, so doc[key] hands
    // back a const variant, and is<JsonArray>() asks whether it is a *mutable*
    // array -- which a const variant never is. It compiles, returns false for
    // every array, and sends the caller down the comma-separated-string path
    // with "[\"#ff0000\"]" in hand.
    bool isArray(const char *key) const { return doc[key].is<JsonArrayConst>(); }
    JsonArrayConst array(const char *key) const { return doc[key].as<JsonArrayConst>(); }

    // Accepts real booleans, 0/1, and the words people and shell scripts
    // actually type. Anything unrecognized leaves the fallback in place rather
    // than silently meaning "off".
    bool flag(const char *key, bool fallback) const {
        if (doc[key].is<bool>()) return doc[key].as<bool>();
        String s = str(key);
        s.toLowerCase();
        if (s == "1" || s == "true" || s == "on" || s == "yes") return true;
        if (s == "0" || s == "false" || s == "off" || s == "no") return false;
        return fallback;
    }

private:
    WebServer &server;
    JsonDocument doc;
};

}  // namespace

// ===========================================================================
// Status
// ===========================================================================

String LampSettings::statusJson(bool pretty) const {
    JsonDocument doc;

    doc["name"] = label;
    doc["hostname"] = host;
    doc["mdns"] = String(MDNS_HOSTNAME_HINT);
    doc["version"] = FIRMWARE_VERSION;

    // Home Assistant's light convention: a lamp that is off keeps the
    // brightness it will come back on at.
    doc["power"] = lampPower() ? "on" : "off";
    doc["on"] = lampPower();
    doc["brightness"] = lampBrightness();
    doc["color"] = lampColorHex();
    doc["identifying"] = lampIdentifying();

    JsonObject effect = doc["effect"].to<JsonObject>();
    effect["name"] = lampEffectName();
    effect["default"] = lampEffectIsDefault();
    // -1 rather than null for "not expiring", so a caller can compare a number
    // without first testing for absence.
    effect["expires_in"] = lampEffectExpiresIn();
    uint32_t rgb[MAX_COLORS];
    char hex[8];

    JsonArray colors = effect["colors"].to<JsonArray>();
    uint8_t count = lampEffectColors(rgb);
    for (uint8_t i = 0; i < count; i++) {
        snprintf(hex, sizeof(hex), "#%06lx", (unsigned long)rgb[i]);
        colors.add(hex);
    }

    // What was last chosen, which resetting does not change. The settings page
    // fills its pickers from this so "back to default" changes the lamp
    // without also wiping the palette sitting in front of you.
    JsonObject selected = effect["selected"].to<JsonObject>();
    selected["name"] = lampSelectedName();
    JsonArray chosen = selected["colors"].to<JsonArray>();
    count = lampSelectedColors(rgb);
    for (uint8_t i = 0; i < count; i++) {
        snprintf(hex, sizeof(hex), "#%06lx", (unsigned long)rgb[i]);
        chosen.add(hex);
    }

    bool online = WiFi.status() == WL_CONNECTED;
    JsonObject net = doc["network"].to<JsonObject>();
    net["online"] = online;
    net["ssid"] = online ? WiFi.SSID() : String();
    net["ip"] = online ? WiFi.localIP().toString() : String();
    net["rssi"] = online ? WiFi.RSSI() : 0;
    net["mac"] = WiFi.macAddress();

    JsonObject mqttObj = doc["mqtt"].to<JsonObject>();
    mqttObj["enabled"] = mqttEnabled();
    mqttObj["connected"] = mqttConnected();
    mqttObj["host"] = broker;
    mqttObj["port"] = brokerPort;

    JsonObject ota = doc["ota"].to<JsonObject>();
    ota["state"] = otaState();
    ota["latest"] = otaLatestTag();
    ota["available"] = otaUpdateAvailable();
    ota["checked"] = otaSecondsSinceCheck();
    // Error text comes from HTTPUpdate and the TLS stack, which are free to put
    // a quote or a backslash in it. ArduinoJson escapes it; the hand-rolled
    // version of this that 0.0.2 shipped had to do that itself.
    ota["error"] = otaLastError();

    doc["uptime"] = millis() / 1000;

    String out;
    if (pretty) {
        serializeJsonPretty(doc, out);
    } else {
        serializeJson(doc, out);
    }
    return out;
}

void LampSettings::sendStatus() {
    WebServer &server = configServer.getServer();
    // No-store: a cached status is worse than none, and some browsers will
    // happily serve one back for a bare GET on a small unchanging URL.
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", statusJson());
}

void LampSettings::handleApiStatus() {
    WebServer &server = configServer.getServer();
    server.sendHeader("Cache-Control", "no-store");
    // Pretty-printed when a person opens it in a browser, compact for the
    // pollers. `pretty=0` forces the compact form.
    bool pretty = server.hasArg("pretty") ? server.arg("pretty") != "0" : true;
    server.send(200, "application/json", statusJson(pretty));
}

// ===========================================================================
// REST API
// ===========================================================================

// The machine-readable counterpart of /help: an agent that finds a lamp can ask
// what it supports without being taught the endpoints beforehand.
void LampSettings::handleApiIndex() {
    WebServer &server = configServer.getServer();

    JsonDocument doc;
    doc["name"] = label;
    doc["hostname"] = host;
    doc["version"] = FIRMWARE_VERSION;
    doc["help"] = "/help";
    doc["mdns_service"] = "_" MDNS_SERVICE "._tcp";

    JsonArray eps = doc["endpoints"].to<JsonArray>();
    auto add = [&eps](const char *method, const char *path, const char *body, const char *what) {
        JsonObject e = eps.add<JsonObject>();
        e["method"] = method;
        e["path"] = path;
        if (body[0]) e["body"] = body;
        e["description"] = what;
    };
    add("GET", "/api", "", "This index.");
    add("GET", "/api/status", "", "Full lamp state. Add ?pretty=0 for compact JSON.");
    add("POST", "/api/power", "{\"on\": true | false | \"toggle\"}", "Switch the lamp on or off. Persists.");
    add("POST", "/api/brightness", "{\"value\": 0-255}", "Set brightness. Persists. Does not switch the lamp on.");
    add("POST", "/api/identify", "{\"seconds\": 1-60}", "Blink white so you can find this lamp.");
    add("GET", "/api/effects", "", "The effects this firmware can render, and the limits.");
    add("POST", "/api/effect",
        "{\"effect\": \"blend\", \"colors\": [\"#ff0000\"], \"seconds\": 300}",
        "Set the effect and palette. Reverts to the default when it expires.");
    add("POST", "/api/effect/reset", "", "Back to the default effect and palette now.");
    add("POST", "/api/name", "{\"name\": \"Living Room\", \"hostname\": \"glow-lamp\"}",
        "Rename. A changed hostname needs a reboot to take effect.");
    add("POST", "/api/ota/check", "", "Ask GitHub for the latest release. Installs nothing.");
    add("POST", "/api/ota/install", "", "Install the latest release, then reboot.");
    add("POST", "/api/ota/update", "",
        "Check, and install only if the release differs. Safe to call on a schedule.");
    add("POST", "/api/reboot", "", "Reboot the lamp.");

    doc["notes"]["responses"] =
        "Every mutating call returns the same object as GET /api/status, so the new state "
        "never needs a second request.";
    doc["notes"]["parameters"] =
        "Values are accepted as a JSON body, a form field, or a query parameter.";
    doc["notes"]["methods"] =
        "Mutating calls are POST only, so nothing a browser can prefetch changes the lamp.";

    String out;
    serializeJsonPretty(doc, out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

void LampSettings::handleApiPower() {
    WebServer &server = configServer.getServer();
    Params p(server);

    // Refused rather than defaulted. A missing or unparseable value used to
    // fall back to toggling, which meant a malformed request still changed the
    // lamp -- and looked like it had worked.
    if (!p.has("on")) {
        server.send(400, "application/json",
                    "{\"error\":\"send {\\\"on\\\": true | false | \\\"toggle\\\"}\"}");
        return;
    }

    // "toggle" is a string where the others are booleans, so it is checked
    // before flag() reduces the value to true/false.
    String raw = p.str("on");
    raw.toLowerCase();
    bool want = raw == "toggle" ? !lampPower() : p.flag("on", !lampPower());

    setLampPower(want);
    savePower(want);
    ESP_LOGI(TAG, "power %s over the API", want ? "on" : "off");
    sendStatus();
}

void LampSettings::handleApiBrightness() {
    WebServer &server = configServer.getServer();
    Params p(server);

    // "value" is the documented name; "brightness" is what anyone who read
    // status.json first will reach for.
    long v = p.has("value") ? p.num("value", bright) : p.num("brightness", bright);
    if (v < 0 || v > 255) {
        server.send(400, "application/json",
                    "{\"error\":\"brightness must be between 0 and 255\"}");
        return;
    }

    setLampBrightness((uint8_t)v);
    saveBrightness((uint8_t)v);
    ESP_LOGI(TAG, "brightness %ld over the API", v);
    sendStatus();
}

void LampSettings::handleApiIdentify() {
    Params p(configServer.getServer());

    uint32_t ms = IDENTIFY_DEFAULT_MS;
    if (p.has("seconds")) ms = (uint32_t)p.num("seconds", 4) * 1000;
    if (p.has("ms")) ms = (uint32_t)p.num("ms", (long)IDENTIFY_DEFAULT_MS);
    if (ms < 100) ms = 100;
    if (ms > IDENTIFY_MAX_MS) ms = IDENTIFY_MAX_MS;  // nobody meant to blink it for an hour

    identifyLamp(ms);
    ESP_LOGI(TAG, "identify for %lu ms", (unsigned long)ms);
    sendStatus();
}


// ===== Effects =====

// How long an effect lasts when the caller does not say, and the ceiling on
// what they can ask for. An effect is a thing someone is trying, not a new
// permanent state, so it times out by default rather than by request.
static const uint32_t EFFECT_DEFAULT_SECONDS = 300;      // 5 minutes
static const uint32_t EFFECT_MAX_SECONDS = 8UL * 60 * 60;  // 8 hours

// "#ff8800", "ff8800" or "0xff8800" -> 0xff8800. Returns false on anything
// else, including a short form: "#f80" is a CSS convenience this does not
// implement, and silently reading it as 0x000f80 would be worse than refusing.
static bool parseHexColor(const String &in, uint32_t &out) {
    String v = in;
    v.trim();
    if (v.startsWith("#")) v = v.substring(1);
    else if (v.startsWith("0x") || v.startsWith("0X")) v = v.substring(2);
    if (v.length() != 6) return false;

    uint32_t value = 0;
    for (unsigned i = 0; i < 6; i++) {
        char c = v[i];
        uint8_t digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

// What this firmware can render, so a caller does not have to guess at the
// names or discover the limits by being refused.
void LampSettings::handleApiEffects() {
    WebServer &server = configServer.getServer();

    JsonDocument doc;
    JsonArray list = doc["effects"].to<JsonArray>();
    // The ring shows one color at a time, so these differ in how that color
    // behaves over time, never in where it sits on the ring.
    const char *what[EFFECT_COUNT] = {
        "Holds on a color, then eases to the next.",
        "Walks the palette steadily, never resting on a color.",
        "Similar colors mixing and guttering, like a flame. The one effect that "
        "lights the ring several colors at once.",
        "One color, steady, with the stutter of a failing neon tube.",
    };
    for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
        JsonObject e = list.add<JsonObject>();
        e["name"] = EFFECT_NAMES[i];
        e["description"] = what[i];
    }
    doc["max_colors"] = MAX_COLORS;
    doc["default_seconds"] = EFFECT_DEFAULT_SECONDS;
    doc["max_seconds"] = EFFECT_MAX_SECONDS;
    doc["current"] = lampEffectName();

    String out;
    serializeJsonPretty(doc, out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

void LampSettings::handleApiEffect() {
    WebServer &server = configServer.getServer();
    Params p(server);

    // Name, not index: an integer here would be a number someone has to look
    // up, and would pin the wire format to the enum's order forever.
    String wanted = p.str("effect", p.str("name"));
    wanted.trim();
    wanted.toLowerCase();
    uint8_t mode = EFFECT_COUNT;
    for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
        if (wanted == EFFECT_NAMES[i]) {
            mode = i;
            break;
        }
    }
    if (mode == EFFECT_COUNT) {
        String names;
        for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
            if (i) names += ", ";
            names += EFFECT_NAMES[i];
        }
        server.send(400, "application/json",
                    "{\"error\":\"effect must be one of: " + names + "\"}");
        return;
    }

    // Colors arrive either as a JSON array or as repeated/comma-separated
    // form fields, so a browser form and a curl one-liner both work.
    uint32_t colors[MAX_COLORS];
    uint8_t count = 0;

    if (p.isArray("colors")) {
        for (JsonVariantConst v : p.array("colors")) {
            if (count >= MAX_COLORS) break;
            if (!parseHexColor(v.as<String>(), colors[count])) {
                server.send(400, "application/json",
                            "{\"error\":\"colors must be hex like #ff0000\"}");
                return;
            }
            count++;
        }
    } else if (p.has("colors")) {
        String list = p.str("colors");
        while (list.length() && count < MAX_COLORS) {
            int comma = list.indexOf(',');
            String one = comma < 0 ? list : list.substring(0, comma);
            list = comma < 0 ? String() : list.substring(comma + 1);
            one.trim();
            if (one.length() == 0) continue;
            if (!parseHexColor(one, colors[count])) {
                server.send(400, "application/json",
                            "{\"error\":\"colors must be hex like #ff0000\"}");
                return;
            }
            count++;
        }
    }

    // No colors given means "this effect, the palette it is already showing",
    // which is what makes switching effects from the UI a one-field call.
    if (count == 0) count = lampEffectColors(colors);

    uint32_t seconds = EFFECT_DEFAULT_SECONDS;
    if (p.has("seconds")) {
        long v = p.num("seconds", EFFECT_DEFAULT_SECONDS);
        if (v < 0) v = 0;
        // Clamped rather than refused: asking for a week is a reasonable way
        // to say "as long as you will let me".
        if ((uint32_t)v > EFFECT_MAX_SECONDS) v = EFFECT_MAX_SECONDS;
        seconds = (uint32_t)v;
    }

    if (!setLampEffect(mode, colors, count, seconds)) {
        server.send(400, "application/json",
                    "{\"error\":\"could not apply that effect\"}");
        return;
    }
    sendStatus();
}

void LampSettings::handleApiEffectReset() {
    ESP_LOGI(TAG, "effect reset to the default");
    resetLampEffect();
    sendStatus();
}

void LampSettings::handleApiName() {
    WebServer &server = configServer.getServer();
    Params p(server);

    String newLabel = p.str("name", label);
    newLabel.trim();
    if (newLabel.length() == 0) newLabel = label;
    if (newLabel.length() > 32) newLabel = newLabel.substring(0, 32);

    String newHost = host;
    if (p.has("hostname")) {
        String clean = sanitizeHost(p.str("hostname"));
        // A hostname that sanitizes away to nothing is a bad request, not a
        // reason to leave the lamp unreachable under a name nobody chose.
        if (clean.length() == 0) {
            server.send(400, "application/json",
                        "{\"error\":\"hostname must contain a letter or digit\"}");
            return;
        }
        newHost = clean;
    }

    bool hostChanged = newHost != host;
    saveNames(newLabel, newHost);
    ESP_LOGI(TAG, "renamed: label=%s host=%s%s", label.c_str(), host.c_str(),
             hostChanged ? " (reboot to publish)" : "");
    sendStatus();
}

// The broker password is never echoed back, here or anywhere: an empty one in
// a request means "leave it alone", so the host can be changed without knowing
// the password.
void LampSettings::handleApiBroker() {
    WebServer &server = configServer.getServer();
    Params p(server);

    String newHost = p.has("host") ? p.str("host") : broker;
    newHost.trim();

    long newPort = p.has("port") ? p.num("port", brokerPort) : brokerPort;
    if (newPort < 1 || newPort > 65535) {
        server.send(400, "application/json", "{\"error\":\"port must be 1-65535\"}");
        return;
    }

    String newUser = p.has("user") ? p.str("user") : brokerUser;
    String newPass = brokerPass;
    if (p.has("pass") && p.str("pass").length()) newPass = p.str("pass");

    saveBroker(newHost, (uint16_t)newPort, newUser, newPass);
    ESP_LOGI(TAG, "broker set to %s:%ld user=%s",
             newHost.length() ? newHost.c_str() : "(none)", newPort,
             newUser.length() ? newUser.c_str() : "(none)");
    sendStatus();
}

void LampSettings::handleApiOtaCheck() {
    ESP_LOGI(TAG, "update check requested over HTTP");
    requestOtaCheck();
    sendStatus();
}

void LampSettings::handleApiOtaInstall() {
    ESP_LOGI(TAG, "update install requested over HTTP (running %s, latest %s)", FIRMWARE_VERSION,
             otaLatestTag().length() ? otaLatestTag().c_str() : "unknown");
    requestOtaInstall();
    // Answered before the download starts, because once it does this device
    // stops serving anything until it reboots.
    sendStatus();
}

// One call for a scheduler. Check-then-install-if-different is what the lamp's
// own nightly timer does; this is the same thing on someone else's schedule, so
// Home Assistant can drive the rollout without a second request to decide.
//
// A lamp with nothing new to take does nothing at all, which is what makes this
// safe to fire at every lamp every night.
void LampSettings::handleApiOtaUpdate() {
    ESP_LOGI(TAG, "check-and-update requested over HTTP (running %s)", FIRMWARE_VERSION);
    requestOtaUpdate();
    sendStatus();
}

void LampSettings::handleApiReboot() {
    WebServer &server = configServer.getServer();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"rebooting\":true}");
    server.client().flush();
    delay(250);  // let the response reach the caller before the reset
    ESP.restart();
}

// ===========================================================================
// Pages
// ===========================================================================

// EasyWiFi's stylesheet covers .card, .form-group, .button and text inputs, but
// has nothing for <h2> (its own pages have no subheadings), for tables, or for
// a range slider. Those get rules here so these pages still look like the WiFi
// setup screens they sit next to.
static const char *EXTRA_CSS =
    "<style>"
    "h2{font-size:16px;margin:24px 0 4px;color:#333;"
    "text-transform:uppercase;letter-spacing:.04em;}"
    "h2:first-of-type{margin-top:8px;}"
    "hr{border:0;border-top:1px solid #eee;margin:24px 0;}"
    "code{background:#f0f0f0;padding:1px 4px;border-radius:4px;}"
    "pre{background:#f7f7f7;padding:12px;border-radius:8px;overflow-x:auto;"
    "font-size:13px;line-height:1.5;}"
    "pre code{background:none;padding:0;}"
    "table{width:100%;border-collapse:collapse;margin:8px 0 4px;}"
    "th,td{text-align:left;padding:8px 0;border-bottom:1px solid #eee;font-size:15px;}"
    "th{color:#666;font-weight:500;width:40%;}"
    "td{color:#333;}"
    "input[type=range]{width:100%;}"
    "#swatch{display:inline-block;width:14px;height:14px;border-radius:50%;"
    "margin-right:8px;vertical-align:-2px;border:1px solid rgba(0,0,0,.15);}"
    ".swatchbox{display:inline-block;text-align:center;margin:0 10px 8px 0;}"
    ".swatchbox input[type=color]{width:52px;height:38px;padding:0;border:1px solid #ddd;"
    "border-radius:8px;background:none;cursor:pointer;display:block;}"
    ".swatchbox label{font-size:12px;color:#999;font-weight:400;margin:4px 0 0;}"
    ".swatchbox label input{margin-right:3px;}"
    ".api{font-size:14px;margin:0 0 18px;}"
    ".api b{font-family:ui-monospace,Menlo,monospace;font-size:13px;}"
    "</style>";

String LampSettings::page(const String &title, const String &bodyHtml) {
    // getHTMLHeader() already closes </head> and opens <body>, so the page only
    // supplies the card itself.
    String html = webPages->getHTMLHeader(title);
    html += EXTRA_CSS;
    html += "<div class='card'>";
    html += "<h1>" + title + "</h1>";
    html += bodyHtml;
    html += "</div>";

    // Not webPages->getHTMLFooter(): that renders "<device> v0.1", a version
    // string hardcoded in EasyWiFi's WebPages.cpp that has never had anything
    // to do with this firmware. It read as the lamp's own version and was
    // wrong on every page. EasyWiFi's own /wifi pages still show it -- fixing
    // those means fixing the library.
    html += "<div class='footer'><small>";
    html += label + " &middot; " FIRMWARE_VERSION;
    html += "</small></div></body></html>";
    return html;
}

// One row of a status table. Kept out of the page builders so the markup for a
// missing value is written once.
static String row(const String &label, const String &value) {
    return "<tr><th>" + label + "</th><td>" + (value.length() ? value : String("&mdash;")) + "</td></tr>";
}

// Same, with an id on the value cell so the poller can rewrite it.
static String rowId(const String &label, const String &value, const char *id) {
    return "<tr><th>" + label + "</th><td id='" + id + "'>" +
           (value.length() ? value : String("&mdash;")) + "</td></tr>";
}

// The controls shared by the status page and the settings page: power,
// brightness and identify. Written once because both pages carry them -- the
// status page is where you land, the settings page is where you are when you
// need to tell one lamp from another.
//
// Every control calls the REST API and renders the state that comes back, so
// the page and a script driving the same lamp never disagree.
static String controlsHtml() {
    String b;
    b += "<h2>Light</h2>";
    b += "<div class='button-group'>";
    b += "<button id='powerbtn' class='button primary'>&hellip;</button>";
    b += "<button id='idbtn' class='button'>Identify</button>";
    b += "</div>";
    b += "<div class='form-group'>";
    b += "<label for='bslider'>Brightness <span id='bval'>&hellip;</span> / 255</label>";
    b += "<input type='range' id='bslider' min='0' max='255' value='0'>";
    b += "</div>";
    return b;
}

// The script behind those controls. Split from the markup only because both
// pages need both halves and the poller has to come after the elements exist.
static String controlsJs() {
    return String(
        "function post(path,body){"
        "return fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify(body||{})}).then(r=>r.json());"
        "}"
        // The slider fires continuously while dragging. Sending every step
        // would queue dozens of writes to NVS behind a lamp that answers one
        // request at a time, so it is only sent when the drag ends -- and the
        // label tracks the thumb in the meantime so it still feels live.
        "var slider=document.getElementById('bslider');"
        "var dragging=false;"
        "slider.addEventListener('input',function(){"
        "dragging=true;document.getElementById('bval').textContent=slider.value;});"
        "slider.addEventListener('change',function(){"
        "dragging=false;post('/api/brightness',{value:parseInt(slider.value,10)}).then(render);});"
        "document.getElementById('powerbtn').onclick=function(){"
        "post('/api/power',{on:'toggle'}).then(render);};"
        "document.getElementById('idbtn').onclick=function(){"
        "post('/api/identify',{seconds:4}).then(render);};"
        "function render(s){"
        "if(!s)return;"
        "var pb=document.getElementById('powerbtn');"
        "pb.textContent=s.on?'Turn off':'Turn on';"
        "pb.className='button '+(s.on?'primary':'');"
        "document.getElementById('idbtn').disabled=!!s.identifying;"
        // Not while dragging: overwriting the thumb under the finger makes the
        // slider fight back.
        "if(!dragging){slider.value=s.brightness;"
        "document.getElementById('bval').textContent=s.brightness;}"
        "}");
}

void LampSettings::handleHome() {
    WebServer &server = configServer.getServer();
    bool online = WiFi.status() == WL_CONNECTED;

    String b;
    b += "<p>A ring of 8 LEDs blending between three vibrant colors.</p>";

    b += controlsHtml();

    b += "<h2>Status</h2>";
    b += "<table>";
    b += row("Name", label);
    b += "<tr><th>Color</th><td><span id='swatch' style='background:" + lampColorHex() +
         "'></span><span id='color'>" + lampColorHex() + "</span></td></tr>";
    b += rowId("State", lampPower() ? "on" : "off", "state");
    b += rowId("Effect", lampEffectName(), "effect");
    b += rowId("MQTT", mqttEnabled() ? (mqttConnected() ? "connected" : "connecting") : "off",
               "mqtt");
    b += "</table>";

    b += "<h2>Network</h2>";
    b += "<table>";
    b += row("Hostname", host);
    b += row("mDNS", String(MDNS_HOSTNAME_HINT));
    b += row("WiFi", online ? WiFi.SSID() : String("not connected"));
    b += row("IP", online ? WiFi.localIP().toString() : String());
    b += rowId("Signal", online ? String(WiFi.RSSI()) + " dBm" : String(), "rssi");
    b += "</table>";

    b += "<h2>Firmware</h2>";
    b += "<table>";
    b += row("Running", FIRMWARE_VERSION);
    b += rowId("Latest release", "", "latest");
    b += "</table>";

    b += "<div class='button-group'>";
    b += "<a href='/settings' class='button'>Settings</a>";
    b += "<a href='/help' class='button'>API</a>";
    b += "<a href='/wifi' class='button'>WiFi Setup</a>";
    b += "</div>";

    // One poller drives the whole page. Failures are swallowed: a reboot or a
    // dropped link should leave the last known values on screen rather than
    // blanking the page.
    b += "<script>";
    b += controlsJs();
    b += "function u(){fetch('/api/status?pretty=0',{cache:'no-store'}).then(r=>r.json()).then(s=>{"
         "document.getElementById('color').textContent=s.color;"
         "document.getElementById('swatch').style.background=s.color;"
         "document.getElementById('state').textContent=s.power;"
         "document.getElementById('effect').textContent="
         "s.effect.name+(s.effect.default?'':' (reverting)');"
         "document.getElementById('mqtt').textContent="
         "s.mqtt.enabled?(s.mqtt.connected?'connected':'connecting'):'off';"
         "document.getElementById('rssi').textContent="
         "s.network.online?s.network.rssi+' dBm':'\\u2014';"
         "document.getElementById('latest').textContent=s.ota.latest?s.ota.latest:'\\u2014';"
         "render(s);"
         "}).catch(()=>{});}"
         "setInterval(u,1000);u();"
         "</script>";

    server.send(200, "text/html", page("Glow Lamp", b));
}

void LampSettings::handleSettingsGet() {
    String b;

    // Firmware first: it is the thing most likely to be wanted, and it is the
    // one control that changes what the lamp is rather than what it is doing.
    b += "<h2>Firmware</h2>";
    b += "<table>";
    b += row("Running", FIRMWARE_VERSION);
    b += rowId("Latest release", "", "latest");
    b += "</table>";
    b += "<div id='otabox'></div>";
    b += "<div class='button-group'>";
    b += "<button id='checkbtn' class='button'>Check for updates</button>";
    b += "</div>";

    b += "<hr>";
    b += controlsHtml();

    b += "<hr>";
    b += "<h2>Effect</h2>";
    b += "<div class='form-group'>";
    b += "<label for='effect'>Effect</label>";
    b += "<select id='effect'>";
    const char *what[EFFECT_COUNT] = {
        "Blend \u2014 holds a color, eases to the next",
        "Loop \u2014 walks the palette, never resting",
        "Flicker \u2014 mixes similar colors, like a flame",
        "Neon \u2014 steady, with the stutter of a failing tube",
    };
    for (uint8_t i = 0; i < EFFECT_COUNT; i++) {
        b += "<option value='" + String(EFFECT_NAMES[i]) + "'>" + what[i] + "</option>";
    }
    b += "</select>";
    b += "</div>";

    // Five pickers, each with a checkbox. A count field would make picking two
    // colors mean "the first two", so the colors you want have to be the first
    // ones in the row -- the checkbox lets any two be the two.
    b += "<div class='form-group'>";
    b += "<label>Colors <small style='display:inline;color:#999'>(up to 5)</small></label>";
    b += "<div id='swatches'>";
    for (uint8_t i = 0; i < MAX_COLORS; i++) {
        String n = String(i);
        b += "<span class='swatchbox'>";
        b += "<input type='color' id='c" + n + "'>";
        b += "<label><input type='checkbox' id='u" + n + "' checked> use</label>";
        b += "</span>";
    }
    b += "</div></div>";

    b += "<div class='form-group'>";
    b += "<label for='secs'>Revert after</label>";
    b += "<select id='secs'>";
    b += "<option value='300' selected>5 minutes</option>";
    b += "<option value='900'>15 minutes</option>";
    b += "<option value='1800'>30 minutes</option>";
    b += "<option value='3600'>1 hour</option>";
    b += "<option value='14400'>4 hours</option>";
    b += "<option value='28800'>8 hours</option>";
    b += "<option value='0'>until the lamp reboots</option>";
    b += "</select>";
    b += "<small>An effect is temporary on purpose: when it expires the lamp goes "
         "back to blending the five default colors. Nothing here survives a reboot.</small>";
    b += "</div>";

    b += "<div id='fxstate'></div>";
    b += "<div class='button-group'>";
    b += "<button id='applyfx' class='button primary'>Apply effect</button>";
    b += "<button id='resetfx' class='button'>Back to default</button>";
    b += "</div>";

    b += "<hr>";
    b += "<form method='POST' action='/settings'>";
    b += "<h2>Names</h2>";

    b += "<div class='form-group'>";
    b += "<label for='name'>Lamp name</label>";
    b += "<input type='text' id='name' name='name' value='" + label + "' maxlength='32' required>";
    b += "<small>What you call this lamp. Shown here and announced over mDNS so a "
         "scan can tell two lamps apart. Takes effect immediately.</small>";
    b += "</div>";

    b += "<div class='form-group'>";
    b += "<label for='hostname'>Hostname</label>";
    b += "<input type='text' id='hostname' name='hostname' value='" + host + "' maxlength='18' required>";
    b += "<small>Lowercase letters, digits and hyphens. The lamp answers at <code>" +
         host + ".local</code>. Give each lamp its own name: two with the same one "
         "would both reply to it. Needs a reboot.</small>";
    b += "</div>";

    b += "<div class='button-group'><button type='submit' class='button primary'>Save names</button></div>";
    b += "</form>";

    b += "<hr>";
    b += "<form method='POST' action='/settings'>";
    b += "<h2>Home Assistant</h2>";
    b += "<div id='mqttstate'></div>";
    b += "<div class='form-group'>";
    b += "<label for='mqtthost'>MQTT broker</label>";
    b += "<input type='text' id='mqtthost' name='mqtthost' value='" + broker +
         "' placeholder='192.168.1.10'>";
    b += "<small>Leave blank to turn MQTT off. With a broker set, the lamp announces "
         "itself to Home Assistant and appears as a light \u2014 no YAML, no restart.</small>";
    b += "</div>";

    b += "<div class='form-group'>";
    b += "<label for='mqttport'>Port</label>";
    b += "<input type='text' id='mqttport' name='mqttport' inputmode='numeric' "
         "pattern='[0-9]{1,5}' value='" + String(brokerPort) + "'>";
    b += "</div>";

    b += "<div class='form-group'>";
    b += "<label for='mqttuser'>Username</label>";
    b += "<input type='text' id='mqttuser' name='mqttuser' value='" + brokerUser +
         "' autocomplete='username'>";
    b += "</div>";

    // The stored password is never rendered back into the page. An empty submit
    // means "leave it alone", so the host can be changed without retyping it.
    b += "<div class='form-group'>";
    b += "<label for='mqttpass'>Password</label>";
    b += "<input type='password' id='mqttpass' name='mqttpass' autocomplete='new-password' "
         "placeholder='";
    b += brokerPass.length() ? "unchanged" : "not set";
    b += "'>";
    b += "<small>";
    b += brokerPass.length() ? "Leave blank to keep the stored password." : "No password set.";
    b += "</small>";
    b += "</div>";

    b += "<div class='button-group'><button type='submit' class='button primary'>"
         "Save broker</button></div>";
    b += "</form>";

    // Separate form: nesting it would submit the names too.
    b += "<hr>";
    b += "<form method='POST' action='/reboot' onsubmit='return confirm(\"Reboot the lamp?\")'>";
    b += "<div class='button-group'>";
    b += "<a href='/' class='button'>Home</a>";
    b += "<a href='/help' class='button'>API</a>";
    b += "<button type='submit' class='button danger'>Reboot</button>";
    b += "</div></form>";

    b += "<script>";
    b += controlsJs();
    b += "var fxTouched=false;"
         // Once someone starts choosing, the poller stops overwriting their
         // choices -- otherwise picking a color would be undone a second later
         // by whatever the lamp is currently showing.
         "document.getElementById('effect').onchange=function(){fxTouched=true;};"
         "document.getElementById('secs').onchange=function(){fxTouched=true;};"
         "for(var i=0;i<5;i++){"
         "document.getElementById('c'+i).onchange=function(){fxTouched=true;};"
         "document.getElementById('u'+i).onchange=function(){fxTouched=true;};"
         "}"
         "function fxRender(f){"
         "var box=document.getElementById('fxstate');"
         "if(f.default){box.innerHTML=\"<div class='status success'>Showing the default: \"+"
         "f.name+\"</div>\";}"
         "else if(f.expires_in<0){box.innerHTML=\"<div class='status warning'>\"+f.name+"
         "\", until the lamp reboots</div>\";}"
         "else{var m=Math.floor(f.expires_in/60),sec=f.expires_in%60;"
         "box.innerHTML=\"<div class='status warning'>\"+f.name+\", reverting in \"+"
         "(m?m+'m ':'')+sec+\"s</div>\";}"
         "if(fxTouched)return;"
         // From f.selected, not f.colors: the pickers show what you chose,
         // which is not always what the lamp is showing.
         "var sel=f.selected;"
         "document.getElementById('effect').value=sel.name;"
         "for(var i=0;i<5;i++){"
         "var has=i<sel.colors.length;"
         "document.getElementById('u'+i).checked=has;"
         "if(has)document.getElementById('c'+i).value=sel.colors[i];"
         "}"
         "}"
         "document.getElementById('applyfx').onclick=function(){"
         "var colors=[];"
         "for(var i=0;i<5;i++){"
         "if(document.getElementById('u'+i).checked)colors.push(document.getElementById('c'+i).value);"
         "}"
         "if(!colors.length){alert('Pick at least one color.');return;}"
         "post('/api/effect',{effect:document.getElementById('effect').value,colors:colors,"
         "seconds:parseInt(document.getElementById('secs').value,10)})"
         ".then(function(s){fxTouched=false;render(s);fxRender(s.effect);});"
         "};"
         "document.getElementById('resetfx').onclick=function(){"
         "post('/api/effect/reset',{}).then(function(s){fxTouched=false;render(s);fxRender(s.effect);});"
         "};"
         "var installing=false;"
         "function esc(t){var d=document.createElement('div');d.textContent=t;return d.innerHTML;}"
         "function otaBox(o){"
         "var box=document.getElementById('otabox'),btn=document.getElementById('checkbtn');"
         "document.getElementById('latest').textContent=o.latest?o.latest:'\\u2014';"
         "if(installing)return;"
         "btn.disabled=(o.state=='checking');"
         "btn.textContent=o.state=='checking'?'Checking\\u2026':'Check for updates';"
         "if(o.state=='error'&&o.error){"
         "box.innerHTML=\"<div class='status warning'>Check failed: \"+esc(o.error)+\"</div>\";return;}"
         "if(o.available){"
         "box.innerHTML=\"<div class='status warning'>Version \"+esc(o.latest)+\" is available.</div>\"+"
         "\"<div class='button-group'><button id='upbtn' class='button primary'>Update to \"+esc(o.latest)+\"</button></div>\";"
         "document.getElementById('upbtn').onclick=install;return;}"
         "if(o.checked>=0){box.innerHTML=\"<div class='status success'>Up to date.</div>\";return;}"
         "box.innerHTML='';"
         "}"
         "function install(){"
         "if(!confirm('Download and install the latest firmware? The lamp reboots when it finishes.'))return;"
         "installing=true;"
         "document.getElementById('checkbtn').disabled=true;"
         "document.getElementById('otabox').innerHTML="
         "\"<div class='loading'></div><div class='status'>Downloading and installing. The lamp stops \"+"
         "\"answering for a minute, then reboots on the new version. This page recovers on its own.</div>\";"
         "post('/api/ota/install',{}).catch(()=>{});"
         "setTimeout(function(){location.reload();},45000);"
         "}"
         "document.getElementById('checkbtn').onclick=function(){"
         "document.getElementById('checkbtn').disabled=true;"
         "document.getElementById('checkbtn').textContent='Checking\\u2026';"
         "post('/api/ota/check',{}).catch(()=>{});};"
         "function mqttBox(m){"
         "var box=document.getElementById('mqttstate');"
         "if(!m.enabled){box.innerHTML=\"<div class='status'>MQTT is off. The lamp works "
         "over its REST API.</div>\";return;}"
         "box.innerHTML=m.connected"
         "?\"<div class='status success'>Connected to \"+m.host+\"</div>\""
         ":\"<div class='status warning'>Not connected to \"+m.host+\"</div>\";"
         "}"
         "function u(){fetch('/api/status?pretty=0',{cache:'no-store'}).then(r=>r.json()).then(s=>{"
         "render(s);otaBox(s.ota);fxRender(s.effect);mqttBox(s.mqtt);"
         "}).catch(()=>{});}"
         "setInterval(u,1000);u();"
         "</script>";

    configServer.getServer().send(200, "text/html", page("Settings", b));
}

void LampSettings::handleSettingsSave() {
    WebServer &server = configServer.getServer();

    String newLabel = server.hasArg("name") ? server.arg("name") : label;
    newLabel.trim();
    if (newLabel.length() == 0) newLabel = label;
    if (newLabel.length() > 32) newLabel = newLabel.substring(0, 32);

    String newHost = host;
    if (server.hasArg("hostname")) {
        String clean = sanitizeHost(server.arg("hostname"));
        if (clean.length()) newHost = clean;
    }

    bool hostChanged = newHost != host;
    saveNames(newLabel, newHost);

    // The settings page carries two forms that both post here, and only one is
    // ever submitted at a time -- so the broker is written only when its fields
    // are actually present, and the names form does not blank the broker.
    if (server.hasArg("mqtthost")) {
        String bHost = server.arg("mqtthost");
        bHost.trim();
        uint16_t bPort = brokerPort;
        if (server.hasArg("mqttport")) {
            long v = server.arg("mqttport").toInt();
            if (v > 0 && v <= 65535) bPort = (uint16_t)v;
        }
        String bUser = server.hasArg("mqttuser") ? server.arg("mqttuser") : brokerUser;
        String bPass = brokerPass;
        if (server.hasArg("mqttpass") && server.arg("mqttpass").length()) {
            bPass = server.arg("mqttpass");
        }
        saveBroker(bHost, bPort, bUser, bPass);
    }

    ESP_LOGI(TAG, "saved: label=%s host=%s", label.c_str(), host.c_str());

    String b;
    b += "<div class='status success'>Saved</div>";
    if (hostChanged) {
        b += "<div class='status warning'>Hostname is now <b>" + host +
             "</b>. Reboot for it to take effect.</div>";
        b += "<form method='POST' action='/reboot'><div class='button-group'>"
             "<button type='submit' class='button danger'>Reboot now</button></div></form>";
    }
    b += "<div class='button-group'><a href='/settings' class='button primary'>Back to settings</a></div>";

    server.send(200, "text/html", page("Settings", b));
}

void LampSettings::handleReboot() {
    WebServer &server = configServer.getServer();

    // Where to send the browser once the device answers again. Same origin
    // normally; after a rename the old .local name stops resolving, so the page
    // has to offer the new one instead of polling an address that is gone.
    String target = expectedHostname();

    String b;
    b += "<div class='loading'></div>";
    b += "<p id='msg' style='text-align:center'>Rebooting, waiting for the lamp to come back...</p>";
    b += "<div id='fallback' style='display:none'>";
    b += "<div class='status warning'>Still not answering. It may have come back "
         "under a new name.</div>";
    b += "<div class='button-group'><a href='http://" + target + "/' class='button primary'>"
         "Open http://" + target + "/</a></div>";
    b += "</div>";

    // Give it a moment to actually go down first: polling immediately would hit
    // the still-running server and "succeed" before the reset even happens.
    // 90 attempts, not 30: the device itself is back in ~10s, but the host's
    // mDNS cache can hold the stale record for a good while longer, so the
    // browser cannot resolve the .local name even though the lamp is answering.
    // Giving up early sends people to a fallback link that would have worked.
    b += "<script>"
         "var n=0,max=90;"
         "function poll(){"
         "n++;"
         "document.getElementById('msg').textContent="
         "'Rebooting, waiting for the lamp to come back... ('+n+'/'+max+')';"
         "fetch('/api/status?pretty=0',{cache:'no-store'})"
         ".then(r=>{if(r.ok){location.href='/';}else{again();}})"
         ".catch(()=>again());"
         "}"
         "function again(){"
         "if(n>=max){document.getElementById('msg').style.display='none';"
         "document.getElementById('fallback').style.display='block';"
         "document.querySelector('.loading').style.display='none';return;}"
         "setTimeout(poll,1000);"
         "}"
         "setTimeout(poll,4000);"
         "</script>";

    server.send(200, "text/html", page("Rebooting", b));
    server.client().flush();
    delay(250);  // let the response reach the browser before the reset
    ESP.restart();
}

String LampSettings::expectedHostname() const { return host + ".local"; }

// The human- and agent-readable API reference, served by the lamp itself so it
// travels with the firmware and cannot describe a version that is not running.
// GET /api is the same thing as JSON.
//
// Everything here is literal: an agent reading this page should be able to
// drive the lamp without guessing at a parameter name or a value format.
void LampSettings::handleHelp() {
    WebServer &server = configServer.getServer();
    String base = "http://" + String(MDNS_HOSTNAME_HINT);
    if (String(MDNS_HOSTNAME_HINT).length() == 0) base = "http://" + WiFi.localIP().toString();

    // One endpoint's entry. Written as a helper so the page and its markup stay
    // in step as endpoints are added.
    auto ep = [](const char *method, const char *path, const char *body, const String &what) {
        String s = "<p class='api'><b>" + String(method) + " " + String(path) + "</b><br>" + what;
        if (body[0]) s += "<br><code>" + String(body) + "</code>";
        s += "</p>";
        return s;
    };

    String b;
    b += "<p>This lamp is controlled over HTTP. Every mutating call answers with the "
         "same object <code>GET /api/status</code> returns, so the new state never needs "
         "a second request.</p>";

    b += "<h2>Conventions</h2>";
    b += "<p class='api'>Values may be sent as a query parameter, a form field, or a "
         "JSON body &mdash; but a JSON body is only read when the request sets "
         "<code>Content-Type: application/json</code>. Curl's default content type is "
         "form-encoded, and a JSON document sent that way is discarded by the HTTP "
         "server before this firmware sees it, so <code>-d '{&quot;on&quot;:false}'</code> "
         "alone sends nothing. Query parameters never have that problem.</p>";
    b += "<p class='api'>Anything that changes the lamp is <b>POST</b> only, so nothing a "
         "browser can prefetch can switch a lamp off or reflash it. A missing or "
         "unreadable value is refused with a 400 rather than defaulted. There is no "
         "authentication: these lamps are LAN devices and anything that can reach one can "
         "control it.</p>";

    b += "<h2>Finding a lamp</h2>";
    b += "<p class='api'>Lamps announce <b>_" MDNS_SERVICE "._tcp</b> over mDNS on port 80. "
         "The TXT records carry the lamp's name, hostname and firmware version. "
         "<code>scripts/glowlamp.py discover</code> in the project repo does this and "
         "prints one line per lamp.</p>";

    b += "<h2>Endpoints</h2>";
    b += ep("GET", "/api", "", "This reference as JSON, including the endpoint list.");
    b += ep("GET", "/api/status", "",
            "Full state: name, version, power, brightness, color, network and update "
            "status. Add <code>?pretty=0</code> for compact JSON.");
    b += ep("POST", "/api/power", "{\"on\": true}",
            "Switch the lamp on or off. Accepts <code>true</code>, <code>false</code> or "
            "<code>\"toggle\"</code>; also <code>on/off</code>, <code>1/0</code>, "
            "<code>yes/no</code>. Persists across a reboot.");
    b += ep("POST", "/api/brightness", "{\"value\": 128}",
            "Set brightness, 0&ndash;255. Persists. Independent of power: setting it on a "
            "lamp that is off changes what it comes back on at.");
    b += ep("POST", "/api/identify", "{\"seconds\": 4}",
            "Blink white for a moment so you can tell which lamp this is. Overrides power "
            "and restores whatever was showing, so identifying a lamp that is off leaves "
            "it off. 1&ndash;60 seconds.");
    b += ep("GET", "/api/effects", "",
            "The four effects this firmware can render, with the color and duration "
            "limits. The ring shows one color at a time, so effects differ in how that "
            "color changes over time, not in where it sits on the ring.");
    b += ep("POST", "/api/effect",
            "{\"effect\": \"blend\", \"colors\": [\"#ff0000\"], \"seconds\": 300}",
            "Set the effect and up to five colors. <code>colors</code> may be omitted to "
            "keep the current palette. <code>seconds</code> defaults to 300 and is capped "
            "at 28800 (8 hours); 0 means until the lamp reboots. When it expires the lamp "
            "returns to blending its five default colors.");
    b += ep("POST", "/api/effect/reset", "", "Back to the default effect and palette now.");
    b += ep("POST", "/api/name", "{\"name\": \"Living Room\", \"hostname\": \"glow-lamp\"}",
            "Rename. <code>name</code> is free text and takes effect immediately; "
            "<code>hostname</code> is lowercase letters, digits and hyphens and becomes "
            "<code>&lt;hostname&gt;.local</code>, and needs a reboot because mDNS has "
            "already published the old one.");
    b += ep("POST", "/api/ota/check", "",
            "Ask GitHub for the latest release. Installs nothing &mdash; read the result "
            "from <code>ota</code> in the status a second or two later.");
    b += ep("POST", "/api/ota/install", "",
            "Install the latest release and reboot. The lamp answers first, then stops "
            "responding for 10&ndash;30 s while it downloads.");
    b += ep("POST", "/api/ota/update", "",
            "Check, and install only if the latest release differs from what is running. "
            "One call, nothing to decide on the caller's side, and a no-op when there is "
            "nothing new &mdash; this is the one to put on a nightly schedule.");
    b += ep("POST", "/api/broker",
            "{\"host\": \"192.168.1.10\", \"port\": 1883, \"user\": \"\", \"pass\": \"\"}",
            "Set the MQTT broker. An empty host turns MQTT off. An empty password leaves "
            "the stored one alone. Takes effect without a reboot.");
    b += ep("POST", "/api/reboot", "", "Reboot the lamp.");

    b += "<h2>Examples</h2>";
    b += "<pre><code># what is this lamp doing\n";
    b += "curl " + base + "/api/status\n\n";
    b += "# off, on, half brightness -- query parameters need no header\n";
    b += "curl -X POST '" + base + "/api/power?on=false'\n";
    b += "curl -X POST '" + base + "/api/power?on=true'\n";
    b += "curl -X POST '" + base + "/api/brightness?value=128'\n\n";
    b += "# which one is this?\n";
    b += "curl -X POST '" + base + "/api/identify?seconds=4'\n\n";
    b += "# an effect, with colors (the # may be left off in a URL)\n";
    b += "curl -X POST '" + base + "/api/effect?effect=neon&colors=ff0000,00ff00,0000ff'\n\n";
    b += "# a JSON body works, but ONLY with the content type set --\n";
    b += "# curl's default is form-encoded, and this API cannot read that\n";
    b += "curl -X POST -H 'Content-Type: application/json' \\\n";
    b += "  -d '{\"effect\":\"blend\",\"colors\":[\"#ff0000\",\"#0000ff\"],\"seconds\":600}' \\\n";
    b += "  " + base + "/api/effect\n\n";
    b += "# nightly: take a new release if there is one, do nothing if not\n";
    b += "curl -X POST " + base + "/api/ota/update</code></pre>";

    b += "<h2>Home Assistant</h2>";
    b += "<p class='api'>With a broker configured the lamp publishes a retained discovery "
         "config to <b>homeassistant/light/&lt;id&gt;/config</b> and becomes a light entity "
         "on its own &mdash; no YAML and no restart. It subscribes to "
         "<b>glowlamp/&lt;hostname&gt;/set</b> (Home Assistant's JSON light schema: state, "
         "brightness, color, effect), publishes retained state to "
         "<b>glowlamp/&lt;hostname&gt;/state</b>, and carries a last will on "
         "<b>glowlamp/&lt;hostname&gt;/availability</b> so the entity goes unavailable when "
         "the lamp drops off.</p>";
    b += "<p class='api'>Anything set over MQTT has no expiry: Home Assistant is a "
         "controller, and an effect that reverted after five minutes would leave the entity "
         "showing a state the lamp no longer has. MQTT is optional &mdash; a lamp with no "
         "broker is fully usable here.</p>";

    b += "<h2>Status fields</h2>";
    b += "<table>";
    b += row("name", "What this lamp is called.");
    b += row("hostname", "The mDNS label; the lamp answers at <code>&lt;hostname&gt;.local</code>.");
    b += row("version", "Firmware version, matching a GitHub release tag.");
    b += row("on / power", "Boolean and the same thing as <code>\"on\"</code> or <code>\"off\"</code>.");
    b += row("brightness", "0&ndash;255, what it shows at when on.");
    b += row("color", "The color the ring is on right now, <code>#rrggbb</code>.");
    b += row("identifying", "True while the identify flash is running.");
    b += row("network", "online, ssid, ip, rssi, mac.");
    b += row("ota", "state, latest, available, checked (seconds ago, -1 if never), error.");
    b += row("uptime", "Seconds since boot.");
    b += "</table>";

    b += "<div class='button-group'>";
    b += "<a href='/' class='button'>Home</a>";
    b += "<a href='/settings' class='button'>Settings</a>";
    b += "<a href='/api' class='button'>/api as JSON</a>";
    b += "</div>";

    server.send(200, "text/html", page("REST API", b));
}
