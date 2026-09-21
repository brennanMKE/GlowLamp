#ifndef LOGBUF_H
#define LOGBUF_H

#include <Arduino.h>

// The last few KB of the log, kept in RAM and served over HTTP.
//
// A lamp has no console. Everything it knows about why it is behaving the way
// it is -- an OTA that failed, a broker that refused it, an effect that
// expired, the reason it last rebooted -- goes to a UART nobody is plugged
// into. Diagnosing anything meant carrying the lamp to a desk and a cable.
//
// This tees the ESP-IDF log into a ring buffer on its way to that UART, so the
// same lines can be read from /api/log over the network. The UART still gets
// everything; nothing is diverted.

void setupLogBuffer();

// Everything currently held, oldest first.
String logBufferContents();

// Why the chip last restarted, in words -- "power on", "software", "panic",
// "watchdog". A lamp that reboots on its own says nothing about it otherwise,
// and the difference between a panic and a power cut is most of the diagnosis.
const char *lastResetReason();

#endif  // LOGBUF_H
