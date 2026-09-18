#include "wifi_link.h"

#include <Config.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_log.h>

#include "settings.h"
#include "version.h"

static const char *TAG = "WIFI";

// RunLoop is EasyWiFi's state machine: it loads stored credentials, connects,
// and falls back to AP + captive portal when there are none or the join fails.
static RunLoop runloop;

// The Settings page hangs off EasyWiFi's web server, so it is reachable both
// from the setup AP and from the device's address once it has joined.
LampSettings settings(runloop.getConfigServer());

// Filled in when the service is announced, so the home page can show the name
// the device is actually reachable by rather than recomputing it.
static String mdnsName;

const char *mdnsHostname() { return mdnsName.c_str(); }

void setupWifiLink() {
    // Bring the WiFi driver up BEFORE EasyWiFi runs. Two things depend on it:
    //
    //  - WiFiManager::generateAPSSID() builds the setup SSID from
    //    WiFi.macAddress(), which returns 00:00:00 until the driver is
    //    initialized. Without this the AP is "<name>-Setup-000800" on every
    //    board -- identical, and useless for telling two apart.
    //  - startAccessPointEx() calls WiFi.disconnect(), which logs
    //    "STA not started! You must call begin first." for the same reason.
    //
    // Both are EasyWiFi bugs worth fixing there; this makes them go away without
    // forking the library.
    WiFi.mode(WIFI_STA);

    // Settings first: EasyWiFi derives both the mDNS hostname and the setup AP
    // SSID from the name passed to setup(), so the stored one has to be loaded
    // before that call rather than after.
    settings.begin();
    runloop.setup(settings.deviceName());
    runloop.getConfigServer().registerCustomHandler(&settings);

    uint8_t mac[6];
    WiFi.macAddress(mac);
    ESP_LOGI(TAG, "EasyWiFi up, MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
}

void loopWifiLink() {
    runloop.loop();

    // EasyWiFi starts mDNS and announces _http._tcp once connected. Add our own
    // service on top so the lamp is findable by what it *is* rather than by
    // being one of every web server on the LAN -- see scripts/find_devices.sh.
    //
    // Retry rather than fire once on WL_CONNECTED: the STA link comes up a beat
    // before RunLoop reaches its CONNECTED state and calls MDNS.begin(), and
    // addService() fails outright if the responder is not running yet.
    static bool announced = false;
    static uint32_t nextTry = 0;
    if (!announced && WiFi.status() == WL_CONNECTED && millis() - nextTry >= 2000) {
        nextTry = millis();
        if (MDNS.addService(MDNS_SERVICE, "tcp", 80)) {
            MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "device", settings.deviceName().c_str());
            MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "fw", FIRMWARE_VERSION);
            MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "settings", "/settings");
            announced = true;
            uint8_t mac[6];
            WiFi.macAddress(mac);
            char buf[48];
            snprintf(buf, sizeof(buf), "%s-%02x%02x%02x.local", settings.deviceName().c_str(),
                     mac[3], mac[4], mac[5]);
            mdnsName = buf;
            ESP_LOGI(TAG, "announced _%s._tcp as %s", MDNS_SERVICE, mdnsName.c_str());
        }
    }
}
