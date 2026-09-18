#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <CustomPageHandler.h>  // not pulled in by EasyWiFi.h
#include <EasyWiFi.h>

// Everything the lamp persists, the pages that edit it, and the REST API that
// drives it from a script. All of it hangs off EasyWiFi's web server, so it is
// reachable from the setup AP as well as from the device's address once it has
// joined a network.
//
// Config.h supplies the defaults a freshly flashed board uses until someone
// saves a form or calls the API.
class LampSettings : public CustomPageHandler {
public:
    explicit LampSettings(ConfigServer &cs) : configServer(cs) {}

    // Load stored values (falling back to Config.h). Call once after
    // RunLoop::setup().
    void begin();

    // CustomPageHandler: add our routes to EasyWiFi's server.
    void registerRoutes() override;

    // ===== Stored state =====

    // The network label: EasyWiFi builds the mDNS hostname and the setup AP's
    // SSID from it, both suffixed with the MAC. Lowercase, alphanumeric and
    // hyphens. Changing it needs a reboot, since mDNS has already published by
    // the time the form is submitted.
    const String &hostName() const { return host; }

    // What a person calls this lamp -- "Living Room", "Desk". Free text, shown
    // in the UI and announced over mDNS so a scan can tell two lamps apart
    // without anyone decoding a MAC suffix. Purely cosmetic: nothing on the
    // network is addressed by it.
    const String &displayName() const { return label; }

    uint8_t brightness() const { return bright; }
    bool power() const { return on; }

    // Each of these writes through to NVS immediately. The lamp is switched by
    // Home Assistant, and an off that does not survive a power cut is a lamp
    // that comes back on by itself in the middle of the night.
    void saveBrightness(uint8_t value);
    void savePower(bool value);
    void saveNames(const String &newLabel, const String &newHost);

private:
    ConfigServer &configServer;

    String host;
    String label;
    uint8_t bright = 64;
    bool on = true;

    // ===== Pages =====
    void handleHome();
    void handleSettingsGet();
    void handleSettingsSave();
    void handleHelp();
    void handleReboot();

    // ===== REST API =====
    // Every mutating call answers with the same object GET /api/status returns,
    // so a caller never has to make a second request to learn the new state.
    void handleApiIndex();
    void handleApiStatus();
    void handleApiPower();
    void handleApiBrightness();
    void handleApiIdentify();
    void handleApiName();
    void handleApiOtaCheck();
    void handleApiOtaInstall();
    void handleApiOtaUpdate();
    void handleApiReboot();

    // Serialize the full lamp state. `pretty` is for a human reading /api/status
    // in a browser; the UI poller takes the compact form.
    String statusJson(bool pretty = false) const;
    void sendStatus();

    // Wraps body content in EasyWiFi's card + header/footer chrome so these
    // pages match the WiFi setup screens.
    String page(const String &title, const String &bodyHtml);

    // "<hostname>-<mac>.local" for the name currently configured.
    String expectedHostname() const;

    // EasyWiFi puts this into an SSID and an mDNS label, so it has to survive
    // both: lowercase, alphanumeric and hyphen only, and short enough that
    // "-Setup-XXXXXX" still fits inside the 32-character SSID limit.
    static String sanitizeHost(const String &in);
};

extern LampSettings settings;

#endif  // SETTINGS_H
