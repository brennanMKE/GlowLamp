#ifndef SEQUENCES_H
#define SEQUENCES_H

// The named looks a lamp can run. What Home Assistant lists as effects, what
// glowlamp/<host>/sequence and glowlamp/all/sequence select, and what a lamp
// saves and comes back to after a power cut.
//
// Every one but ember follows the shared clock, so any two lamps on the same
// sequence show the same color at the same moment. See effects.h.
//
// Hues are FastLED's rainbow wheel, not degrees: red 0, orange 32, yellow 64,
// green 96, aqua 128, blue 160, purple 192, pink 224.
//
// APPEND ONLY. The names are the wire format and what NVS stores; the index is
// only ever used inside one boot.

#include <strings.h>  // strcasecmp

#include "effects.h"

static const uint8_t SEQ_BREATHE = 6;  // the one whose color comes from the lamp

static const Sequence SEQUENCES[] = {
    // The original: five colors spaced 40-56 apart so none read as the same
    // on a ring this small, easing between them, 6 s each.
    {"vibrant", EFFECT_BLEND, 5,
     {{0, 255}, {40, 255}, {96, 255}, {150, 255}, {200, 255}},
     {6000, 6000, 6000, 6000, 6000}, 0, 0, 0},

    // Teal to indigo and back round, slowly, with a shallow swell like water.
    // 8 s swell divides the 40 s cycle.
    {"ocean", EFFECT_BLEND, 4,
     {{112, 255}, {136, 255}, {160, 255}, {180, 255}},
     {10000, 10000, 10000, 10000}, 0, 40, 8000},

    // Out to gold and back again, so it never jumps from gold straight to
    // magenta. Six steps, four colors, 72 s.
    {"sunset", EFFECT_BLEND, 6,
     {{216, 255}, {0, 255}, {20, 255}, {40, 255}, {20, 255}, {0, 255}},
     {12000, 12000, 12000, 12000, 12000, 12000}, 0, 0, 0},

    // Uneven legs so it feels like it is wandering. Fixed, not random, so two
    // lamps still wander together.
    {"aurora", EFFECT_BLEND, 4,
     {{96, 255}, {120, 255}, {190, 255}, {128, 255}},
     {11000, 6000, 14000, 8000}, 0, 0, 0},

    // The whole wheel, one turn a minute.
    {"spectrum", EFFECT_SPECTRUM, 1, {{0, 255}}, {60000}, 0, 0, 0},

    // The vibrant colors, snapping every 1.5 s. Where lamps visibly switching
    // together pays off most.
    {"party", EFFECT_NEON, 5,
     {{0, 255}, {40, 255}, {96, 255}, {150, 255}, {200, 255}},
     {1500, 1500, 1500, 1500, 1500}, 150, 0, 0},

    // One color, rising and falling over 5 s. The color here is a placeholder;
    // main.cpp substitutes the lamp's breathe color, which Home Assistant's
    // color picker sets.
    {"breathe", EFFECT_BLEND, 1, {{150, 255}}, {5000}, 0, 180, 5000},

    // A fire. Unsynced: see effects.h.
    {"ember", EFFECT_FLICKER, 3, {{0, 255}, {12, 255}, {24, 255}}, {1500, 1500, 1500}, 0, 0, 0},
};

static const uint8_t SEQUENCE_COUNT = sizeof(SEQUENCES) / sizeof(SEQUENCES[0]);

// One line each for the API and the effects page.
static const char *const SEQUENCE_DESCRIPTIONS[SEQUENCE_COUNT] = {
    "red, gold, green, azure, magenta, easing from one to the next",
    "teal to indigo, slow, with a gentle swell",
    "magenta to gold and back again",
    "green, teal and violet, wandering at uneven speeds",
    "the whole color wheel, one turn a minute",
    "vibrant colors, switching every second and a half",
    "one color, rising and falling",
    "red and amber flickering like a fire",
};

// Index of `name`, or SEQUENCE_COUNT if there is no such sequence. Case
// insensitive, because it is typed into automations by hand.
inline uint8_t findSequence(const char *name) {
    for (uint8_t i = 0; i < SEQUENCE_COUNT; i++) {
        if (strcasecmp(name, SEQUENCES[i].name) == 0) return i;
    }
    return SEQUENCE_COUNT;
}

#endif  // SEQUENCES_H
