// src/drivers/button.cpp
// Simplified button driver with hold-time classification.
//
// Gestures:
//   SHORT_PRESS     (<2s)  — release before 2s
//   LONG_PRESS      (2-5s) — release between 2s and 5s
//   VERY_LONG_PRESS (>5s)  — release after 5s (or fires at 5s while still held)
//   TRIPLE_AND_HOLD        — 3 rapid clicks then hold 4s (factory reset)
//
// Double-press was removed because it's unreliable on tactile buttons
// and confusing for non-technical users (school staff).

#include "drivers/button.h"
#include "config.h"

namespace button {

    enum class State : uint8_t {
        IDLE,
        PRESSED,           // button is being held down
        WAIT_MULTI,        // just released a short press, waiting for another click
        MULTI_PRESSED,     // 2nd or 3rd press is being held
        HOLD_AFTER_TRIPLE, // 3 clicks done, holding for factory reset
    };

    static State    state       = State::IDLE;
    static uint32_t pressStart  = 0;
    static uint32_t releaseTs   = 0;
    static uint8_t  clickCount  = 0;
    static bool     lastStable  = false;  // true = pressed
    static uint32_t lastEdgeMs  = 0;
    static bool     veryLongFired = false;

    static bool rawPressed() { return digitalRead(pins::BUTTON) == LOW; }

    void begin() {
        pinMode(pins::BUTTON, INPUT_PULLUP);
        state = State::IDLE;
        clickCount = 0;
        lastStable = false;
        veryLongFired = false;
    }

    bool poll(ButtonGesture& gesture) {
        const uint32_t now = millis();
        const bool raw = rawPressed();

        // ── Debounce ──
        if (raw != lastStable) {
            if (now - lastEdgeMs < BUTTON_DEBOUNCE_MS) return false;
            lastEdgeMs = now;
            lastStable = raw;
        }

        switch (state) {

            // ── IDLE: waiting for a press ──
            case State::IDLE:
                if (raw) {
                    state = State::PRESSED;
                    pressStart = now;
                    veryLongFired = false;
                }
                break;

            // ── PRESSED: button is held down ──
            case State::PRESSED:
                if (!raw) {
                    // Released — classify by how long it was held
                    uint32_t held = now - pressStart;
                    if (held >= BUTTON_VERY_LONG_MS) {
                        // Already fired via veryLongFired below, or fire now
                        if (!veryLongFired) {
                            gesture = ButtonGesture::VERY_LONG_PRESS;
                            Serial.printf("[btn] VERY_LONG (%lums)\n", held);
                            state = State::IDLE;
                            clickCount = 0;
                            return true;
                        }
                        state = State::IDLE;
                        clickCount = 0;
                    } else if (held >= BUTTON_LONG_PRESS_MS) {
                        gesture = ButtonGesture::LONG_PRESS;
                        Serial.printf("[btn] LONG (%lums)\n", held);
                        state = State::IDLE;
                        clickCount = 0;
                        return true;
                    } else {
                        // Short press — maybe start of multi-click sequence
                        clickCount = 1;
                        releaseTs = now;
                        state = State::WAIT_MULTI;
                    }
                } else if (!veryLongFired && (now - pressStart >= BUTTON_VERY_LONG_MS)) {
                    // Fire VERY_LONG while still held (user doesn't need to release)
                    veryLongFired = true;
                    gesture = ButtonGesture::VERY_LONG_PRESS;
                    Serial.println("[btn] VERY_LONG (still held)");
                    state = State::IDLE;
                    clickCount = 0;
                    return true;
                }
                break;

            // ── WAIT_MULTI: short press released, watching for more clicks ──
            case State::WAIT_MULTI:
                if (raw) {
                    clickCount++;
                    pressStart = now;
                    state = State::MULTI_PRESSED;
                } else if (now - releaseTs > BUTTON_DOUBLE_GAP_MS) {
                    // Timeout — emit SHORT_PRESS
                    gesture = ButtonGesture::SHORT_PRESS;
                    Serial.println("[btn] SHORT");
                    state = State::IDLE;
                    clickCount = 0;
                    return true;
                }
                break;

            // ── MULTI_PRESSED: 2nd or 3rd press is being held ──
            case State::MULTI_PRESSED:
                if (!raw) {
                    releaseTs = now;
                    if (clickCount >= 3) {
                        // 3 clicks done — wait for the hold
                        state = State::HOLD_AFTER_TRIPLE;
                        pressStart = now; // will re-press for hold
                    } else {
                        // Wait for more clicks
                        state = State::WAIT_MULTI;
                    }
                }
                break;

            // ── HOLD_AFTER_TRIPLE: 3 clicks done, need to press+hold 4s ──
            case State::HOLD_AFTER_TRIPLE:
                if (raw) {
                    if (now - pressStart >= 4000) {
                        gesture = ButtonGesture::TRIPLE_AND_HOLD;
                        Serial.println("[btn] TRIPLE+HOLD — factory reset");
                        state = State::IDLE;
                        clickCount = 0;
                        return true;
                    }
                } else {
                    // They pressed but released too early after triple
                    if (now - releaseTs > BUTTON_DOUBLE_GAP_MS) {
                        // Timeout — treat as short press
                        gesture = ButtonGesture::SHORT_PRESS;
                        Serial.println("[btn] SHORT (triple timeout)");
                        state = State::IDLE;
                        clickCount = 0;
                        return true;
                    }
                    // If they press again quickly, start the hold timer
                    if (raw) {
                        pressStart = now;
                    }
                }
                break;
        }

        return false;
    }
}
