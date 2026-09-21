#include "logbuf.h"

#include <esp_log.h>
#include <esp_system.h>

#include <stdarg.h>
#include <stdio.h>

// 4 KB holds a few hundred lines, which covers a boot plus whatever has
// happened since. Static, not heap: this has to be usable while diagnosing a
// device that may be short of memory, which is the worst time to allocate.
static const size_t LOG_CAPACITY = 4096;
static char logBuf[LOG_CAPACITY];
static size_t logHead = 0;   // where the next character goes
static bool logWrapped = false;

// The UART writer this replaces, kept so the serial console still gets
// everything. Teeing rather than diverting matters: someone who does plug a
// cable in should not find the log missing because a feature took it.
static vprintf_like_t previousWriter = nullptr;

// Called from whatever task logged, including from inside the WiFi and TLS
// stacks. It has to be cheap and it must not allocate or block.
//
// There is deliberately no lock. Two tasks logging at the same instant can
// interleave their characters, which makes a mangled line; taking a mutex here
// would put a logging call on the critical path of every task in the system to
// prevent an inconvenience. A garbled line in a diagnostic buffer is a fair
// trade for that.
static int logWriter(const char *format, va_list args) {
    char line[256];
    int written = vsnprintf(line, sizeof(line), format, args);
    if (written > 0) {
        size_t n = (size_t)written < sizeof(line) ? (size_t)written : sizeof(line) - 1;
        for (size_t i = 0; i < n; i++) {
            logBuf[logHead] = line[i];
            logHead = (logHead + 1) % LOG_CAPACITY;
            if (logHead == 0) logWrapped = true;
        }
    }

    // Straight through to the original writer, with the untouched varargs.
    if (previousWriter) return previousWriter(format, args);
    return written;
}

void setupLogBuffer() {
    previousWriter = esp_log_set_vprintf(&logWriter);

    // The compile-time ceiling is set in platformio.ini; this is the runtime
    // level, which defaults lower on some builds. Setting it explicitly means
    // the buffer's contents do not depend on which default happened to apply.
    esp_log_level_set("*", ESP_LOG_INFO);
}

String logBufferContents() {
    String out;
    if (logWrapped) {
        out.reserve(LOG_CAPACITY + 1);
        // Oldest first: everything after the head, then everything before it.
        for (size_t i = logHead; i < LOG_CAPACITY; i++) out += logBuf[i];
        for (size_t i = 0; i < logHead; i++) out += logBuf[i];
    } else {
        out.reserve(logHead + 1);
        for (size_t i = 0; i < logHead; i++) out += logBuf[i];
    }
    return out;
}

const char *lastResetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "panic or exception";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_EXT: return "external reset";
        default: return "unknown";
    }
}
