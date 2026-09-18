#ifndef LAMP_H
#define LAMP_H

// The seam between the LED animation (main.cpp) and everything that drives it
// -- the web UI and the REST API. Nothing outside main.cpp touches FastLED.
//
// None of these persist anything. Storing the new value is settings.cpp's job,
// so a caller that only wants a momentary change (the identify flash, a test)
// does not have to write to NVS to get one.

#include <Arduino.h>

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

// The color the ring is showing right now, as "#rrggbb", before brightness
// scaling. Reports the color the cycle is on even while the lamp is off.
String lampColorHex();

#endif  // LAMP_H
