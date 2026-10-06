// src/drivers/leds.h
#pragma once
#include <Arduino.h>

namespace leds {
    void begin();
    void greenOn();
    void greenOff();
    void redOn();
    void redOff();
    void allOff();

    // Non-blocking blink: turns the LED on immediately and schedules an
    // off at now+durationMs. Call tick() periodically to perform the off.
    void blinkGreen(uint16_t durationMs = 1500);
    void blinkRed(uint16_t durationMs = 1500);

    // Called from the display/logic loop to drive the non-blocking off.
    void tick();

    // Dormant slow pulse (call repeatedly from idle task)
    void dormantPulseTick();
}
