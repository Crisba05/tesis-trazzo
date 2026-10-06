// src/drivers/scanner.h
// QR/Barcode scanner driver. Uses Serial2 (GM65) on hardware,
// Serial (typed input) on Wokwi simulation.
#pragma once

#include <Arduino.h>

namespace scanner {
    void begin();

    // Returns true if a code was read. Copies into `buf` (max `bufLen`).
    bool poll(char* buf, size_t bufLen);
}
