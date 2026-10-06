// src/drivers/i2c_bus.h
// Shared I2C bus wrapper. OLED (0x3C) and LCD (0x27) share SDA/SCL, so all
// access goes through a single FreeRTOS mutex to avoid bus collisions.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace i2c {
    bool begin();
    bool take(TickType_t timeoutTicks = pdMS_TO_TICKS(100));
    void give();

    // RAII lock guard used by display drivers.
    struct Lock {
        bool acquired;
        explicit Lock(TickType_t timeout = pdMS_TO_TICKS(100)) : acquired(take(timeout)) {}
        ~Lock() { if (acquired) give(); }
    };

    // Attempts to recover a stuck bus by clocking SCL manually 9 times.
    // Call this after several consecutive timeouts.
    void recover();

    // Diagnostic scanner — prints every responding I2C address to Serial.
    // Expected devices on our bus:  0x27 (LCD 16x2 PCF8574 backpack).
    void scan();
}
