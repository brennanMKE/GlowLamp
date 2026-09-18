#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <CustomPageHandler.h>  // not pulled in by EasyWiFi.h
#include <EasyWiFi.h>

// A Settings page served by EasyWiFi's web server at /settings, so the device
// name and brightness are set on the device instead of compiled into it. Values
// persist in the same NVS namespace the OTA scheduler uses.
//
// Config.h still supplies the defaults, which is what a freshly flashed board
// uses until someone saves the form.
class LampSettings : public CustomPageHandler {
public:
    explicit LampSettings(ConfigServer &cs) : configServer(cs) {}

    // Load stored values (falling back to Config.h). Call once after
    // RunLoop::setup().
    void begin();

    // CustomPageHandler: add our routes to EasyWiFi's server.
    void registerRoutes() override;

    // The device name: EasyWiFi builds the mDNS hostname and the setup AP's
    // SSID from it, both suffixed with the MAC. Changing it needs a reboot,
    // since mDNS has already published by the time the form is submitted.
    const String &deviceName() const { return name; }

    // 0-255. The stored value, which main.cpp applies at boot; the slider
    // writes through to FastLED immediately and persists here.
    uint8_t brightness() const { return bright; }

private:
    ConfigServer &configServer;

    String name;
    uint8_t bright = 64;

    void handleHome();
    void handleStatusJson();
    void handleGet();
    void handleSave();
    void handleOtaCheck();
    void handleOtaInstall();
    void handleReboot();

    // Wraps body content in EasyWiFi's card + header/footer chrome so these
    // pages match the WiFi setup screens.
    String page(const String &title, const String &bodyHtml);

    // "<name>-<mac>.local" for the name currently configured.
    String expectedHostname() const;

    // EasyWiFi puts this into an SSID and an mDNS label, so it has to survive
    // both: lowercase, alphanumeric and hyphen only, and short enough that
    // "-Setup-XXXXXX" still fits inside the 32-character SSID limit.
    static String sanitizeName(const String &in);
};

extern LampSettings settings;

#endif  // SETTINGS_H
