#include "wifi_link.h"

#include <Config.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <mdns.h>  // mdns_hostname_set(), which ESPmDNS does not expose
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

    // The DHCP hostname, so the router's lease table shows "castor-lamp"
    // rather than "espressif". Has to be set before the join, which EasyWiFi
    // performs inside its own loop.
    WiFi.setHostname(settings.hostName().c_str());
    runloop.setup(settings.hostName());
    runloop.getConfigServer().registerCustomHandler(&settings);

    uint8_t mac[6];
    WiFi.macAddress(mac);
    ESP_LOGI(TAG, "EasyWiFi up, MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
}

void loopWifiLink() {
    runloop.loop();

    // EasyWiFi starts mDNS itself, under a hostname it builds as
    // "<name>-<last 3 bytes of MAC>" -- WiFiManager::startMDNS() hardcodes that
    // format and takes no say in it. It is the right default for a device that
    // ships unnamed, and the wrong one for a lamp someone has deliberately
    // called "castor-lamp": the suffix is then a second thing to type and to
    // remember, guarding against a collision that naming the lamps already
    // prevents.
    //
    // The obvious fix -- MDNS.end() then MDNS.begin(hostName()) -- does not
    // hold. This runs the moment WL_CONNECTED appears, which is BEFORE
    // RunLoop reaches its CONNECTED state and calls startMDNS(); EasyWiFi then
    // overwrites the hostname a second later and the lamp answers to the
    // suffixed name again. It looked like MDNS.begin() had failed. It had not:
    // it ran too early, and end() also threw away the _http._tcp service
    // EasyWiFi's own pages link to.
    //
    // So the host record is renamed through the IDF call underneath instead,
    // which leaves the service list alone, and it is re-asserted a few times
    // over the following half minute so that whichever order the two run in,
    // this one lands last. RunLoop calls startMDNS() exactly once, so this
    // settles rather than fighting forever.
    //
    // Two lamps given the same name would both answer to it, and nothing here
    // would notice: the ESP32 responder does not report a conflict back, so
    // EasyWiFi's own retry loop never fires either. Distinct names are the
    // whole defence, which is why the settings page asks for one.
    static bool announced = false;
    static uint32_t nextTry = 0;
    if (!announced && WiFi.status() == WL_CONNECTED && millis() - nextTry >= 2000) {
        nextTry = millis();
        if (MDNS.addService(MDNS_SERVICE, "tcp", 80)) {
            // What a scan reads without opening a connection: the friendly
            // name is here so two lamps can be told apart from the browse list
            // alone, rather than by decoding a MAC suffix.
            MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "device", settings.hostName().c_str());
            MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "name", settings.displayName().c_str());
            MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "fw", FIRMWARE_VERSION);
            MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "api", "/api");
            MDNS.addServiceTxt(MDNS_SERVICE, "tcp", "help", "/help");
            announced = true;
            mdnsName = settings.hostName() + ".local";
            ESP_LOGI(TAG, "announced _%s._tcp as %s", MDNS_SERVICE, mdnsName.c_str());
        }
    }

    // The rename, and the re-assertions. The instance name goes with it, so a
    // browse list shows "castor-lamp" rather than the name the service was
    // first registered under.
    //
    // The first assertion is immediate, not on the 5-second beat: until it
    // runs the lamp genuinely answers to EasyWiFi's suffixed name, and a boot
    // is exactly when someone is most likely to be looking for it.
    static uint8_t asserts = 0;
    static uint32_t nextAssert = 0;
    if (announced && asserts < 7 && (asserts == 0 || millis() - nextAssert >= 5000)) {
        nextAssert = millis();
        asserts++;
        mdns_hostname_set(settings.hostName().c_str());
        MDNS.setInstanceName(settings.hostName());
    }
}
