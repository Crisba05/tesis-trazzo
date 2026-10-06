// src/drivers/buzzer.cpp
// Uses LEDC directly (channel 1) instead of Arduino's tone(), which was
// producing `ledc_get_duty(...): LEDC is not initialized` warnings when the
// TFT backlight setup left channel 0 in an odd state. Channel 1 is dedicated.
#include "drivers/buzzer.h"
#include "config.h"

namespace buzzer {
    static bool muted = false;
    static constexpr uint8_t  BZ_CH   = 1;
    static constexpr uint8_t  BZ_BITS = 10;
    static bool               initOk  = false;

    void begin() {
        pinMode(pins::BUZZER, OUTPUT);
        digitalWrite(pins::BUZZER, LOW);
        // Init LEDC channel 1 at 2 kHz base — real freq is set per note.
        ledcSetup(BZ_CH, 2000, BZ_BITS);
        ledcAttachPin(pins::BUZZER, BZ_CH);
        ledcWrite(BZ_CH, 0);
        initOk = true;
    }

    void tone(uint16_t freqHz, uint16_t durationMs) {
        if (muted || durationMs == 0 || !initOk) return;
        ledcWriteTone(BZ_CH, freqHz);
        ledcWrite(BZ_CH, 1 << (BZ_BITS - 1));   // 50% duty
        delay(durationMs);
        ledcWrite(BZ_CH, 0);
    }

    void patternOk() {
        tone(2200, 80);
        delay(40);
        tone(2800, 80);
    }

    void patternError() {
        tone(400, 250);
    }

    void patternWarn() {
        tone(1500, 150);
    }

    void patternBoot() {
        tone(2000, 60);
    }

    void patternOffline() {
        tone(1200, 100);
        delay(50);
        tone(1200, 100);
    }

    // ─── Decision-specific patterns ─────────────────────────────────────
    void patternPresent() {
        tone(2800, 80);
    }

    void patternLate() {
        tone(1800, 60);
        delay(40);
        tone(1800, 60);
    }

    void patternReinforcement() {
        tone(2200, 200);
    }

    void patternExit() {
        tone(3200, 100);
    }

    void patternNoRegister() {
        tone(400, 60); delay(40);
        tone(400, 60); delay(40);
        tone(400, 60);
    }

    void patternOutOfHour() {
        tone(300, 300);
    }
}
