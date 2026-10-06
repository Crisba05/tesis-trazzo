// src/drivers/buzzer.h
#pragma once
#include <Arduino.h>

namespace buzzer {
    void begin();

    // Single tone (blocking).
    void tone(uint16_t freqHz, uint16_t durationMs);

    // Predefined patterns — call from logic task, NOT from ISR.
    void patternOk();          // happy double beep (generic)
    void patternError();       // low buzzer error
    void patternWarn();        // single mid beep (offline / out of window)
    void patternBoot();        // tiny chirp at startup
    void patternOffline();     // saved offline

    // Decision-specific patterns — different sound per attendance outcome so
    // the operator can tell them apart without looking at the display.
    void patternPresent();      // 1 short chirp @ 2800Hz          → PRESENTE
    void patternLate();         // 2 mid chirps @ 1800Hz w/ gap    → TARDANZA
    void patternReinforcement();// 1 sustained mid tone @ 2200Hz   → REFORZAMIENTO
    void patternExit();         // 1 short high @ 3200Hz           → SALIDA
    void patternNoRegister();   // 3 low chirps @ 400Hz            → NO REGISTRADO
    void patternOutOfHour();    // 1 long deep tone @ 300Hz        → FUERA DE HORA
}
