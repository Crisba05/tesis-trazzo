// src/logic/dedup.h
// Anti-duplicate ring buffer. Ignores same code within DEDUP_WINDOW_MS.
#pragma once

#include <Arduino.h>

namespace dedup {
    void reset();

    // Returns true if this code is a duplicate (seen within the window).
    bool isDuplicate(const char* code);

    // Record a code as seen now.
    void record(const char* code);
}
