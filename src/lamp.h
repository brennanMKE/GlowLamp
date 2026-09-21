#ifndef LAMP_H
#define LAMP_H

// The seam between the LED animation (main.cpp) and everything that drives it
// -- the web UI and the REST API. Nothing outside main.cpp touches FastLED.
//
// None of these persist anything. Storing the new value is settings.cpp's job,
// so a caller that only wants a momentary change (the identify flash, a test)
// does not have to write to NVS to get one.

#include <Arduino.h>

#include "effects.h"

// 0 is off, 255 is full -- Home Assistant's light convention, straight onto
// FastLED.setBrightness(). Brightness is independent of power: turning the lamp
// off and back on restores the brightness it had.
void setLampBrightness(uint8_t brightness);
uint8_t lampBrightness();

// Off renders black and keeps the color cycle running underneath, so turning it
// back on resumes where the lamp would have been rather than restarting.
void setLampPower(bool on);
bool lampPower();

// Blink white for a moment so you can tell which lamp you are looking at.
// Overrides both power and the current color, and restores whatever was showing
// when it finishes -- identifying a lamp that is switched off leaves it off.
void identifyLamp(uint32_t durationMs);
bool lampIdentifying();

// ===== Alert =====
//
// A red pulse loud enough to be noticed from across a room, for Home Assistant
// to fire at something that needs a person: a door left open, a cycle finished,
// a sensor that has gone quiet.
//
// It is an overlay, not an effect. It overrides the running effect, the
// brightness setting and the power state -- an alert nobody can see because the
// lamp was switched off or dimmed to 5 is not an alert -- and when it ends the
// lamp goes back to exactly what it was doing, including being off.
//
// It always expires. An alert that stays on forever stops being an alert, and a
// lamp stuck red because a broker went down is worse than no alert at all.
void triggerLampAlert(uint32_t seconds);
void clearLampAlert();
bool lampAlerting();

// Seconds left, or -1 when no alert is running.
int32_t lampAlertRemaining();

// The color the ring is showing right now, as "#rrggbb", before brightness
// scaling. Reports the color the cycle is on even while the lamp is off.
String lampColorHex();

// ===== Effects =====
//
// An effect set here is deliberately temporary. It expires back to the default
// -- blend, over the five built-in colors -- after `seconds`, and it does not
// survive a reboot: nothing persists it, so a lamp that loses power comes back
// showing what it is supposed to show rather than whatever someone was trying
// last week.
//
// `seconds` of 0 means "until the lamp reboots", which is as permanent as an
// effect gets here.

// Colors arrive as 0xRRGGBB and are converted to hue and saturation for
// rendering. Returns false if the mode is out of range or no colors were
// given, in which case nothing changes.
bool setLampEffect(uint8_t mode, const uint32_t *colors, uint8_t count, uint32_t seconds);

// Back to blend over the built-in palette, immediately.
void resetLampEffect();

uint8_t lampEffectMode();
const char *lampEffectName();

// Fills `out` with up to MAX_COLORS colors as 0xRRGGBB and returns how many.
// These are the colors as they were given, not the hue/saturation pair they
// are rendered from, so a caller reading them back sees what it set.
uint8_t lampEffectColors(uint32_t *out);

// Seconds until this effect reverts: -1 when the default is running or the
// effect was set to last until reboot.
int32_t lampEffectExpiresIn();

// True while the built-in default is what is showing.
bool lampEffectIsDefault();

// The last effect and palette someone chose, which is NOT what resetting
// changes. Reverting to the default is about what the lamp shows, not about
// forgetting what you picked -- so the settings page can put your colors back
// in front of you even while the lamp is showing its own.
//
// Seeded with the defaults at boot, so a lamp nobody has configured still
// offers a sensible starting palette.
const char *lampSelectedName();
uint8_t lampSelectedColors(uint32_t *out);

#endif  // LAMP_H
