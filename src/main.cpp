#include <Arduino.h>
#include <FastLED.h>
#include <esp_log.h>

#include "lamp.h"
#include "ota.h"
#include "settings.h"
#include "version.h"
#include "wifi_link.h"

static const char *TAG = "LAMP";

// ===== Ring layout =====
// One ring of 8 WS2812B LEDs. Every LED always shows the same color -- the ring
// is one light, not eight addressable pixels. Later versions may break that.
#define DATA_PIN 4
#define NUM_LEDS 8

// ===== Palette =====
// Three vibrant colors the lamp blends between, in cycle order. Saturated and
// far apart in hue on purpose: WS2812B washes pastels out to near-white, and a
// blend between two neighboring hues reads as one slowly shifting color rather
// than as a cycle.
static const CRGB PALETTE[] = {
    CRGB(255, 0, 40),    // magenta-red
    CRGB(0, 120, 255),   // azure
    CRGB(0, 255, 90),    // spring green
};
static const uint8_t PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);

// ===== Timing =====
// Milliseconds to cross from one palette color to the next, so a full cycle is
// BLEND_MS * PALETTE_SIZE. Slow on purpose: at a few seconds per leg the ring
// reads as a color-changing lamp; much faster and it reads as an effect.
static const uint32_t BLEND_MS = 6000;

// ~60 fps. Faster buys nothing visible on a blend this slow, and each show()
// disables interrupts for roughly 30 us per LED.
static const uint32_t FRAME_MS = 16;

static const uint8_t DEFAULT_BRIGHTNESS = 64;

CRGB leds[NUM_LEDS];

// ===== State =====
static uint8_t brightness = DEFAULT_BRIGHTNESS;
static CRGB currentColor = PALETTE[0];

uint8_t lampBrightness() { return brightness; }

void setLampBrightness(uint8_t value) {
    brightness = value;
    FastLED.setBrightness(brightness);
}

String lampColorHex() {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", currentColor.r, currentColor.g, currentColor.b);
    return String(buf);
}

// Phase is accumulated from elapsed time rather than taken as millis() % cycle.
// The modulo form jumps at the millis() rollover -- a visible color snap once
// every 49.7 days -- because the wrap point is not a multiple of the cycle.
// Unsigned subtraction of two samples is exact across the wrap, so accumulating
// the difference is not.
static void loopLeds() {
    static uint32_t lastFrame = 0;
    static uint32_t phase = 0;  // ms into the full cycle

    uint32_t now = millis();
    uint32_t delta = now - lastFrame;
    if (delta < FRAME_MS) return;
    lastFrame = now;

    phase = (phase + delta) % (BLEND_MS * PALETTE_SIZE);

    uint8_t from = phase / BLEND_MS;
    uint8_t to = (from + 1) % PALETTE_SIZE;

    // 0..255 across the leg. The multiply is done before the divide and in
    // 32-bit, so a 6000 ms leg does not lose resolution to integer truncation.
    uint8_t t = ((phase % BLEND_MS) * 255UL) / BLEND_MS;

    // Ease in and out so the lamp lingers on each palette color instead of
    // sweeping past it -- a linear blend spends as little time on the pure
    // colors as on the muddy midpoint between them.
    currentColor = blend(PALETTE[from], PALETTE[to], ease8InOutCubic(t));

    fill_solid(leds, NUM_LEDS, currentColor);
    FastLED.show();
}

void setup() {
    Serial.begin(115200);
    delay(500);  // let USB CDC come up before the first log line

    FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 500);  // volts, mA
    FastLED.clear(true);

    Serial.println("\n=== Glow Lamp " FIRMWARE_VERSION " ===");
    ESP_LOGI(TAG, "%d LEDs on pin %d, %lu ms per blend", NUM_LEDS, DATA_PIN,
             (unsigned long)BLEND_MS);

    // Brightness comes out of NVS, so setupWifiLink() (which calls
    // settings.begin()) has to run before it is applied.
    setupWifiLink();
    setLampBrightness(settings.brightness());
}

void loop() {
    loopWifiLink();
    loopOta();

    loopLeds();
}
