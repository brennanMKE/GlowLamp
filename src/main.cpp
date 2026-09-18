#include <Arduino.h>
#include <FastLED.h>
#include <esp_log.h>

#include "every_n_millis.h"
#include "effects.h"
#include "lamp.h"
#include "mqtt.h"
#include "ota.h"
#include "settings.h"
#include "version.h"
#include "wifi_link.h"

static const char *TAG = "LAMP";

// ===== Ring layout =====
// One ring of 8 WS2812B LEDs. The ring is one light, not eight pixels: behind a
// diffuser, several colors an inch apart mix into white, so blend, loop and
// neon all light it a single color at a time. Flicker is the exception, and
// relies on that mixing to look like a flame. See the header of effects.h.
#define DATA_PIN 4
#define NUM_LEDS 8

// ===== Default palette =====
//
// Five colors, as hue and saturation. Given as hues rather than RGB triples
// because that is what the renderers blend in, and because a hue is the one
// form that cannot accidentally be pale: saturation 255 is vivid by
// construction, whatever the hue.
//
// Spaced 40-56 apart around the wheel so no two are close enough to read as
// the same color on a ring this small. Deliberately no white, no amber and no
// pastel: on a WS2812B those wash out, and the whole point of the palette is
// that the lamp is never showing something that looks like a dirty bulb.
static const PaletteColor DEFAULT_PALETTE[] = {
    {0, 255},    // red
    {40, 255},   // gold
    {96, 255},   // green
    {150, 255},  // azure
    {200, 255},  // magenta
};
static const uint8_t DEFAULT_PALETTE_SIZE =
    sizeof(DEFAULT_PALETTE) / sizeof(DEFAULT_PALETTE[0]);

// ===== Timing =====
// Milliseconds to cross from one palette color to the next, so a full blend
// cycle is BLEND_MS * colorCount -- 30 seconds on the five defaults. Slow on
// purpose: at a few seconds per leg the ring reads as a color-changing lamp,
// much faster and it reads as an effect.
static const uint32_t BLEND_MS = 6000;

// One leg of the loop effect -- shorter than a blend leg, because loop never
// pauses on a color and a slow constant drift reads as "stuck" rather than
// "moving".
static const uint32_t LOOP_LEG_MS = 2500;

// Neon holds a color dead still, then switches over the tail of the leg. The
// fade is quick relative to the hold: a tube changing color is a thing that
// happens, not a thing you watch happen.
static const uint32_t NEON_LEG_MS = 7000;
static const uint32_t NEON_FADE_MS = 700;

// ~60 fps. Faster buys nothing visible and each show() disables interrupts for
// roughly 30 us per LED.
static const uint32_t FRAME_MS = 16;

static const uint8_t DEFAULT_BRIGHTNESS = 64;

// How long the identify flash lasts by default, and how long each blink is.
// 150 ms reads as a deliberate signal; much faster looks like a fault.
static const uint32_t IDENTIFY_BLINK_MS = 150;

// Identify at a floor brightness, so a lamp dimmed to 5 in a bright room still
// announces itself. Restored to the configured value when the flash ends.
static const uint8_t IDENTIFY_MIN_BRIGHTNESS = 160;

CRGB leds[NUM_LEDS];

// ===== State =====
static uint8_t brightness = DEFAULT_BRIGHTNESS;
static bool power = true;
static CRGB currentColor = CRGB::Black;

static bool identifying = false;
static uint32_t identifyStart = 0;
static uint32_t identifyEnd = 0;

static EffectState fx;
static uint8_t effectMode = EFFECT_BLEND;

// What the API reports back, kept alongside the hue/saturation the renderers
// use so a caller reading its colors back sees the values it sent rather than
// what survived a round trip through the color wheel.
static uint32_t effectRgb[MAX_COLORS];
static uint8_t effectRgbCount = 0;

static bool effectIsDefault = true;
static bool effectHasExpiry = false;
static uint32_t effectExpiresAt = 0;

// The last thing someone chose, kept apart from what is rendering. Reverting
// to the default changes the lamp, not the choice -- otherwise "back to
// default" would quietly throw away a palette that took a minute to pick.
static uint8_t selectedMode = EFFECT_BLEND;
static uint32_t selectedRgb[MAX_COLORS];
static uint8_t selectedCount = 0;

uint8_t lampBrightness() { return brightness; }
bool lampPower() { return power; }
bool lampIdentifying() { return identifying; }
uint8_t lampEffectMode() { return effectMode; }
const char *lampEffectName() { return EFFECT_NAMES[effectMode]; }
bool lampEffectIsDefault() { return effectIsDefault; }

void setLampBrightness(uint8_t value) {
    brightness = value;
    // Not applied while identifying: the flash owns the brightness until it
    // ends, and would otherwise be dimmed mid-blink by a slider change.
    if (!identifying) FastLED.setBrightness(brightness);
}

void setLampPower(bool on) { power = on; }

void identifyLamp(uint32_t durationMs) {
    identifyStart = millis();
    identifyEnd = identifyStart + durationMs;
    identifying = true;
    FastLED.setBrightness(max(brightness, IDENTIFY_MIN_BRIGHTNESS));
}

String lampColorHex() {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", currentColor.r, currentColor.g, currentColor.b);
    return String(buf);
}

// Applying a palette resets the per-LED timers with it. Without this, the LEDs
// a flicker effect had already scheduled would keep their old deadlines and
// the new palette would arrive one LED at a time over the following second.
static void applyPalette(const PaletteColor *colors, uint8_t count) {
    fx.colorCount = count;
    for (uint8_t i = 0; i < count; i++) fx.colors[i] = colors[i];

    uint32_t now = millis();
    for (uint8_t i = 0; i < NUM_LEDS; i++) fx.timeouts[i] = now;
    fx.hueTimeout = now + 3000;
    fx.hueIndex = 0;
}

void resetLampEffect() {
    effectMode = EFFECT_BLEND;
    applyPalette(DEFAULT_PALETTE, DEFAULT_PALETTE_SIZE);

    // Report the defaults in the same form a caller would have sent them.
    for (uint8_t i = 0; i < DEFAULT_PALETTE_SIZE; i++) {
        CRGB rgb = CHSV(DEFAULT_PALETTE[i].h, DEFAULT_PALETTE[i].s, 255);
        effectRgb[i] = ((uint32_t)rgb.r << 16) | ((uint32_t)rgb.g << 8) | rgb.b;
    }
    effectRgbCount = DEFAULT_PALETTE_SIZE;

    // Only seeds the selection when nothing has been chosen yet -- at boot.
    // A reset after that leaves the stored choice untouched, which is the
    // whole point of keeping the two apart.
    if (selectedCount == 0) {
        for (uint8_t i = 0; i < DEFAULT_PALETTE_SIZE; i++) selectedRgb[i] = effectRgb[i];
        selectedCount = DEFAULT_PALETTE_SIZE;
        selectedMode = EFFECT_BLEND;
    }

    effectIsDefault = true;
    effectHasExpiry = false;
}

bool setLampEffect(uint8_t mode, const uint32_t *colors, uint8_t count, uint32_t seconds) {
    if (mode >= EFFECT_COUNT) return false;
    if (count == 0 || count > MAX_COLORS) return false;

    PaletteColor palette[MAX_COLORS];
    for (uint8_t i = 0; i < count; i++) {
        CRGB rgb((colors[i] >> 16) & 0xFF, (colors[i] >> 8) & 0xFF, colors[i] & 0xFF);
        // rgb2hsv_approximate is lossy, which is exactly why the original
        // 0xRRGGBB is kept for reporting. What matters here is that the
        // renderers get a hue they can travel along without crossing gray.
        CHSV hsv = rgb2hsv_approximate(rgb);
        palette[i].h = hsv.h;
        // A color dark enough to have no meaningful hue -- near-black -- would
        // otherwise render as an arbitrary one at full saturation. Treating it
        // as unsaturated lets it read as the dim white it is.
        palette[i].s = hsv.v < 16 ? 0 : hsv.s;
        effectRgb[i] = colors[i];
    }

    effectMode = mode;
    effectRgbCount = count;
    applyPalette(palette, count);

    // Remember it as the choice, so a later reset can be undone by eye.
    selectedMode = mode;
    selectedCount = count;
    for (uint8_t i = 0; i < count; i++) selectedRgb[i] = colors[i];

    effectIsDefault = false;
    effectHasExpiry = seconds > 0;
    effectExpiresAt = millis() + seconds * 1000UL;

    ESP_LOGI(TAG, "effect %s, %u colors, expires in %lus", EFFECT_NAMES[mode], count,
             (unsigned long)seconds);
    return true;
}

const char *lampSelectedName() { return EFFECT_NAMES[selectedMode]; }

uint8_t lampSelectedColors(uint32_t *out) {
    for (uint8_t i = 0; i < selectedCount; i++) out[i] = selectedRgb[i];
    return selectedCount;
}

uint8_t lampEffectColors(uint32_t *out) {
    for (uint8_t i = 0; i < effectRgbCount; i++) out[i] = effectRgb[i];
    return effectRgbCount;
}

int32_t lampEffectExpiresIn() {
    if (effectIsDefault || !effectHasExpiry) return -1;
    uint32_t now = millis();
    if (timeReached(now, effectExpiresAt)) return 0;
    return (int32_t)((effectExpiresAt - now) / 1000);
}

static void loopLeds() {
    static uint32_t lastFrame = 0;

    uint32_t now = millis();
    uint32_t delta = now - lastFrame;
    if (delta < FRAME_MS) return;
    lastFrame = now;

    // An effect outlives its welcome by default, not by exception: anything set
    // over the API reverts on its own, so a lamp left mid-experiment finds its
    // way back without anyone remembering to put it there.
    if (effectHasExpiry && timeReached(now, effectExpiresAt)) {
        ESP_LOGI(TAG, "effect expired, back to the default");
        resetLampEffect();
    }

    // Phase accumulates from elapsed time rather than being taken as
    // millis() % cycle. The modulo form jumps at the millis() rollover -- a
    // visible color snap once every 49.7 days -- because the wrap point is not
    // a multiple of the cycle. Unsigned subtraction of two samples is exact
    // across the wrap, so accumulating the difference is too.
    fx.phase += delta;

    // The effect renders whatever the lamp is doing, so power and identify only
    // decide what reaches the LEDs. Switching back on resumes the color the
    // effect would have been on rather than restarting it.
    switch (effectMode) {
        case EFFECT_LOOP:
            fx.phase %= LOOP_LEG_MS * fx.colorCount;
            renderLoop(fx, LOOP_LEG_MS);
            break;
        case EFFECT_FLICKER:
            renderFlicker(fx, now);
            break;
        case EFFECT_NEON:
            fx.phase %= NEON_LEG_MS * fx.colorCount;
            renderNeon(fx, NEON_LEG_MS, NEON_FADE_MS);
            break;
        case EFFECT_BLEND:
        default:
            fx.phase %= BLEND_MS * fx.colorCount;
            renderBlend(fx, BLEND_MS);
            break;
    }

    // What the status page reports. Taken after rendering so it is the color
    // the ring is actually showing, whichever effect drew it.
    currentColor = leds[0];

    if (identifying) {
        // timeReached() rather than a plain compare, and elapsed rather than an
        // absolute deadline: both survive the millis() rollover. See
        // include/every_n_millis.h.
        if (timeReached(now, identifyEnd)) {
            identifying = false;
            FastLED.setBrightness(brightness);
        } else {
            bool lit = ((now - identifyStart) / IDENTIFY_BLINK_MS) % 2 == 0;
            fill_solid(leds, NUM_LEDS, lit ? CRGB::White : CRGB::Black);
            FastLED.show();
            return;
        }
    }

    if (!power) fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
}

void setup() {
    Serial.begin(115200);
    delay(500);  // let USB CDC come up before the first log line

    FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 500);  // volts, mA
    FastLED.clear(true);

    fx.leds = leds;
    fx.numLeds = NUM_LEDS;
    resetLampEffect();

    Serial.println("\n=== Glow Lamp " FIRMWARE_VERSION " ===");
    ESP_LOGI(TAG, "%d LEDs on pin %d, %d effects, %d default colors", NUM_LEDS, DATA_PIN,
             EFFECT_COUNT, DEFAULT_PALETTE_SIZE);

    // Brightness and the power state come out of NVS, so setupWifiLink()
    // (which calls settings.begin()) has to run before they are applied. A lamp
    // switched off by Home Assistant comes back off after a power cut.
    setupWifiLink();
    setLampBrightness(settings.brightness());
    setLampPower(settings.power());
    setupMqtt();
}

void loop() {
    loopWifiLink();
    loopOta();
    loopMqtt();

    loopLeds();
}
