// src/utils/evlog.h
// Persistent event log ("caja negra") stored in LittleFS.
//
// Why: the experiments (network cuts, resets, POWER cuts) make a PC-attached
// serial capture fragile — unplugging USB kills the capture. evlog keeps the
// module's own timeline in flash, so it survives any reset/power loss and can
// be downloaded afterwards (USB serial: "LOG DUMP", or WiFi via the captive
// portal: GET /log).
//
// evlog::logf()/logln() are drop-in replacements for Serial.printf()/println():
// they print EXACTLY the same text on Serial AND append it to the log file with
// a "B<boot> M<millis> E<epoch> | " prefix, so tools that parse the serial
// output parse the log too. Noisy periodic lines (heartbeat OK, NTP retries…)
// are Serial-only to keep flash writes low.
//
// Observer note: evlog adds a few small LittleFS appends around events. It is
// instrumentation on the same flash as the offline queue — disclose it in the
// methods when reporting power-cut results.
#pragma once

#include <Arduino.h>

namespace evlog {

    // Call once, right after storage::beginFs(). Bumps the boot counter, replays
    // the pre-FS boot lines into the log and writes a [boot] integrity report
    // (offline-queue survival: lines/valid/tailBytes).
    void begin(const char* resetReason, int resetCode);

    // printf-style: Serial + persistent log.
    void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    // println-style: Serial + persistent log.
    void logln(const char* s = "");

    // Non-blocking USB-serial command reader ("LOG DUMP", "LOG CLEAR", "LOG INFO").
    // Call periodically from a non-critical task. No-op in Wokwi/mock builds
    // (where Serial is the scanner).
    void pollCommands();

    uint32_t bootSeq();
}
