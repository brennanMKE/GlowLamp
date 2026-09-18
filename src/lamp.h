#ifndef LAMP_H
#define LAMP_H

// The seam between the LED animation (main.cpp) and everything else (the web
// UI, and later whatever else drives the lamp). Nothing outside main.cpp
// touches FastLED directly.

#include <Arduino.h>

// 0 is off, 255 is full -- Home Assistant's light convention, straight onto
// FastLED.setBrightness(). Persisting it is the caller's job; main.cpp only
// applies it.
void setLampBrightness(uint8_t brightness);
uint8_t lampBrightness();

// The color the whole ring is showing right now, as "#rrggbb", before
// brightness scaling. For the status page.
String lampColorHex();

#endif  // LAMP_H
