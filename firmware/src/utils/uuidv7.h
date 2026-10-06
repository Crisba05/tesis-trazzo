// src/utils/uuidv7.h
// UUIDv7 generator — time-ordered 128-bit identifier per RFC 9562.
// Format: 48-bit unix_ts_ms | 4-bit version (7) | 12-bit rand | 2-bit variant | 62-bit rand
// String: "01910f8e-7b00-7a92-8c4f-1234abcd5678" (36 chars + null)
#pragma once

#include <Arduino.h>

namespace uuidv7 {
    // Generates a UUIDv7 string into `out`. Buffer must be at least 37 bytes.
    void generate(char* out);
}
