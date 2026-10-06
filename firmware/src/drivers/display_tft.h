// src/drivers/display_tft.h
// ILI9341 2.8" TFT color display — replaces the small SSD1306 OLED.
// Same responsibility as the previous OLED driver: module status + KPIs.
// Student confirmation still lives on the 16×2 LCD.
//
// API mirrors display_oled 1:1 for drop-in replacement in tasks/main.
#pragma once

#include <Arduino.h>
#include "types.h"

namespace display_tft {

    enum class Mode : uint8_t {
        BOOT,
        PROVISIONING,   // shows pairing code large + QR
        WIFI_SETUP,     // shows QR for joining AP + instructions
        IDLE_KPIS,
        DIAG,           // detailed: uptime, fw, IP, RSSI
        DORMANT,        // outside operating window
        ERROR,          // recoverable error banner
        SCAN_TOAST,     // brief overlay after a scan (auto-revert to IDLE)
    };

    bool begin();

    void setMode(Mode m);
    void setPairingCode(const char* code);
    void setKpis(const OledKpis& kpis);
    void setErrorMessage(const char* code, const char* detail);
    void setDormantInfo(const char* nextWindowStr);

    // QR for WiFi join: WIFI:T:WPA;S:<ssid>;P:<pass>;;
    void setWifiQr(const char* qrData, const char* ssid, const char* pass);

    // Show a full-screen scan result for ~3s then revert to previous mode.
    // decisionCode:
    //   0=PRESENTE  1=TARDANZA  2=SALIDA  3=FUERA_DE_HORA
    //   4=NO_REGISTRADO  5=OFFLINE  6=SIN_JORNADA  7=ERROR_SERVER
    //   8=QR_INVALIDO   9=REFORZAMIENTO
    // extraMsg is an optional sub-label (e.g. student first name from backend
    // or a hint like "sin conexion"). Pass nullptr to skip.
    void showScanToast(const char* studentName, const char* gradeSection,
                       const char* timeStr, uint8_t decisionCode,
                       const char* extraMsg = nullptr);

    // Update the "Ultimo scan" indicator on the dashboard footer (persistent
    // until the next scan). Called from tasks after a scan is processed.
    void setLastScan(const char* studentName, const char* timeStr,
                     uint8_t decisionCode);

    // Optional: current device IP address shown on DIAG.
    void setDeviceIp(const char* ip);

    // Called from the display task.
    void renderTick();

    // Optional backlight control (0..255 duty). Uses LEDC PWM.
    void setBacklight(uint8_t duty);
}
