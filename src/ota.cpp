#include "ota.h"

#include <Config.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_log.h>
#include <time.h>

#include "version.h"

static const char *TAG = "OTA";

// Borrowed from GlowGhosts, including the reasoning: pinning two roots instead
// of shipping the full Mozilla store saves ~70 KB of flash, and BOTH are needed
// even though only one vendor is involved. api.github.com and github.com chain
// to Sectigo, but the release asset is redirected to
// objects.githubusercontent.com, which chains to ISRG. Pin only Sectigo and the
// version check succeeds while the download fails -- which looks exactly like
// "no update available" and is miserable to diagnose.
//
// These are the self-signed roots from a trusted store, not the cross-signed
// copies GitHub presents; mbedTLS closes the chain on subject + key, so the
// presented cross-signs go unused. Both run to 2045/2046.
//
// If GitHub ever moves to an unpinned root, every device fails to update at
// once and silently. The recovery path is then a USB reflash.
//
// mbedtls_x509_crt_parse() accepts concatenated PEM, so both live in one
// constant and one setCACert() call.
static const char GITHUB_ROOT_CAS[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"  // Sectigo Public Server Authentication Root E46, expires 2046-03-21
    "MIICOjCCAcGgAwIBAgIQQvLM2htpN0RfFf51KBC49DAKBggqhkjOPQQDAzBfMQsw\n"
    "CQYDVQQGEwJHQjEYMBYGA1UEChMPU2VjdGlnbyBMaW1pdGVkMTYwNAYDVQQDEy1T\n"
    "ZWN0aWdvIFB1YmxpYyBTZXJ2ZXIgQXV0aGVudGljYXRpb24gUm9vdCBFNDYwHhcN\n"
    "MjEwMzIyMDAwMDAwWhcNNDYwMzIxMjM1OTU5WjBfMQswCQYDVQQGEwJHQjEYMBYG\n"
    "A1UEChMPU2VjdGlnbyBMaW1pdGVkMTYwNAYDVQQDEy1TZWN0aWdvIFB1YmxpYyBT\n"
    "ZXJ2ZXIgQXV0aGVudGljYXRpb24gUm9vdCBFNDYwdjAQBgcqhkjOPQIBBgUrgQQA\n"
    "IgNiAAR2+pmpbiDt+dd34wc7qNs9Xzjoq1WmVk/WSOrsfy2qw7LFeeyZYX8QeccC\n"
    "WvkEN/U0NSt3zn8gj1KjAIns1aeibVvjS5KToID1AZTc8GgHHs3u/iVStSBDHBv+\n"
    "6xnOQ6OjQjBAMB0GA1UdDgQWBBTRItpMWfFLXyY4qp3W7usNw/upYTAOBgNVHQ8B\n"
    "Af8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNnADBkAjAn7qRa\n"
    "qCG76UeXlImldCBteU/IvZNeWBj7LRoAasm4PdCkT0RHlAFWovgzJQxC36oCMB3q\n"
    "4S6ILuH5px0CMk7yn2xVdOOurvulGu7t0vzCAxHrRVxgED1cf5kDW21USAGKcw==\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"  // ISRG Root YR (Let's Encrypt), expires 2045-09-02
    "MIIFKTCCAxGgAwIBAgIRAOxGNJNgz0sP+KmC2Tqpyj0wDQYJKoZIhvcNAQELBQAw\n"
    "LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw\n"
    "HhcNMjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzEN\n"
    "MAsGA1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEB\n"
    "BQADggIPADCCAgoCggIBANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9\n"
    "sGNiB0BD1fcOxbSUQCJIM1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGg\n"
    "YbSQ4OpzI+DG8SGuTlcE873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBN\n"
    "JAY+OKfX/FUvYKuhjT+no49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBzn\n"
    "ZqvbNPLMXMLFxCb3WTfrJBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904\n"
    "M+faKx8hnLCpJ15ZqaEgcNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawT\n"
    "vSZuVvlbRrAlLxIB6pwMBjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEa\n"
    "tnMdmDT5BqnKC92bd0EhM1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lz\n"
    "Yal+9zTg7C5DALyVOeG/CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU\n"
    "6H1qGg3DgTOuskf8eahTMiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9\n"
    "IWhH4YZKh3WnJEIt+oQvlYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/B\n"
    "AgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud\n"
    "DgQWBBTe51tg0CJtQCh9Pw0B/qS1UrRRlDANBgkqhkiG9w0BAQsFAAOCAgEAWHnf\n"
    "713Bdkq7t5yN2dNIgQakUb94X9WuyhMEHHkgx4oDpSUlnG0w4g94MoqaEUE31ZjR\n"
    "LU7L5LD1g9ujFHTQu8AD215AHMVQFbm6j8hQxdXHAzDajFNQnOlDJrLjzIx176oy\n"
    "AjvUtejZx2NNmdb5fd0WGVGsCdoAJ3N8ozo7ajE8t6vfxStZb4BQ9WYJGHUDrv2N\n"
    "i5tJF6CNiPnlzs3BUfECRbE4JSk+jvy8+VoGiFE8qsH/j78x2fjgQhAQFV7P7Zxy\n"
    "dBTZ1wEkNpZNW2qnaK1SKBLa+xf6E06YRIq5uaI+HWH8SY1y5VbRgzq40EKg3yxP\n"
    "06fz+uYAUIFJoLNfhwRCc3Q6pQVuMX3yAjHAes4gk4moGcLQ5p7HAh39yeylZc1J\n"
    "41sx/jKwLIkPE6Rr1Nf4pxdsxf9SA4yOEiAkDgq04DVxn8hgYFdUtBCuiuVC2heA\n"
    "EiqVEa+8QZjuw8Gj0EbHXcRd1nInvGqRS1o9Is7YBdQN57X1AYveGBNNqjICSb7c\n"
    "awuw1EawTDrs13VUlJVEsbQ0/O/1aaV73mCdOQ8azqL2KTv1Ewu1xbquE2S+kdQU\n"
    "To9TUwat3wUA6cwXh1EfpS/3fJ0aGah5hdpRyoCLDlsSn8tkrjMfFFX0viC+GxHc\n"
    "sI1ANRYvqSFC2X1VRZfDg+wD6E21BccmifG4yWc=\n"
    "-----END CERTIFICATE-----\n";

static Preferences prefs;

// Set from the web handler, acted on in loopOta(). See ota.h.
static bool checkRequested = false;

void requestOtaCheck() { checkRequested = true; }

// Download and flash whatever is at `url`, unconditionally -- the caller decides
// whether an update is warranted. Returns only on failure; success reboots.
static void performOtaFromUrl(const String &url) {
    ESP_LOGI(TAG, "downloading firmware from %s", url.c_str());

    WiFiClientSecure client;
    client.setCACert(GITHUB_ROOT_CAS);

    // Progress logging, otherwise the ~1 MB write is a silent 10-30 s gap.
    httpUpdate.onProgress([](int done, int total) {
        ESP_LOGI(TAG, "progress %d%% (%d/%d bytes)", total ? (done * 100 / total) : 0, done, total);
    });
    httpUpdate.onError([](int err) {
        ESP_LOGE(TAG, "error %d: %s", err, httpUpdate.getLastErrorString().c_str());
    });

    // Own the reboot so the log line makes it out over UART before the reset.
    httpUpdate.rebootOnUpdate(false);
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    switch (httpUpdate.update(client, url)) {
        case HTTP_UPDATE_OK:
            ESP_LOGI(TAG, "written, rebooting into new firmware");
            Serial.flush();
            delay(100);
            ESP.restart();
            break;
        case HTTP_UPDATE_NO_UPDATES:
            ESP_LOGI(TAG, "server says no update needed");
            break;
        case HTTP_UPDATE_FAILED:
            ESP_LOGE(TAG, "failed, error=%d: %s", httpUpdate.getLastError(),
                     httpUpdate.getLastErrorString().c_str());
            break;
    }
}

// Ask GitHub for the latest release tag and flash it if it differs from what is
// running. Tag comparison, not ordering: a deliberate rollback to an older
// release is an update too.
//
// Until the repo has its first release the API answers 404, which logs a warning
// and returns -- the lamp keeps running whatever it was flashed with.
static void checkForOtaUpdate() {
    ESP_LOGI(TAG, "checking for update (running %s)", FIRMWARE_VERSION);

    WiFiClientSecure client;
    client.setCACert(GITHUB_ROOT_CAS);

    HTTPClient http;
    String api = String("https://api.github.com/repos/") + OTA_GITHUB_REPO + "/releases/latest";
    http.begin(client, api);
    http.addHeader("User-Agent", "GlowLamp-OTA");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        ESP_LOGW(TAG, "GitHub API returned HTTP %d", code);
        http.end();
        return;
    }

    // Scan for tag_name by hand. The release payload is tens of KB of fields we
    // do not want and would have to filter or size a JSON document for; two
    // indexOf calls are smaller and cannot fail on an unexpectedly large
    // response. ArduinoJson is not linked into this firmware at all.
    String body = http.getString();
    http.end();

    int idx = body.indexOf("\"tag_name\"");
    if (idx < 0) {
        ESP_LOGW(TAG, "tag_name not found in response");
        return;
    }
    int q1 = body.indexOf('"', idx + 10);
    int q2 = body.indexOf('"', q1 + 1);
    if (q1 < 0 || q2 < 0) {
        ESP_LOGW(TAG, "could not parse tag_name");
        return;
    }
    String tag = body.substring(q1 + 1, q2);
    if (tag.length() > 0 && tag[0] == 'v') tag = tag.substring(1);

    ESP_LOGI(TAG, "latest tag=%s, running=%s", tag.c_str(), FIRMWARE_VERSION);
    if (tag == String(FIRMWARE_VERSION)) {
        ESP_LOGI(TAG, "already up to date");
        return;
    }

    performOtaFromUrl(String("https://github.com/") + OTA_GITHUB_REPO +
                      "/releases/latest/download/firmware.bin");
}

void loopOta() {
    if (WiFi.status() != WL_CONNECTED) return;

    // NTP once per boot, after WiFi is up. Only the daily scheduler needs it --
    // the checks themselves query GitHub directly and do not care about the clock.
    static bool timeConfigured = false;
    static uint32_t wifiUpAt = 0;
    if (!timeConfigured) {
        configTzTime("PST8PDT,M3.2.0,M11.1.0/2", "pool.ntp.org", "time.nist.gov");
        timeConfigured = true;
        wifiUpAt = millis();
    }

    // Operator requests first, so a manual check is not delayed behind the
    // startup timer below.
    if (checkRequested) {
        checkRequested = false;
        checkForOtaUpdate();
        return;
    }

    // One check a short while after joining WiFi, so a board flashed by hand
    // catches up to the current release without waiting for the daily window.
    static const uint32_t STARTUP_DELAY_MS = 15000;
    static bool startupDone = false;
    if (!startupDone && (millis() - wifiUpAt) >= STARTUP_DELAY_MS) {
        startupDone = true;
        checkForOtaUpdate();
        return;
    }
    if (!startupDone) return;

    // Daily check, mid-afternoon on purpose rather than overnight: an update
    // that goes wrong reboots the lamp, and 15:00 is when someone is around to
    // notice. There is no rollback, so the hour is the only safety margin.
    static const int CHECK_HOUR = 15;

    struct tm now;
    if (!getLocalTime(&now, 0)) return;     // NTP has not landed yet
    if (now.tm_year + 1900 < 2020) return;  // clock not plausible
    if (now.tm_hour != CHECK_HOUR) return;

    prefs.begin("glowlamp", true);
    int lastYday = prefs.getInt("ota_yday", -1);
    prefs.end();
    if (lastYday == now.tm_yday) return;  // already ran today

    // Persist BEFORE checking: a successful update reboots from inside
    // checkForOtaUpdate() and never returns, so writing the day first is what
    // keeps the once-a-day guard intact across that reboot.
    prefs.begin("glowlamp", false);
    prefs.putInt("ota_yday", now.tm_yday);
    prefs.end();

    ESP_LOGI(TAG, "daily check (yday=%d)", now.tm_yday);
    checkForOtaUpdate();
}
