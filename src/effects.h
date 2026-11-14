#ifndef EFFECTS_H
#define EFFECTS_H

// The renderers, the palette they draw with, and the Sequence that bundles the
// two into something with a name.
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
// So blend, loop, neon and spectrum paint all 8 LEDs the same color on every
// frame, and differ in how that one color moves over TIME -- how it changes,
// how fast, and how its brightness behaves -- not in where it sits in space.
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
// ===== Time comes from the shared clock =====
//
// Every renderer except flicker is a pure function of one number: milliseconds
// on a clock that every lamp agrees on (NTP wall time, eased in by main.cpp).
// Two lamps running the same sequence therefore show the same color at the
// same moment without ever exchanging a message -- including lamps on
// different networks in different buildings.
//
// That clock is 64-bit, and the position in the cycle is `clock % cycle`.
// The old accumulate-a-phase scheme existed to dodge the millis() rollover;
// a 64-bit millisecond clock does not roll over, so the modulo is exact.
//
// Flicker is unsynced on purpose: two lamps guttering in lockstep look like
// one lamp being dimmed, not like two fires.
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
// The cross-fade between two sequences goes the same way, per LED.
//
// Value is not stored in the palette. It belongs to the brightness setting;
// the only thing a sequence may do with it is a slow swell (see below).

#include <FastLED.h>
#include <stdint.h>

#include "every_n_millis.h"  // timeReached()

// Five is the ceiling the UI offers and the API enforces for a custom palette.
// On 8 LEDs a sixth color would get barely one LED to itself in flicker.
#define MAX_COLORS 5

// A built-in sequence can have more steps than a custom palette, because a
// step is a point in time, not a color competing for LEDs: sunset walks out to
// gold and back, which is six steps over four colors.
#define SEQ_MAX_STEPS 8

// Sized for the 8-LED ring with room to spare, so flicker's per-LED timers do
// not have to be resized alongside a bigger ring.
#define MAX_LEDS 16

struct PaletteColor {
    uint8_t h;
    uint8_t s;
};

// How a sequence moves. The first four are also the custom effects the REST
// API and the effects page offer; the order is the wire format's.
enum EffectMode {
    EFFECT_BLEND = 0,     // holds on a color, eases to the next
    EFFECT_LOOP = 1,      // walks the palette steadily, never resting
    EFFECT_FLICKER = 2,   // several similar colors mixing, guttering like a flame
    EFFECT_NEON = 3,      // one color, full brightness, switching cleanly to the next
    EFFECT_COUNT = 4,     // the custom effects end here
    EFFECT_SPECTRUM = 4,  // the whole wheel, turning continuously (sequences only)
};

// The wire form, and what the UI shows. Index matches EffectMode; appended,
// never inserted, because these values are what an API caller sends.
static const char *const EFFECT_NAMES[EFFECT_COUNT] = {"blend", "loop", "flicker", "neon"};

// ===== Sequence =====
//
// A named look: what moves (the renderer), through what (the steps), and how
// fast (one duration per step, so a sequence can linger on some colors and
// hurry past others).
//
// Swell is an optional slow rise and fall in value on top of whatever the
// renderer is doing. It is NOT a flicker -- effects.h has been burned twice by
// the whole ring dimming at once reading as a fault -- so it is slow (seconds)
// and, apart from breathe, shallow. `swellDepth` is how much of the value is
// taken away at the trough: 0 is none, 255 is down to black.
//
// The swell period should divide the cycle. main.cpp moves the clock by whole
// cycles when NTP first lands, which is invisible only if every part of the
// look repeats on that cycle.
struct Sequence {
    const char *name;
    uint8_t mode;  // EffectMode
    uint8_t count;
    PaletteColor colors[SEQ_MAX_STEPS];
    uint16_t legMs[SEQ_MAX_STEPS];
    uint16_t fadeMs;  // neon: the cross-fade at the end of each leg
    uint8_t swellDepth;
    uint16_t swellMs;
};

inline uint32_t sequenceCycleMs(const Sequence &s) {
    uint32_t total = 0;
    for (uint8_t i = 0; i < s.count; i++) total += s.legMs[i];
    return total ? total : 1;
}

// The period after which every part of the sequence repeats -- what main.cpp
// may jump the clock by without anyone seeing. 0 for flicker, which has no
// period and does not follow the clock at all.
inline uint32_t sequenceRepeatMs(const Sequence &s) {
    if (s.mode == EFFECT_FLICKER) return 0;
    uint32_t cycle = sequenceCycleMs(s);
    if (s.swellDepth && s.swellMs && cycle % s.swellMs) return cycle * s.swellMs;
    return cycle;
}

// Flicker's per-LED timers. One per layer, so a cross-fade between two
// flickering sequences does not have them fighting over the same LEDs.
struct FlickerState {
    uint32_t timeouts[MAX_LEDS];
    CHSV px[MAX_LEDS];
    uint32_t hueTimeout;
    uint8_t hueIndex;
};

inline void resetFlicker(FlickerState &f, uint32_t now) {
    for (uint8_t i = 0; i < MAX_LEDS; i++) {
        f.timeouts[i] = now;
        f.px[i] = CHSV(0, 0, 0);
    }
    f.hueTimeout = now + 1500;
    f.hueIndex = 0;
}

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

// The cross-fade between two whole renders. Value is interpolated too, so a
// swell or a flicker fades out rather than stopping.
inline CHSV blendHsv(const CHSV &a, const CHSV &b, uint8_t frac) {
    return CHSV(blendHue(a.h, b.h, frac), lerp8by8(a.s, b.s, frac), lerp8by8(a.v, b.v, frac));
}

// Which step the clock is in, and how far into it. Walks the legs because they
// can differ; at most eight of them, once a frame.
inline void findLeg(const Sequence &s, uint32_t t, uint8_t &from, uint32_t &into, uint32_t &leg) {
    for (uint8_t i = 0; i < s.count; i++) {
        if (t < s.legMs[i] || i == s.count - 1) {
            from = i;
            into = t;
            leg = s.legMs[i] ? s.legMs[i] : 1;
            if (into >= leg) into = leg - 1;
            return;
        }
        t -= s.legMs[i];
    }
    from = 0;
    into = 0;
    leg = 1;
}

inline uint8_t fracOf(uint32_t into, uint32_t span) {
    return span ? (uint8_t)(((uint64_t)into * 255U) / span) : 255;
}

// ===== BLEND =====
//
// The whole ring holds one color, eases to the next, and holds again.
// ease8InOutCubic is what makes it linger on the palette colors instead of
// sweeping past them -- a linear blend spends as little time on the pure
// colors as on the transition.
inline CHSV renderBlend(const Sequence &s, uint32_t t) {
    uint8_t from;
    uint32_t into, leg;
    findLeg(s, t, from, into, leg);
    uint8_t to = (uint8_t)((from + 1) % s.count);
    return blendColor(s.colors[from], s.colors[to], ease8InOutCubic(fracOf(into, leg)));
}

// ===== LOOP =====
//
// The same walk as blend, at a steady rate and never resting. Blend eases, so
// it spends most of its time sitting on a palette color; loop is always
// mid-transition, so the ring is never quite the color you would name.
inline CHSV renderLoop(const Sequence &s, uint32_t t) {
    uint8_t from;
    uint32_t into, leg;
    findLeg(s, t, from, into, leg);
    uint8_t to = (uint8_t)((from + 1) % s.count);
    // Linear, deliberately. ease8InOutCubic() here would make this blend.
    return blendColor(s.colors[from], s.colors[to], fracOf(into, leg));
}

// ===== NEON =====
//
// A lit tube: full brightness, holding a color, then a quick fade to the next.
//
// This effect shipped twice with a flicker in it, on the theory that a stutter
// is what makes neon read as neon. On 8 diffused LEDs it is the whole lamp
// going dim at once, which is not a tube struggling to strike -- it is a light
// with a fault. What distinguishes it from blend is the SHAPE of the change:
// blend is always drifting, neon sits rock steady and then switches.
inline CHSV renderNeon(const Sequence &s, uint32_t t) {
    uint8_t from;
    uint32_t into, leg;
    findLeg(s, t, from, into, leg);
    uint8_t to = (uint8_t)((from + 1) % s.count);

    uint32_t fade = s.fadeMs ? s.fadeMs : 1;
    if (fade > leg) fade = leg;
    uint32_t hold = leg - fade;
    uint8_t frac = into >= hold ? fracOf(into - hold, fade) : 0;
    return blendColor(s.colors[from], s.colors[to], ease8InOutCubic(frac));
}

// ===== SPECTRUM =====
//
// The whole wheel, one turn per cycle, at full saturation. Not a palette walk:
// there is nothing to linger on, so it is a straight line round the hue.
inline CHSV renderSpectrum(const Sequence &s, uint32_t t) {
    uint32_t cycle = sequenceCycleMs(s);
    return CHSV((uint8_t)(((uint64_t)t * 256U) / cycle), 255, 255);
}

// ===== FLICKER =====
//
// A flame. Each LED carries its own palette color and its own brightness, and
// picks a new brightness every 80-220 ms on its own schedule; the assignment
// drifts around the ring every second and a half, so no LED stays the one that
// is always red. The unevenness IS the flame.
inline void renderFlicker(const Sequence &s, FlickerState &f, uint32_t now, CHSV *out,
                          uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        // timeReached() rather than a plain compare: an absolute deadline test
        // strands the LED for 49.7 days if the loop blocks across the rollover.
        if (timeReached(now, f.timeouts[i])) {
            f.timeouts[i] = now + random16(80, 220);
            const PaletteColor &c = s.colors[(i + f.hueIndex) % s.count];
            f.px[i] = CHSV(c.h, c.s, (uint8_t)random16(130, 255));
        }
        out[i] = f.px[i];
    }

    if (timeReached(now, f.hueTimeout)) {
        f.hueTimeout = now + 1500;
        f.hueIndex = (uint8_t)((f.hueIndex + 1) % s.count);
    }
}

// ===== One frame of a sequence =====
//
// `clockMs` is the shared clock; `now` is millis(), for flicker's timers only.
inline void renderSequence(const Sequence &s, uint64_t clockMs, uint32_t now, FlickerState &f,
                           CHSV *out, uint8_t n) {
    if (s.count == 0) {
        for (uint8_t i = 0; i < n; i++) out[i] = CHSV(0, 0, 0);
        return;
    }
    if (s.mode == EFFECT_FLICKER) {
        renderFlicker(s, f, now, out, n);
        return;
    }

    uint32_t t = (uint32_t)(clockMs % sequenceCycleMs(s));
    CHSV c;
    switch (s.mode) {
        case EFFECT_LOOP:
            c = renderLoop(s, t);
            break;
        case EFFECT_NEON:
            c = renderNeon(s, t);
            break;
        case EFFECT_SPECTRUM:
            c = renderSpectrum(s, t);
            break;
        case EFFECT_BLEND:
        default:
            c = renderBlend(s, t);
            break;
    }

    // A sine, so it never sits at the bottom: a swell that pauses dark reads
    // as the lamp having gone out.
    if (s.swellDepth && s.swellMs) {
        uint8_t angle = (uint8_t)(((clockMs % s.swellMs) * 256U) / s.swellMs);
        uint8_t wave = sin8(angle);  // 0..255, starts mid-way
        uint8_t dip = scale8(s.swellDepth, (uint8_t)(255 - wave));
        c.v = (uint8_t)(255 - dip);
    }

    for (uint8_t i = 0; i < n; i++) out[i] = c;
}

#endif  // EFFECTS_H
