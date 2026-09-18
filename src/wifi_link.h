#ifndef WIFI_LINK_H
#define WIFI_LINK_H

// WiFi is owned entirely by EasyWiFi. An unprovisioned board raises its own
// access point with a captive portal, takes the SSID and password there, and
// stores them in NVS. Credentials are never compiled into the firmware, so a
// release binary is safe to publish and any board can be re-provisioned in the
// field without a reflash.
//
// There is deliberately no hard-coded fallback path.

// Bonjour/mDNS service type the lamp announces once it is on the network, so
// scripts/find_devices.sh can locate it without knowing its address. Announced
// as _glowlamp._tcp on port 80; the TXT records carry the device name, firmware
// version, and the path to the Settings page.
#define MDNS_SERVICE "glowlamp"

// The .local name EasyWiFi published, e.g. "glow-lamp-d0ffdc.local".
// Empty until mDNS has started.
#define MDNS_HOSTNAME_HINT mdnsHostname()
const char *mdnsHostname();

void setupWifiLink();
void loopWifiLink();

#endif  // WIFI_LINK_H
