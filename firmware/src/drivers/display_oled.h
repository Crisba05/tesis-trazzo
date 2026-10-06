// src/drivers/display_oled.h
// Screen driver for module status + KPIs (per PLAN_V2 §5).
// LCD handles student confirmation; this display is for staff diagnostics.
//
// Physical hardware: since 2026-07 the small SSD1306 was replaced by the
// ILI9341 2.8" TFT. When TRAZZO_USE_TFT is defined (esp32-dev env) the
// display_oled namespace becomes an alias of display_tft — zero changes
// required at call sites.
//
// Wokwi simulation still uses the SSD1306 impl (display_oled.cpp).
#pragma once

#include <Arduino.h>
#include "types.h"

#if defined(TRAZZO_USE_TFT)
    #include "drivers/display_tft.h"
    namespace display_oled = display_tft;
#else

namespace display_oled {

    enum class Mode : uint8_t {
        BOOT,
        PROVISIONING,   // shows pairing code large
        WIFI_SETUP,     // shows QR for joining AP + instructions
        IDLE_KPIS,
        DIAG,           // detailed: uptime, fw, IP, RSSI
        DORMANT,        // outside operating window
        ERROR,          // recoverable error banner
    };

    bool begin();

    void setMode(Mode m);
    void setPairingCode(const char* code);
    void setKpis(const OledKpis& kpis);
    void setErrorMessage(const char* code, const char* detail);
    void setDormantInfo(const char* nextWindowStr);

    // QR for WiFi join: data string is the standard WIFI:T:WPA;S:..;P:..;; format.
    // ssid is shown as a label below the QR.
    void setWifiQr(const char* qrData, const char* ssid, const char* pass);

    // Call regularly from a low-priority render task.
    void renderTick();
}

#endif  // TRAZZO_USE_TFT
