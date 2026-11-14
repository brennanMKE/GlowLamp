#include <Arduino.h>
#include <FastLED.h>
#include <esp_log.h>
#include <sys/time.h>

#include "every_n_millis.h"
#include "effects.h"
#include "lamp.h"
#include "logbuf.h"
#include "mqtt.h"
#include "ota.h"
#include "sequences.h"
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

// ===== Sequences =====
// The built-in looks live in sequences.h. This is how long switching between
// two of them takes, and the custom effects' (shorter, because someone is
// sitting at the effects page waiting to see what they picked).
static const uint32_t SEQUENCE_FADE_MS = 3000;
static const uint32_t CUSTOM_FADE_MS = 1500;

// Breathe's color until Home Assistant's color picker sets one: the azure the
// vibrant palette already uses.
static const uint32_t DEFAULT_BREATHE_RGB = 0x0080ff;

// ===== Pulse =====
// A flash Home Assistant fires on an event. Four phases:
//
//   dip      down to a fifth of the current brightness   40% of the lead-in
//   rise     back up past it to the peak, accelerating   60% of the lead-in
//   peak     full brightness AND full white               PULSE_PEAK_FRAMES
//   return   back to the color and brightness it had      PULSE_RETURN_MS
//
// The lead-in is whatever the total leaves after peak and return. The dip is
// what makes it visible on a lamp that is already bright -- the old pulse only
// went up, and did nothing at all at 255 -- and white is the only way past
// 255: a saturated color lights one or two of each LED's channels, white
// lights all three. Nothing is saved.
//
// The peak is counted in frames, not ms, because a frame is the unit the eye
// actually gets: three of them (48 ms) is long enough to register as a flash
// without settling into a glow.
static const uint32_t PULSE_DEFAULT_MS = 750;
static const uint32_t PULSE_MIN_MS = 300;
static const uint32_t PULSE_MAX_MS = 5000;
static const uint32_t PULSE_PEAK_FRAMES = 3;
static const uint32_t PULSE_RETURN_MS = 75;
static const uint8_t PULSE_DIP_PERCENT = 40;  // of the lead-in; the rise gets the rest
static const uint8_t PULSE_DIP_FLOOR = 51;    // fraction of brightness at the bottom, /255
static const uint8_t PULSE_WHITEN_FROM = 192; // how far into the rise white starts, /255

// ===== Timing =====
// The custom effects (REST and the effects page). Milliseconds to cross from
// one palette color to the next, so a full blend cycle is BLEND_MS * colorCount
// -- 30 seconds on five colors. Slow on
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

// ===== Alert =====
// Full brightness, not a floor: an alert is the one thing that overrides what
// someone chose, because the whole point is that it cannot be missed.
static const uint8_t ALERT_BRIGHTNESS = 255;

// One pulse. Fast enough to read as urgent, slow enough not to strobe -- this
// is a beacon, not a stutter, and it swings between a dim ember and full red
// rather than blinking on and off.
static const uint32_t ALERT_PULSE_MS = 650;
static const uint8_t ALERT_FLOOR = 40;

// Default and ceiling for how long one runs.
static const uint32_t ALERT_DEFAULT_S = 30;
static const uint32_t ALERT_MAX_S = 3600;

CRGB leds[NUM_LEDS];

// ===== State =====
static uint8_t brightness = DEFAULT_BRIGHTNESS;
static bool power = true;
static CRGB currentColor = CRGB::Black;  // what the ring is lit with
static CRGB effectColor = CRGB::Black;   // what the effect is on, underneath

static bool identifying = false;
static uint32_t identifyStart = 0;
static uint32_t identifyEnd = 0;

static bool alerting = false;
static uint32_t alertStart = 0;
static uint32_t alertEnd = 0;

// What was last handed to FastLED. Identify and alert both override the
// configured brightness, so the value actually applied is decided in one place
// each frame rather than by whichever of them last called setBrightness().
//
// Initialised to 255, which is FastLED's own power-on default, NOT to 0: this
// is compared against the wanted value to decide whether to call setBrightness
// at all, so it has to start out describing what FastLED is actually doing. At
// 0 it would claim the strip was already dark, and a lamp whose stored
// brightness is 0 would come back from a reboot at full blast.
static uint8_t appliedBrightness = 255;

// ===== What is being drawn =====
//
// Two layers, so one sequence can cross-fade into the next. `layers[current]`
// is what is showing; the other is what is coming in while `fading`.
struct Layer {
    Sequence seq;
    FlickerState flicker;
};
static Layer layers[2];
static uint8_t current = 0;

static bool fading = false;
static uint64_t fadeStart = 0;  // on the shared clock, not millis()
static uint32_t fadeMs = 0;

// The saved sequence, and the color breathe uses.
static uint8_t sequenceIndex = 0;
static uint32_t breatheRgb = DEFAULT_BREATHE_RGB;

// A custom effect from the REST API or the effects page, showing instead of
// the sequence until it expires. effectIsDefault is false while it is.
static Sequence customSeq;
static uint8_t effectMode = EFFECT_BLEND;

// ===== The shared clock =====
//
// Milliseconds since 1970 once NTP has answered, which is what makes two lamps
// that never talk to each other show the same color: they are both drawing
// the same moment. Before NTP answers it is simply uptime, and the lamp runs
// on its own until it can join in.
//
// It is NEVER set straight from the wall clock. It advances by the frame time
// and is steered towards the wall clock -- by whole cycles when it is far off,
// which looks like nothing, then by speeding up or slowing down the rest of
// the way. Setting it would snap the color every time NTP corrected the clock.
static uint64_t clockMs = 0;
static bool clockSynced = false;
static int32_t clockErrorMs = 0;

// Anything earlier than late 2023 is the ESP32's own idea of time before NTP,
// which starts at 1970.
static const uint64_t WALL_PLAUSIBLE_MS = 1700000000000ULL;

// What was last chosen in the custom effect controls, kept apart from what is
// rendering. Reverting to the sequence changes the lamp, not the choice --
// otherwise "back to the sequence" would quietly throw away a palette that
// took a minute to pick.
static uint8_t selectedMode = EFFECT_BLEND;
static uint32_t selectedRgb[MAX_COLORS];
static uint8_t selectedCount = 0;

// What the API reports back, kept alongside the hue/saturation the renderers
// use so a caller reading its colors back sees the values it sent rather than
// what survived a round trip through the color wheel.
static uint32_t effectRgb[MAX_COLORS];
static uint8_t effectRgbCount = 0;

static bool effectIsDefault = true;
static bool effectHasExpiry = false;
static uint32_t effectExpiresAt = 0;

static bool pulsing = false;
static uint32_t pulseStart = 0;
static uint32_t pulseMs = 0;
static uint8_t pulseLevel = 0;

uint8_t lampBrightness() { return brightness; }
bool lampPower() { return power; }
bool lampIdentifying() { return identifying; }
uint8_t lampEffectMode() { return effectMode; }
const char *lampEffectName() {
    return effectIsDefault ? SEQUENCES[sequenceIndex].name : EFFECT_NAMES[effectMode];
}
bool lampEffectIsDefault() { return effectIsDefault; }

uint8_t lampSequence() { return sequenceIndex; }
const char *lampSequenceName() { return SEQUENCES[sequenceIndex].name; }
uint32_t lampBreatheColor() { return breatheRgb; }
bool lampFading() { return fading; }
bool lampClockSynced() { return clockSynced; }
int32_t lampClockErrorMs() { return clockErrorMs; }
bool lampPulsing() { return pulsing; }

void setLampBrightness(uint8_t value) {
    // Only ever records the setting. What reaches FastLED is decided in
    // loopLeds(), which runs every 16 ms -- so a slider change still looks
    // instant, and an overlay in progress is not dimmed out from under itself.
    brightness = value;
}

void setLampPower(bool on) { power = on; }

void identifyLamp(uint32_t durationMs) {
    identifyStart = millis();
    identifyEnd = identifyStart + durationMs;
    identifying = true;
}

bool lampAlerting() { return alerting; }

void triggerLampAlert(uint32_t seconds) {
    if (seconds == 0) seconds = ALERT_DEFAULT_S;
    if (seconds > ALERT_MAX_S) seconds = ALERT_MAX_S;
    alertStart = millis();
    alertEnd = alertStart + seconds * 1000UL;
    alerting = true;
    ESP_LOGI(TAG, "alert for %lus", (unsigned long)seconds);
}

void clearLampAlert() {
    if (!alerting) return;
    alerting = false;
    ESP_LOGI(TAG, "alert cleared");
}

int32_t lampAlertRemaining() {
    if (!alerting) return -1;
    uint32_t now = millis();
    if (timeReached(now, alertEnd)) return 0;
    return (int32_t)((alertEnd - now) / 1000);
}

bool pulseLamp(uint32_t ms, uint8_t level) {
    // An off lamp stays dark. A pulse is "look, something happened" on a lamp
    // someone is already looking at, not a reason to light one they turned off.
    if (!power) {
        ESP_LOGI(TAG, "pulse ignored, the lamp is off");
        return false;
    }
    if (ms == 0) ms = PULSE_DEFAULT_MS;
    if (ms < PULSE_MIN_MS) ms = PULSE_MIN_MS;
    if (ms > PULSE_MAX_MS) ms = PULSE_MAX_MS;
    if (level == 0) level = 255;
    pulseStart = millis();
    pulseMs = ms;
    pulseLevel = level;
    pulsing = true;
    ESP_LOGI(TAG, "pulse: dip, then flash to %u and white, %lums", level, (unsigned long)ms);
    return true;
}

static String hex(const CRGB &c) {
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", c.r, c.g, c.b);
    return String(buf);
}

String lampColorHex() { return hex(currentColor); }
String lampEffectColorHex() { return hex(effectColor); }

// Why the ring is dark, in the words someone standing in front of it would
// use. Empty when it is lit.
//
// This exists because a lamp that is simply switched off looks identical to a
// lamp that is broken, and the device knew the answer the whole time without
// anywhere to say it.
const char *lampDarkReason() {
    if (lampAlerting() || lampIdentifying()) return "";
    if (!power) return "the lamp is switched off";
    if (brightness == 0) return "brightness is set to 0";
    if (currentColor.r == 0 && currentColor.g == 0 && currentColor.b == 0) {
        return "the effect is showing black";
    }
    return "";
}

static uint32_t packRgb(const CRGB &c) {
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
}

static uint32_t paletteToRgb(const PaletteColor &c) { return packRgb(CRGB(CHSV(c.h, c.s, 255))); }

static PaletteColor rgbToPalette(uint32_t color) {
    CRGB rgb((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    // rgb2hsv_approximate is lossy, which is exactly why the original 0xRRGGBB
    // is kept for reporting. What matters here is that the renderers get a hue
    // they can travel along without crossing gray.
    CHSV hsv = rgb2hsv_approximate(rgb);
    PaletteColor out;
    out.h = hsv.h;
    // A color dark enough to have no meaningful hue -- near-black -- would
    // otherwise render as an arbitrary one at full saturation. Treating it as
    // unsaturated lets it read as the dim white it is.
    out.s = hsv.v < 16 ? 0 : hsv.s;
    return out;
}

// The saved sequence, with breathe's color filled in.
static void builtInSequence(Sequence &out) {
    out = SEQUENCES[sequenceIndex];
    if (sequenceIndex == SEQ_BREATHE) out.colors[0] = rgbToPalette(breatheRgb);
}

uint32_t lampSequenceColor() {
    if (sequenceIndex == SEQ_BREATHE) return breatheRgb;
    return paletteToRgb(SEQUENCES[sequenceIndex].colors[0]);
}

// Bring in whatever should now be showing: the custom effect if one is set,
// the saved sequence otherwise.
//
// The fade starts on a whole second of the shared clock, at least half a
// second out. A command sent to every lamp at once reaches them tens of
// milliseconds apart; starting on the same second is what makes them fade
// together rather than one after another.
static void showWanted(uint32_t fade) {
    if (fading) {
        // Already mid-fade: whatever was coming in becomes what is showing.
        // Before it had started, the old layer is still the right one.
        if (clockMs >= fadeStart) current ^= 1;
        fading = false;
    }

    Layer &next = layers[current ^ 1];
    if (effectIsDefault) {
        builtInSequence(next.seq);
    } else {
        next.seq = customSeq;
    }
    resetFlicker(next.flicker, millis());

    if (fade == 0) {
        current ^= 1;
        return;
    }
    fadeStart = ((clockMs + 500) / 1000 + 1) * 1000;
    fadeMs = fade;
    fading = true;
}

// effectRgb describes the sequence while no custom effect is set, so a caller
// switching effect with no colors gets the palette they can see.
static void reportSequenceColors() {
    const Sequence &s = SEQUENCES[sequenceIndex];
    if (sequenceIndex == SEQ_BREATHE) {
        effectRgb[0] = breatheRgb;
        effectRgbCount = 1;
    } else {
        effectRgbCount = s.count < MAX_COLORS ? s.count : MAX_COLORS;
        for (uint8_t i = 0; i < effectRgbCount; i++) effectRgb[i] = paletteToRgb(s.colors[i]);
    }
    effectMode = s.mode < EFFECT_COUNT ? s.mode : EFFECT_BLEND;

    // Only seeds the selection when nothing has been chosen yet -- at boot. A
    // reset after that leaves the stored choice untouched, which is the whole
    // point of keeping the two apart.
    if (selectedCount == 0) {
        for (uint8_t i = 0; i < effectRgbCount; i++) selectedRgb[i] = effectRgb[i];
        selectedCount = effectRgbCount;
        selectedMode = EFFECT_BLEND;
    }
}

bool setLampSequence(uint8_t index, uint32_t fade) {
    if (index >= SEQUENCE_COUNT) return false;
    sequenceIndex = index;
    effectIsDefault = true;
    effectHasExpiry = false;
    reportSequenceColors();
    showWanted(fade);
    ESP_LOGI(TAG, "sequence %s, fading over %lums", SEQUENCES[index].name, (unsigned long)fade);
    return true;
}

bool setLampSequence(uint8_t index) { return setLampSequence(index, SEQUENCE_FADE_MS); }

void setLampBreatheColor(uint32_t rgb) {
    breatheRgb = rgb & 0xFFFFFF;
    // Only redraws when breathe is what is showing; otherwise it is stored for
    // the next time someone picks it.
    if (sequenceIndex == SEQ_BREATHE) {
        reportSequenceColors();
        if (effectIsDefault) showWanted(SEQUENCE_FADE_MS);
    }
}

void resetLampEffect() {
    effectIsDefault = true;
    effectHasExpiry = false;
    reportSequenceColors();
    showWanted(CUSTOM_FADE_MS);
}

bool setLampEffect(uint8_t mode, const uint32_t *colors, uint8_t count, uint32_t seconds) {
    if (mode >= EFFECT_COUNT) return false;
    if (count == 0 || count > MAX_COLORS) return false;

    // The custom effects run on the shared clock too, so two lamps given the
    // same effect and palette over REST stay in step as well.
    static const uint16_t LEG[EFFECT_COUNT] = {BLEND_MS, LOOP_LEG_MS, 1500, NEON_LEG_MS};
    Sequence s = {};
    s.name = EFFECT_NAMES[mode];
    s.mode = mode;
    s.count = count;
    s.fadeMs = NEON_FADE_MS;
    for (uint8_t i = 0; i < count; i++) {
        s.colors[i] = rgbToPalette(colors[i]);
        s.legMs[i] = LEG[mode];
        effectRgb[i] = colors[i];
    }
    customSeq = s;

    effectMode = mode;
    effectRgbCount = count;

    // Remember it as the choice, so a later reset can be undone by eye.
    selectedMode = mode;
    selectedCount = count;
    for (uint8_t i = 0; i < count; i++) selectedRgb[i] = colors[i];

    effectIsDefault = false;
    effectHasExpiry = seconds > 0;
    effectExpiresAt = millis() + seconds * 1000UL;
    showWanted(CUSTOM_FADE_MS);

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

// Advance the shared clock by one frame and steer it towards NTP time.
static void advanceClock(uint32_t delta) {
    clockMs += delta;

    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint64_t wall = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
    if (wall < WALL_PLAUSIBLE_MS) return;  // NTP has not answered yet

    if (!clockSynced) {
        clockSynced = true;
        ESP_LOGI(TAG, "clock synced, joining the shared sequence");
    }

    int64_t err = (int64_t)(wall - clockMs);

    // Far off -- the first sync after boot, or NTP stepping the clock. Move by
    // whole repeats of what is showing, which changes nothing on the ring, and
    // leave at most half a repeat to steer out. Flicker does not follow the
    // clock, so for it the whole difference is invisible.
    uint32_t repeat = sequenceRepeatMs(layers[current].seq);
    int64_t jump = 0;
    if (repeat == 0) {
        jump = err;
    } else {
        int64_t half = repeat / 2;
        if (err > half || err < -half) {
            int64_t r = (int64_t)repeat;
            jump = (err >= 0 ? (err + half) / r : (err - half) / r) * r;
        }
    }
    if (jump) {
        clockMs += jump;
        fadeStart += jump;  // a fade waiting to start stays the same distance away
        err -= jump;
    }

    // Close: run up to half again as fast, or half as fast, until caught up.
    // Never backwards, and a few ms either way is not worth chasing.
    int64_t step = delta / 2;
    if (step < 1) step = 1;
    if (err > 2) {
        clockMs += err < step ? err : step;
    } else if (err < -2) {
        clockMs -= -err < step ? -err : step;
    }

    if (err > INT32_MAX) err = INT32_MAX;
    if (err < INT32_MIN) err = INT32_MIN;
    clockErrorMs = (int32_t)err;
}

// One frame of the pulse, `into` ms after it started, on a lamp at `base`:
// what brightness to show and how far towards white (0 none, 255 full).
static void pulseFrame(uint32_t into, uint8_t base, uint8_t &bright, uint8_t &white) {
    uint8_t peak = pulseLevel > base ? pulseLevel : base;
    uint8_t bottom = scale8(base, PULSE_DIP_FLOOR);
    const uint32_t peakMs = PULSE_PEAK_FRAMES * FRAME_MS;
    uint32_t lead = pulseMs - peakMs - PULSE_RETURN_MS;
    uint32_t dipMs = lead * PULSE_DIP_PERCENT / 100;
    uint32_t riseMs = lead - dipMs;

    if (into < dipMs) {
        uint8_t f = (uint8_t)((into * 255U) / dipMs);
        bright = lerp8by8(base, bottom, ease8InOutQuad(f));
        white = 0;
        return;
    }
    into -= dipMs;
    if (into < riseMs) {
        uint8_t f = (uint8_t)((into * 255U) / riseMs);
        // Squared: slow off the bottom, fastest into the peak, so the flash
        // arrives as a climax rather than a ramp.
        bright = lerp8by8(bottom, peak, scale8(f, f));
        white = f > PULSE_WHITEN_FROM
                    ? (uint8_t)(((uint16_t)(f - PULSE_WHITEN_FROM) * 255U) / (255 - PULSE_WHITEN_FROM))
                    : 0;
        return;
    }
    into -= riseMs;
    if (into < peakMs) {
        bright = peak;
        white = 255;
        return;
    }
    into -= peakMs;
    uint8_t f = into >= PULSE_RETURN_MS ? 255 : (uint8_t)((into * 255U) / PULSE_RETURN_MS);
    bright = lerp8by8(peak, base, f);
    white = (uint8_t)(255 - f);
}

static void loopLeds() {
    static uint32_t lastFrame = 0;

    uint32_t now = millis();
    uint32_t delta = now - lastFrame;
    if (delta < FRAME_MS) return;
    lastFrame = now;

    // A custom effect outlives its welcome by default, not by exception:
    // anything set over the API reverts on its own, so a lamp left
    // mid-experiment finds its way back without anyone remembering to put it
    // there.
    if (effectHasExpiry && timeReached(now, effectExpiresAt)) {
        ESP_LOGI(TAG, "effect expired, back to the sequence");
        resetLampEffect();
    }

    advanceClock(delta);

    // The effect renders whatever the lamp is doing, so power and the overlays
    // only decide what reaches the LEDs. Switching back on shows where the
    // sequence is now, which is where every other lamp is too.
    CHSV px[NUM_LEDS];
    Layer &cur = layers[current];
    renderSequence(cur.seq, clockMs, now, cur.flicker, px, NUM_LEDS);

    if (fading && clockMs >= fadeStart) {
        Layer &next = layers[current ^ 1];
        CHSV in[NUM_LEDS];
        renderSequence(next.seq, clockMs, now, next.flicker, in, NUM_LEDS);
        uint64_t into = clockMs - fadeStart;
        if (into >= fadeMs) {
            current ^= 1;
            fading = false;
            for (uint8_t i = 0; i < NUM_LEDS; i++) px[i] = in[i];
        } else {
            uint8_t frac = ease8InOutCubic((uint8_t)((into * 255U) / fadeMs));
            for (uint8_t i = 0; i < NUM_LEDS; i++) px[i] = blendHsv(px[i], in[i], frac);
        }
    }
    // The pulse decides brightness and how far towards white the ring is
    // pushed. It only touches lamps that are on: see pulseLamp().
    if (pulsing && now - pulseStart >= pulseMs) pulsing = false;
    uint8_t pulseBright = 0, pulseWhite = 0;
    bool pulseShowing = pulsing && power;
    if (pulseShowing) {
        pulseFrame(now - pulseStart, brightness, pulseBright, pulseWhite);
        if (pulseWhite) {
            // Desaturating keeps value: s=0, v=255 is full white.
            uint8_t keep = (uint8_t)(255 - pulseWhite);
            for (uint8_t i = 0; i < NUM_LEDS; i++) {
                px[i].s = scale8(px[i].s, keep);
                px[i].v = qadd8(px[i].v, scale8((uint8_t)(255 - px[i].v), pulseWhite));
            }
        }
    }

    for (uint8_t i = 0; i < NUM_LEDS; i++) leds[i] = px[i];

    // Expire the overlays. timeReached() rather than a plain compare: an
    // absolute deadline test strands them for 49.7 days if the loop blocks
    // across the millis() rollover. See include/every_n_millis.h.
    if (identifying && timeReached(now, identifyEnd)) identifying = false;
    if (alerting && timeReached(now, alertEnd)) {
        alerting = false;
        ESP_LOGI(TAG, "alert over");
    }

    // One decision about brightness per frame, made here rather than by
    // whichever overlay started last. They all override what someone chose,
    // and they can overlap.
    uint8_t want = pulseShowing ? pulseBright : brightness;
    if (identifying && want < IDENTIFY_MIN_BRIGHTNESS) want = IDENTIFY_MIN_BRIGHTNESS;
    if (alerting) want = ALERT_BRIGHTNESS;
    if (want != appliedBrightness) {
        FastLED.setBrightness(want);
        appliedBrightness = want;
    }

    // Identify wins over an alert: someone is standing at the lamp asking which
    // one this is, and they can see the alert on it either way.
    // effectColor is what the cycle is on; currentColor is what the ring is
    // actually lit with. They differ whenever an overlay is running or the lamp
    // is off, and conflating them is how a status page ends up reporting a
    // colour on a lamp that is dark -- which is exactly the report that made a
    // switched-off lamp look like a failure.
    effectColor = leds[0];

    if (identifying) {
        bool lit = ((now - identifyStart) / IDENTIFY_BLINK_MS) % 2 == 0;
        fill_solid(leds, NUM_LEDS, lit ? CRGB::White : CRGB::Black);
        currentColor = leds[0];
        FastLED.show();
        return;
    }

    // An alert ignores power as well as the effect. A lamp switched off is
    // exactly the lamp an alert most needs to reach, and the power state is
    // untouched underneath -- when this ends, it goes back to being off.
    if (alerting) {
        uint32_t into = (now - alertStart) % ALERT_PULSE_MS;
        uint8_t wave = triwave8((uint8_t)((into * 255UL) / ALERT_PULSE_MS));
        uint8_t value = ALERT_FLOOR + (uint8_t)(((uint16_t)wave * (255 - ALERT_FLOOR)) / 255);
        fill_solid(leds, NUM_LEDS, CHSV(0, 255, value));
        currentColor = leds[0];
        FastLED.show();
        return;
    }

    if (!power) fill_solid(leds, NUM_LEDS, CRGB::Black);
    currentColor = leds[0];
    FastLED.show();
}

void setup() {
    Serial.begin(115200);
    delay(500);  // let USB CDC come up before the first log line

    // Before anything else logs, so a boot is captured from its first line.
    setupLogBuffer();

    FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
    // 1 A: what the USB supply provides. Eight LEDs top out around 480 mA at
    // full white, so this is a safety net for a bigger ring, not a dimmer.
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 1000);  // volts, mA
    FastLED.clear(true);

    Serial.println("\n=== Glow Lamp " FIRMWARE_VERSION " ===");
    ESP_LOGI(TAG, "%d LEDs on pin %d, %d sequences, %d custom effects", NUM_LEDS, DATA_PIN,
             SEQUENCE_COUNT, EFFECT_COUNT);

    // Brightness and the power state come out of NVS, so setupWifiLink()
    // (which calls settings.begin()) has to run before they are applied. A lamp
    // switched off by Home Assistant comes back off after a power cut.
    setupWifiLink();
    setLampBrightness(settings.brightness());
    setLampPower(settings.power());
    // No fade at boot: there is nothing on the ring to fade from.
    setLampBreatheColor(settings.breatheColor());
    // Stored by name, so a sequence removed in a later build falls back to the
    // first one rather than to whatever took its index.
    uint8_t seq = findSequence(settings.sequence().c_str());
    setLampSequence(seq < SEQUENCE_COUNT ? seq : 0, 0);
    setupMqtt();
}

void loop() {
    loopWifiLink();
    loopOta();
    loopMqtt();

    loopLeds();
}
