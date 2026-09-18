#ifndef EFFECTS_H
#define EFFECTS_H

// The four renderers, and the palette they draw with.
//
// Adapted from GlowKitchen's effect engine for a ring of 8. Its strip-shaped
// effects -- chase, scan, wipe, rain -- all say "this end, then that end",
// which a ring has nothing to say with: a run that travels along 240 LEDs
// simply goes round and round on 8. What survives is what reads as well in a
// circle as in a line, plus one effect that is better in a circle.
//
// GlowKitchen's version of this header deliberately does not include
// <FastLED.h>, so its renderers can be compiled against a stub and diffed
// frame-for-frame by a native test. That contract costs every other includer a
// forced include order, and this project has no native test to pay it for --
// lamp.h needs MAX_COLORS, and settings.cpp needs lamp.h, neither of which has
// any business knowing about FastLED. So this header includes what it uses.
//
// ===== Why the palette is hue + saturation, not RGB =====
//
// 0.0.4 blended in RGB, and a blend from red (255,0,40) to azure (0,120,255)
// passes through (127,60,147) on the way -- a washed-out mauve that reads as
// dirty white on a WS2812B. Every RGB interpolation between two colors that
// are far apart on the wheel crosses the gray axis, because that is what a
// straight line through the middle of the color cube does.
//
// Interpolating hue instead, the short way around the wheel, never leaves full
// saturation: red to azure now travels through orange, yellow and green, all
// of them vivid. Saturation is carried per color and interpolated too, so a
// deliberately pale color stays pale, but nothing becomes pale by accident.
//
// Value is not stored at all. It belongs to the brightness setting, and a
// palette that also carried value would be fighting it.

#include <FastLED.h>
#include <stdint.h>

#include "every_n_millis.h"  // timeReached()

// Five is the ceiling the UI offers and the API enforces. On 8 LEDs a sixth
// color would get barely one LED to itself in the effects that lay the palette
// out by position.
#define MAX_COLORS 5

struct PaletteColor {
    uint8_t h;
    uint8_t s;
};

enum EffectMode {
    EFFECT_BLEND = 0,    // the whole ring, one color, easing between palette entries
    EFFECT_LOOP = 1,     // the palette wrapped around the ring, rotating
    EFFECT_FLICKER = 2,  // one color, per-LED flicker, like a candle
    EFFECT_NEON = 3,     // a color per LED, each failing on its own schedule
    EFFECT_COUNT = 4
};

// The wire form, and what the UI shows. Index matches EffectMode; appended,
// never inserted, because these values are what an API caller sends.
static const char *const EFFECT_NAMES[EFFECT_COUNT] = {"blend", "loop", "flicker", "neon"};

// Everything a renderer needs. One instance lives in main.cpp.
struct EffectState {
    CRGB *leds = nullptr;
    uint8_t numLeds = 0;

    PaletteColor colors[MAX_COLORS];
    uint8_t colorCount = 0;

    // BLEND and LOOP: milliseconds into the cycle, accumulated from elapsed
    // time rather than taken from millis() directly. The modulo form jumps at
    // the millis() rollover -- a visible color snap once every 49.7 days --
    // because the wrap point is not a multiple of the cycle.
    uint32_t phase = 0;

    // FLICKER and NEON: when each LED next picks a new brightness, and which
    // palette entry FLICKER is currently on.
    uint32_t timeouts[16];
    uint32_t hueTimeout = 0;
    uint8_t hueIndex = 0;
};

// ===== Shared helpers =====

// Interpolate hue the short way around the wheel. Going the short way is what
// keeps a blend saturated: the long way around from red to azure would cross
// magenta, which is fine, but doing it in RGB crosses gray, which is not.
inline uint8_t blendHue(uint8_t from, uint8_t to, uint8_t frac) {
    uint8_t forward = (uint8_t)(to - from);
    uint8_t backward = (uint8_t)(from - to);
    if (forward <= backward) return (uint8_t)(from + (uint8_t)(((uint16_t)forward * frac) / 255));
    return (uint8_t)(from - (uint8_t)(((uint16_t)backward * frac) / 255));
}

inline CHSV blendColor(const PaletteColor &from, const PaletteColor &to, uint8_t frac) {
    return CHSV(blendHue(from.h, to.h, frac), lerp8by8(from.s, to.s, frac), 255);
}

// ===== BLEND =====
//
// The whole ring holds one color, eases to the next, and holds again. This is
// what the lamp has always done and what it comes back to; the only change is
// that the path between two colors now stays vivid.
//
// ease8InOutCubic is what makes it linger on the palette colors instead of
// sweeping past them -- a linear blend spends as little time on the pure
// colors as on the transition.
inline void renderBlend(EffectState &s, uint32_t blendMs) {
    uint8_t from = (s.phase / blendMs) % s.colorCount;
    uint8_t to = (uint8_t)((from + 1) % s.colorCount);

    // The multiply happens before the divide and in 32 bits, so a 6-second leg
    // does not lose resolution to integer truncation.
    uint8_t frac = (uint8_t)(((s.phase % blendMs) * 255UL) / blendMs);

    CHSV color = blendColor(s.colors[from], s.colors[to], ease8InOutCubic(frac));
    for (uint8_t i = 0; i < s.numLeds; i++) s.leds[i] = color;
}

// ===== LOOP =====
//
// The palette laid out around the ring and rotated, so the colors chase each
// other around the circle. This is the effect a ring earns that a strip does
// not: on a strip the two ends never meet and the gradient has a visible seam,
// while on a ring the last color blends back into the first and the seam is
// simply gone.
//
// Position is carried in 1/256ths of a palette step so the rotation is smooth
// at 8 LEDs; whole-LED steps would make it jump one eighth of a turn at a time.
inline void renderLoop(EffectState &s, uint32_t revolutionMs) {
    // How far around the palette one full revolution has travelled, 0..65535.
    uint32_t turn = ((uint64_t)s.phase % revolutionMs) * 65536ULL / revolutionMs;

    for (uint8_t i = 0; i < s.numLeds; i++) {
        // Where this LED sits in palette space, in 1/256ths of a step.
        uint32_t pos = ((uint32_t)i * s.colorCount * 256) / s.numLeds;
        pos = (pos + (turn * s.colorCount / 256)) % ((uint32_t)s.colorCount * 256);

        uint8_t index = (uint8_t)(pos / 256);
        uint8_t frac = (uint8_t)(pos % 256);
        uint8_t next = (uint8_t)((index + 1) % s.colorCount);

        s.leds[i] = blendColor(s.colors[index], s.colors[next], frac);
    }
}

// ===== FLICKER =====
//
// One palette color across the ring, each LED dipping and recovering on its
// own schedule, the color changing every few seconds. A candle, and on a ring
// the unevenness reads as a flame rather than as a fault.
//
// Each LED holds its brightness for 500-750 ms, so the ring settles instead of
// buzzing; the floor of 120 keeps it alight through every dip.
inline void renderFlicker(EffectState &s, uint32_t now) {
    for (uint8_t i = 0; i < s.numLeds; i++) {
        // timeReached() rather than a plain compare: an absolute deadline test
        // strands the LED for 49.7 days if the loop blocks across the rollover.
        if (timeReached(now, s.timeouts[i])) {
            s.timeouts[i] = now + random16(500, 750);
            uint8_t value = (uint8_t)random16(120, 255);
            s.leds[i] = CHSV(s.colors[s.hueIndex].h, s.colors[s.hueIndex].s, value);
        }
    }

    if (timeReached(now, s.hueTimeout)) {
        s.hueTimeout = now + 3000;
        s.hueIndex = (uint8_t)((s.hueIndex + 1) % s.colorCount);
    }
}

// ===== NEON =====
//
// FLICKER, except the palette is laid out by position instead of applied to
// the whole ring at once. That single difference is the mode: a neon sign has
// its blue tube and its red tube lit at the same time, in different places,
// each failing on its own schedule.
//
// GlowKitchen gives each color a run of LEDs, because on a long diffused strip
// four colors one LED apart read as white. A bare ring of 8 has the opposite
// problem -- runs would leave a 5-color palette with one LED each anyway -- so
// colors go one per LED here and the ring reads as distinct points of light.
inline void renderNeon(EffectState &s, uint32_t now) {
    for (uint8_t i = 0; i < s.numLeds; i++) {
        if (timeReached(now, s.timeouts[i])) {
            s.timeouts[i] = now + random16(500, 750);
            uint8_t value = (uint8_t)random16(120, 255);
            const PaletteColor &c = s.colors[i % s.colorCount];
            s.leds[i] = CHSV(c.h, c.s, value);
        }
    }
}

#endif  // EFFECTS_H
