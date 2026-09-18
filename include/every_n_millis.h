// every_n_millis.h — rollover-safe periodic timers for ESP32 / Arduino.
//
// This is the single source of truth for the timing helpers referenced
// throughout this skill. Copy it into the project (src/ or include/) rather
// than retyping the macro — every hand-copied variant is a chance to
// reintroduce a bug that takes 49.7 days to show up.
//
// The one rule behind everything here: never compare two absolute millis()
// values. Compare a difference. See references/timing.md for why.

#pragma once

#include <Arduino.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Fixed-interval periodic timer — the default.
// ---------------------------------------------------------------------------
//
//   if (EVERY_N_MILLIS_ID(sensor_read, 500)) { readSensors(); }
//
// The immediately-invoked lambda makes this a real expression, so it can sit
// inside an if() condition. Each expansion has its own closure type, so the
// static is unique per call site; ID is for readability in review.
//
// millis() is sampled once, so the stored time is exactly the one that was
// tested — re-sampling on assignment makes the timer drift slowly late.
//
// FastLED's lib8tion.h already defines EVERY_N_MILLIS and EVERY_N_MILLISECONDS
// as *block* macros (`EVERY_N_MILLISECONDS(100) { ... }`, no if()). If FastLED
// is linked, prefer its versions — they use the elapsed form and are
// rollover-safe. The guard below keeps this header from colliding with them.
#ifndef EVERY_N_MILLIS_ID
#define EVERY_N_MILLIS_ID(ID, N) ([]{                     \
    static uint32_t _timer_##ID = 0;                      \
    uint32_t _now = millis();                             \
    if (_now - _timer_##ID < (uint32_t)(N)) return false;  \
    _timer_##ID = _now;                                   \
    return true;                                          \
  }())
#endif

// Same thing without the preprocessor, when the state should be visible at the
// call site (e.g. it lives in a struct, or you want to reset it):
//
//   static uint32_t tSensor = 0;
//   if (everyMillis(tSensor, 500)) { readSensors(); }
static inline bool everyMillis(uint32_t &last, uint32_t interval) {
    uint32_t now = millis();
    if (now - last < interval) return false;
    last = now;
    return true;
}

// ---------------------------------------------------------------------------
// Variable-interval deadline test.
// ---------------------------------------------------------------------------
//
// Use when each event carries its own random or computed interval, so the
// elapsed form would need a second array to store it:
//
//   uint32_t timeouts[NUM_LEDS];
//   if (timeReached(now, timeouts[i])) { timeouts[i] = now + random(500, 750); }
//
// `deadline < now` looks correct and usually is, but it survives the millis()
// wrap only if the loop samples the clock during the interval straddling it.
// Block across that instant — a blocking reconnect, an OTA download — and the
// deadline is stranded ~49.7 days in the future.
//
// Unsigned subtraction is well-defined on overflow, so the difference is the
// true elapsed interval across the wrap; reading it as signed answers "has the
// deadline passed?" correctly for any deadline within ~24.8 days of now.
//
// The types must be the same width. uint32_t/int32_t is spelled out
// deliberately: unsigned long/long happen to be 32-bit on ESP32, but become
// 64-bit in a PlatformIO `native` test build, where nothing wraps at 2^32 and
// a rollover test would pass while exercising nothing.
static inline bool timeReached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

// ---------------------------------------------------------------------------
// Optional: shiftable clock for testing the rollover.
// ---------------------------------------------------------------------------
//
// A 49.7-day feedback loop cannot be verified by waiting. Routing timers
// through nowMs() lets a dev build sit just before the wrap so it can be
// reached in seconds.
//
// This is for firmware that will run unattended for weeks. Skip it in a first
// sketch or a demo — it is bookkeeping that only pays off once the device is
// deployed. Enable with -DENABLE_CLOCK_OFFSET.
#ifdef ENABLE_CLOCK_OFFSET
extern uint32_t clockOffsetMs;   // define once in a .cpp: uint32_t clockOffsetMs = 0;
static inline uint32_t nowMs() { return millis() + clockOffsetMs; }
#else
static inline uint32_t nowMs() { return millis(); }
#endif
