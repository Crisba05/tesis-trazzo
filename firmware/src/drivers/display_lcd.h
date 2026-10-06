// src/drivers/display_lcd.h
// LCD 16x2 — student confirmation messages (per PLAN_V2 §5).
#pragma once

#include <Arduino.h>
#include "types.h"

namespace display_lcd {

    enum class Mode : uint8_t {
        BOOT,
        IDLE,           // "LISTO - ESCANEAR" + clock & scan count
        CONFIRM,        // student confirmation banner
        ERROR,
        OFFLINE,
        DORMANT,
    };

    bool begin();

    void setMode(Mode m);
    void setIdleStats(uint32_t scansToday, const char* timeHHMM);
    void setOfflineCount(uint32_t pending);
    void setConfirmation(const LcdConfirmation& c);
    void setErrorMessage(const char* l1, const char* l2);

    void renderTick();
}
