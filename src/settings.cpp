#include "settings.h"

#include <Config.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_log.h>

#include "lamp.h"
#include "ota.h"
#include "version.h"
#include "wifi_link.h"

static const char *TAG = "SETTINGS";

// Shared with the OTA scheduler's ota_yday key -- one namespace for everything
// this firmware persists.
static const char *NVS_NS = "glowlamp";

void LampSettings::begin() {
    Preferences p;
    // Read-write, not read-only: opening a namespace that does not exist yet
    // read-only fails with NOT_FOUND. Read-write creates it, so a first boot is
    // silent instead of logging an error before falling back to the defaults.
    p.begin(NVS_NS, false);
    // isKey() before each read: getString() on a missing key logs an ESP_LOGE
    // before returning the default, which would make every first boot look like
    // a failure in the serial log.
    name = p.isKey("name") ? p.getString("name") : String(DEVICE_NAME);
    if (p.isKey("bright")) bright = p.getUChar("bright");
    p.end();

    ESP_LOGI(TAG, "name=%s brightness=%u", name.c_str(), bright);
}

String LampSettings::sanitizeName(const String &in) {
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
    server.on("/status.json", HTTP_GET, std::bind(&LampSettings::handleStatusJson, this));
    server.on("/settings", HTTP_GET, std::bind(&LampSettings::handleGet, this));
    server.on("/settings", HTTP_POST, std::bind(&LampSettings::handleSave, this));
    server.on("/ota", HTTP_POST, std::bind(&LampSettings::handleOtaCheck, this));
    server.on("/reboot", HTTP_POST, std::bind(&LampSettings::handleReboot, this));
}

// The .local name this device will answer to after a reboot. Computed from the
// *current* setting rather than read back from mDNS, so a rename that has not
// taken effect yet still produces the address to come back to.
String LampSettings::expectedHostname() const {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s-%02x%02x%02x.local", name.c_str(), mac[3], mac[4], mac[5]);
    return String(buf);
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
         "fetch('/status.json',{cache:'no-store'})"
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

// EasyWiFi's stylesheet covers .card, .form-group, .button and text inputs, but
// has nothing for <h2> (its own pages have no subheadings) and does not style
// input[type=range]. Section headings and the brightness slider get rules here.
static const char *EXTRA_CSS =
    "<style>"
    "h2{font-size:16px;margin:24px 0 4px;color:#333;"
    "text-transform:uppercase;letter-spacing:.04em;}"
    "h2:first-of-type{margin-top:8px;}"
    "hr{border:0;border-top:1px solid #eee;margin:24px 0;}"
    "code{background:#f0f0f0;padding:1px 4px;border-radius:4px;}"
    "table{width:100%;border-collapse:collapse;margin:8px 0 4px;}"
    "th,td{text-align:left;padding:8px 0;border-bottom:1px solid #eee;font-size:15px;}"
    "th{color:#666;font-weight:500;width:40%;}"
    "td{color:#333;}"
    "input[type=range]{width:100%;}"
    "#swatch{display:inline-block;width:14px;height:14px;border-radius:50%;"
    "margin-right:8px;vertical-align:-2px;border:1px solid rgba(0,0,0,.15);}"
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
    html += webPages->getHTMLFooter();
    return html;
}

// The home page renders once and would then sit stale while the ring keeps
// blending. This is polled from that page to refresh the live fields in place.
void LampSettings::handleStatusJson() {
    WebServer &server = configServer.getServer();
    bool online = WiFi.status() == WL_CONNECTED;

    String j = "{";
    j += "\"color\":\"" + lampColorHex() + "\"";
    j += ",\"brightness\":" + String(lampBrightness());
    j += ",\"rssi\":" + String(online ? WiFi.RSSI() : 0);
    j += ",\"online\":";
    j += online ? "true" : "false";
    j += "}";

    // No-store: a cached status is worse than none, and some browsers will
    // happily serve one back for a bare GET on a small unchanging URL.
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

// One row of the status table. Kept out of handleHome() so the markup for a
// missing value is written once.
static String row(const String &label, const String &value) {
    return "<tr><th>" + label + "</th><td>" + (value.length() ? value : String("&mdash;")) + "</td></tr>";
}

// Same, with an id on the value cell so the poller can rewrite it.
static String rowId(const String &label, const String &value, const char *id) {
    return "<tr><th>" + label + "</th><td id='" + id + "'>" +
           (value.length() ? value : String("&mdash;")) + "</td></tr>";
}

void LampSettings::handleHome() {
    WebServer &server = configServer.getServer();
    bool online = WiFi.status() == WL_CONNECTED;

    String b;
    b += "<p>A ring of 8 LEDs blending between three vibrant colors.</p>";

    b += "<h2>Lamp</h2>";
    b += "<table>";
    b += "<tr><th>Color</th><td><span id='swatch' style='background:" + lampColorHex() +
         "'></span><span id='color'>" + lampColorHex() + "</span></td></tr>";
    b += rowId("Brightness", String(lampBrightness()) + " / 255", "bright");
    b += "</table>";

    b += "<h2>Network</h2>";
    b += "<table>";
    b += row("Hostname", name);
    b += row("mDNS", String(MDNS_HOSTNAME_HINT));
    b += row("WiFi", online ? WiFi.SSID() : String("not connected"));
    b += row("IP", online ? WiFi.localIP().toString() : String());
    b += rowId("Signal", online ? String(WiFi.RSSI()) + " dBm" : String(), "rssi");
    b += "</table>";

    b += "<h2>Firmware</h2>";
    b += "<table>";
    b += row("Version", FIRMWARE_VERSION);
    b += "</table>";

    b += "<div class='button-group'>";
    b += "<a href='/settings' class='button'>Settings</a>";
    b += "<a href='/wifi' class='button'>WiFi Setup</a>";
    b += "</div>";

    // Poll once a second so the color swatch tracks the ring. Failures are
    // swallowed: a reboot or a dropped link should leave the last known values
    // on screen rather than blanking the page.
    b += "<script>"
         "function u(){fetch('/status.json',{cache:'no-store'}).then(r=>r.json()).then(s=>{"
         "document.getElementById('color').textContent=s.color;"
         "document.getElementById('swatch').style.background=s.color;"
         "document.getElementById('bright').textContent=s.brightness+' / 255';"
         "document.getElementById('rssi').textContent=s.online?s.rssi+' dBm':'\\u2014';"
         "}).catch(()=>{});}"
         "setInterval(u,1000);u();"
         "</script>";

    server.send(200, "text/html", page("Glow Lamp", b));
}

void LampSettings::handleGet() {
    WebServer &server = configServer.getServer();

    String b;
    b += "<form method='POST' action='/settings'>";

    b += "<h2>Device</h2>";
    b += "<div class='form-group'>";
    b += "<label for='name'>Hostname</label>";
    b += "<input type='text' id='name' name='name' value='" + name + "' maxlength='18' required>";
    b += "<small>Lowercase letters, digits and hyphens. Becomes <code>" + name +
         "-&lt;mac&gt;.local</code> and the setup AP name. Takes effect after a reboot.</small>";
    b += "</div>";

    b += "<h2>Lamp</h2>";
    b += "<div class='form-group'>";
    b += "<label for='bright'>Brightness <span id='brightval'>" + String(bright) + "</span> / 255</label>";
    b += "<input type='range' id='bright' name='bright' min='1' max='255' value='" + String(bright) + "'>";
    b += "<small>Applies on save. The 500 mA cap in firmware limits what the top "
         "of the range actually draws.</small>";
    b += "</div>";

    b += "<div class='button-group'><button type='submit' class='button primary'>Save</button></div>";
    b += "</form>";

    // Separate forms: nesting them would submit the settings too.
    b += "<hr>";
    b += "<h2>Firmware</h2>";
    b += "<p>Running <code>" FIRMWARE_VERSION "</code>. The lamp checks GitHub for a "
         "newer release once a day; this asks now.</p>";
    b += "<form method='POST' action='/ota'>";
    b += "<div class='button-group'>";
    b += "<button type='submit' class='button primary'>Check for update</button>";
    b += "</div></form>";

    b += "<hr>";
    b += "<form method='POST' action='/reboot' onsubmit='return confirm(\"Reboot the lamp?\")'>";
    b += "<div class='button-group'>";
    b += "<a href='/' class='button'>Home</a>";
    b += "<button type='submit' class='button danger'>Reboot</button>";
    b += "</div></form>";

    b += "<script>"
         "var r=document.getElementById('bright'),o=document.getElementById('brightval');"
         "r.addEventListener('input',function(){o.textContent=r.value;});"
         "</script>";

    server.send(200, "text/html", page("Settings", b));
}

void LampSettings::handleSave() {
    WebServer &server = configServer.getServer();

    bool nameChanged = false;
    if (server.hasArg("name")) {
        String clean = sanitizeName(server.arg("name"));
        if (clean.length() && clean != name) {
            name = clean;
            nameChanged = true;
        }
    }
    if (server.hasArg("bright")) {
        long v = server.arg("bright").toInt();
        if (v >= 1 && v <= 255) bright = (uint8_t)v;
    }

    Preferences p;
    p.begin(NVS_NS, false);
    p.putString("name", name);
    p.putUChar("bright", bright);
    p.end();

    // Apply immediately rather than at the next boot -- the point of a slider is
    // seeing the result.
    setLampBrightness(bright);
    ESP_LOGI(TAG, "saved: name=%s brightness=%u", name.c_str(), bright);

    String b;
    b += "<div class='status success'>Saved</div>";
    if (nameChanged) {
        b += "<div class='status warning'>Hostname is now <b>" + name +
             "</b>. Reboot for it to take effect.</div>";
        b += "<form method='POST' action='/reboot'><div class='button-group'>"
             "<button type='submit' class='button danger'>Reboot now</button></div></form>";
    }
    b += "<div class='button-group'><a href='/settings' class='button primary'>Back to settings</a></div>";

    server.send(200, "text/html", page("Settings", b));
}

// Queue a check rather than run one here: a successful update reboots from
// inside the download, and doing that from a request handler kills the socket
// mid-response. loopOta() picks it up on the next pass.
void LampSettings::handleOtaCheck() {
    WebServer &server = configServer.getServer();
    ESP_LOGI(TAG, "update check requested from web UI");
    requestOtaCheck();

    String b;
    b += "<div class='status success'>Checking GitHub for a newer release.</div>";
    b += "<p>If one is found the lamp downloads it and reboots, which takes a "
         "minute or so. Watch the serial log for progress. Nothing happens if it "
         "is already up to date.</p>";
    b += "<div class='button-group'><a href='/' class='button primary'>Home</a></div>";

    server.send(200, "text/html", page("Firmware", b));
}
