// src/drivers/leds.cpp
#include "drivers/leds.h"
#include "config.h"

namespace leds {

    // Non-blocking blink state. tick() from the display task turns the LEDs
    // off once the requested duration elapses. Durations were bumped so the
    // operator has time to see the result on a busy scanner.
    static uint32_t _greenOffAtMs = 0;
    static uint32_t _redOffAtMs   = 0;

    void begin() {
        pinMode(pins::LED_GREEN, OUTPUT);
        pinMode(pins::LED_RED,   OUTPUT);
        allOff();
    }

    void greenOn()  { digitalWrite(pins::LED_GREEN, HIGH); _greenOffAtMs = 0; }
    void greenOff() { digitalWrite(pins::LED_GREEN, LOW);  _greenOffAtMs = 0; }
    void redOn()    { digitalWrite(pins::LED_RED,   HIGH); _redOffAtMs   = 0; }
    void redOff()   { digitalWrite(pins::LED_RED,   LOW);  _redOffAtMs   = 0; }

    void allOff() {
        digitalWrite(pins::LED_GREEN, LOW);
        digitalWrite(pins::LED_RED,   LOW);
        _greenOffAtMs = 0;
        _redOffAtMs   = 0;
    }

    void blinkGreen(uint16_t durationMs) {
        digitalWrite(pins::LED_GREEN, HIGH);
        _greenOffAtMs = millis() + durationMs;
    }

    void blinkRed(uint16_t durationMs) {
        digitalWrite(pins::LED_RED, HIGH);
        _redOffAtMs = millis() + durationMs;
    }

    void tick() {
        uint32_t now = millis();
        if (_greenOffAtMs && (int32_t)(now - _greenOffAtMs) >= 0) {
            digitalWrite(pins::LED_GREEN, LOW);
            _greenOffAtMs = 0;
        }
        if (_redOffAtMs && (int32_t)(now - _redOffAtMs) >= 0) {
            digitalWrite(pins::LED_RED, LOW);
            _redOffAtMs = 0;
        }
    }

    void dormantPulseTick() {
        static uint32_t lastMs = 0;
        static bool on = false;
        if (millis() - lastMs >= 2000) {
            lastMs = millis();
            on = !on;
            digitalWrite(pins::LED_GREEN, on ? HIGH : LOW);
        }
    }
}
