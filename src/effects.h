#ifndef EFFECTS_H
#define EFFECTS_H

// The four renderers, and the palette they draw with.
//
// Adapted from GlowKitchen's effect engine for a ring of 8 behind a diffuser.
//
// ===== The ring shows ONE color at a time =====
//
// This is the constraint the whole file is built around, and it rules out more
// than it first appears. GlowKitchen's strip effects -- chase, scan, wipe,
// rain -- are all "this end, then that end", which needs a run of LEDs long
// enough to see a position along it. But so does any effect that lays a
// palette out by position: on 8 diffused LEDs, five colors an inch apart do
// not read as five colors, they mix into a muddy white, which is the one thing
// this lamp must never look like.
//
// So blend, loop and neon paint all 8 LEDs the same color on every frame, and
// differ in how that one color moves over TIME -- how it changes, how fast,
// and how its brightness behaves -- not in where it sits in space.
//
// FLICKER is the deliberate exception, and it works for the same reason the
// others do not. Mixing is exactly what a flame is: give it a few colors that
// are already close together -- reds and ambers -- and the LEDs blurring into
// each other at different brightnesses reads as fire rather than as mud. The
// mixing is the effect. Hand it five colors from opposite sides of the wheel
// and it will go white, which is the user's call to make.
//
// An earlier version of this file got this wrong twice: `loop` wrapped the
// palette around the ring as a rotating gradient, and `neon` gave each LED its
// own color. Both are good effects on a bare 240-LED strip and both are white
// smudges on this lamp.
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

// Sized for the 8-LED ring with room to spare, so flicker's per-LED timers do
// not have to be resized alongside a bigger ring.
#define MAX_LEDS 16

struct PaletteColor {
    uint8_t h;
    uint8_t s;
};

enum EffectMode {
    EFFECT_BLEND = 0,    // holds on a color, eases to the next
    EFFECT_LOOP = 1,     // walks the palette steadily, never resting
    EFFECT_FLICKER = 2,  // several similar colors mixing, guttering like a flame
    EFFECT_NEON = 3,     // one color, full brightness, switching cleanly to the next
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

    // Flicker only: when each LED next picks a new brightness, and which palette
    // entry the ring is drifting through. A timer per LED is what puts them
    // deliberately out of step; no other effect varies brightness at all.
    uint32_t timeouts[MAX_LEDS];
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
// The same palette as blend, walked at a steady rate and never resting. Blend
// eases, so it spends most of its time sitting on a palette color; loop is
// always mid-transition, so the ring is never quite the color you would name.
// That is the whole difference between them, and on a single-color lamp it is
// the only difference available: no easing, and a shorter leg.
inline void renderLoop(EffectState &s, uint32_t legMs) {
    uint8_t from = (s.phase / legMs) % s.colorCount;
    uint8_t to = (uint8_t)((from + 1) % s.colorCount);

    // Linear, deliberately. ease8InOutCubic() here would make this blend.
    uint8_t frac = (uint8_t)(((s.phase % legMs) * 255UL) / legMs);

    CHSV color = blendColor(s.colors[from], s.colors[to], frac);
    for (uint8_t i = 0; i < s.numLeds; i++) s.leds[i] = color;
}

// ===== FLICKER =====
//
// A flame. Each LED carries its own palette color and its own brightness, and
// picks a new brightness every 80-220 ms on its own schedule; the assignment
// drifts around the ring every second and a half, so no LED stays the one that
// is always red.
//
// This is the one renderer that lights the ring several colors at once, and it
// is built to be given similar ones. The unevenness IS the flame: LEDs in
// lockstep read as a lamp being dimmed, not as something burning.
inline void renderFlicker(EffectState &s, uint32_t now) {
    for (uint8_t i = 0; i < s.numLeds; i++) {
        // timeReached() rather than a plain compare: an absolute deadline test
        // strands the LED for 49.7 days if the loop blocks across the rollover.
        if (timeReached(now, s.timeouts[i])) {
            s.timeouts[i] = now + random16(80, 220);
            const PaletteColor &c = s.colors[(i + s.hueIndex) % s.colorCount];
            s.leds[i] = CHSV(c.h, c.s, (uint8_t)random16(130, 255));
        }
    }

    // Drifts which LED shows which color, so the ring does not settle into a
    // fixed pattern of stripes.
    if (timeReached(now, s.hueTimeout)) {
        s.hueTimeout = now + 1500;
        s.hueIndex = (uint8_t)((s.hueIndex + 1) % s.colorCount);
    }
}

// ===== NEON =====
//
// A lit tube: full brightness, holding a color, then a quick fade to the next.
// No brightness variation at all.
//
// This effect shipped twice with a flicker in it, on the theory that a stutter
// is what makes neon read as neon. On a strip of 240 LEDs that is true. On 8
// diffused LEDs it is the whole lamp going dim at once, which is not a tube
// struggling to strike -- it is a light with a fault. Softening it helped and
// did not fix it, because the problem was the idea, not the depth.
//
// What distinguishes it from blend is now the SHAPE of the change rather than
// any texture: blend is always drifting, easing through the whole leg and never
// quite still, while neon sits rock steady on a color and then switches. A neon
// sign does not fade up and down. It is on, in one color, until it is another.
inline void renderNeon(EffectState &s, uint32_t legMs, uint32_t fadeMs) {
    uint8_t from = (s.phase / legMs) % s.colorCount;
    uint8_t to = (uint8_t)((from + 1) % s.colorCount);

    // A leg is mostly hold, with the cross-fade at the end of it.
    uint32_t into = s.phase % legMs;
    uint32_t holdMs = legMs > fadeMs ? legMs - fadeMs : 0;
    uint8_t frac = into >= holdMs ? (uint8_t)(((into - holdMs) * 255UL) / fadeMs) : 0;

    CHSV color = blendColor(s.colors[from], s.colors[to], ease8InOutCubic(frac));
    for (uint8_t i = 0; i < s.numLeds; i++) s.leds[i] = color;
}

#endif  // EFFECTS_H
