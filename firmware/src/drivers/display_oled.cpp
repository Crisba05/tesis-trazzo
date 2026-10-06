// src/drivers/display_oled.cpp
// SSD1306 128×64 OLED impl. On esp32-dev (TRAZZO_USE_TFT=1) this file
// compiles empty — the TFT driver in display_tft.cpp handles the module UI.
#include "drivers/display_oled.h"

#if !defined(TRAZZO_USE_TFT)

#include "drivers/i2c_bus.h"
#include "config.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <qrcode.h>

namespace display_oled {

    static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
    static Mode             mode = Mode::BOOT;
    static OledKpis         lastKpis = {};
    static char             pairingCode[10] = {0};
    static char             errCode[12]  = {0};
    static char             errDetail[40] = {0};
    static char             dormantInfo[24] = {0};
    static char             qrData[200] = {0};
    static char             qrSsid[24] = {0};
    static char             qrPass[24] = {0};
    static uint32_t         lastRenderMs = 0;
    static bool             dirty = true;

    bool begin() {
        i2c::Lock lock;
        if (!lock.acquired) return false;
        if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
            return false;
        }
        oled.clearDisplay();
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextSize(1);
        oled.setCursor(0, 0);
        oled.println(F("TRAZZO IoT"));
        oled.display();
        return true;
    }

    void setMode(Mode m) {
        if (mode != m) {
            mode = m;
            dirty = true;
        }
    }

    void setPairingCode(const char* code) {
        if (code) {
            strncpy(pairingCode, code, sizeof(pairingCode) - 1);
            pairingCode[sizeof(pairingCode) - 1] = 0;
            dirty = true;
        }
    }

    void setKpis(const OledKpis& kpis) {
        lastKpis = kpis;
        if (mode == Mode::IDLE_KPIS) dirty = true;
    }

    void setErrorMessage(const char* code, const char* detail) {
        strncpy(errCode,  code   ? code   : "",  sizeof(errCode)   - 1); errCode[sizeof(errCode) - 1] = 0;
        strncpy(errDetail, detail ? detail : "", sizeof(errDetail) - 1); errDetail[sizeof(errDetail) - 1] = 0;
        dirty = true;
    }

    void setDormantInfo(const char* nextWindowStr) {
        strncpy(dormantInfo, nextWindowStr ? nextWindowStr : "", sizeof(dormantInfo) - 1);
        dormantInfo[sizeof(dormantInfo) - 1] = 0;
        dirty = true;
    }

    void setWifiQr(const char* qrPayload, const char* ssid, const char* pass) {
        strncpy(qrData, qrPayload ? qrPayload : "", sizeof(qrData) - 1);
        qrData[sizeof(qrData) - 1] = 0;
        strncpy(qrSsid, ssid ? ssid : "", sizeof(qrSsid) - 1);
        qrSsid[sizeof(qrSsid) - 1] = 0;
        strncpy(qrPass, pass ? pass : "", sizeof(qrPass) - 1);
        qrPass[sizeof(qrPass) - 1] = 0;
        dirty = true;
    }

    // ────── render helpers ──────────────────────────────────────────
    static void drawHeader(const char* label, bool wifi, bool ntp) {
        oled.setTextSize(1);
        oled.setCursor(0, 0);
        oled.print(label);
        // Right-aligned status icons
        oled.setCursor(OLED_WIDTH - 30, 0);
        oled.print(wifi ? F("W") : F("-"));
        oled.print(ntp  ? F("N") : F("-"));
        oled.drawLine(0, 10, OLED_WIDTH - 1, 10, SSD1306_WHITE);
    }

    static void renderBoot() {
        oled.clearDisplay();
        oled.setTextSize(2);
        oled.setCursor(0, 4);
        oled.println(F("TRAZZO"));
        oled.setTextSize(1);
        oled.setCursor(0, 30);
        oled.print(F("booting..."));
        oled.setCursor(0, OLED_HEIGHT - 8);
        oled.print(F("fw "));
        oled.print(FIRMWARE_VERSION);
    }

    static void renderProvisioning() {
        oled.clearDisplay();
        drawHeader("PAIRING", false, false);
        oled.setTextSize(2);
        oled.setCursor(0, 16);
        oled.print(pairingCode[0] ? pairingCode : "------");
        oled.setTextSize(1);
        oled.setCursor(0, 42);
        oled.println(F("Ingrese en panel"));
        oled.println(F("del colegio"));
    }

    static void renderIdleKpis() {
        oled.clearDisplay();
        drawHeader(lastKpis.deviceIdShort[0] ? lastKpis.deviceIdShort : "TRAZZO",
                   lastKpis.wifiConnected, lastKpis.ntpSynced);

        oled.setTextSize(2);
        oled.setCursor(0, 14);
        oled.print(lastKpis.timeStr[0] ? lastKpis.timeStr : "--:--");

        // Header right side: total roster (Est:NNN) — replaces RSSI.
        oled.setTextSize(1);
        oled.setCursor(80, 16);
        {
            char h[16];
            snprintf(h, sizeof(h), "Est:%lu", (unsigned long)lastKpis.studentsCached);
            oled.print(h);
        }

        // Row 1: entradas + tardanzas — remote KPIs from backend.
        // "--" until first successful attendance-summary pull.
        char line[20];
        oled.setCursor(0, 34);
        if (lastKpis.hasRemoteKpis) {
            snprintf(line, sizeof(line), "Ent:%lu", (unsigned long)lastKpis.remoteEntries);
        } else {
            snprintf(line, sizeof(line), "Ent:--");
        }
        oled.print(line);

        oled.setCursor(64, 34);
        if (lastKpis.hasRemoteKpis) {
            snprintf(line, sizeof(line), "Tar:%lu", (unsigned long)lastKpis.remoteLates);
        } else {
            snprintf(line, sizeof(line), "Tar:--");
        }
        oled.print(line);

        // Row 2: sin registrar (remote) + pending offline queue (local)
        oled.setCursor(0, 44);
        if (lastKpis.hasRemoteKpis) {
            snprintf(line, sizeof(line), "S/R:%lu", (unsigned long)lastKpis.remoteUnregistered);
        } else {
            snprintf(line, sizeof(line), "S/R:--");
        }
        oled.print(line);

        oled.setCursor(64, 44);
        snprintf(line, sizeof(line), "Pnd:%lu", (unsigned long)lastKpis.pendingOffline);
        oled.print(line);

        // Row 3: footer — firmware version + total scans today
        oled.setCursor(0, OLED_HEIGHT - 8);
        snprintf(line, sizeof(line), "Sc:%lu", (unsigned long)lastKpis.scansToday);
        oled.print(line);

        oled.setCursor(64, OLED_HEIGHT - 8);
        oled.print(F("fw "));
        oled.print(FIRMWARE_VERSION);
    }

    static void renderDiag() {
        oled.clearDisplay();
        drawHeader("DIAG", lastKpis.wifiConnected, lastKpis.ntpSynced);
        char buf[24];

        // Left column
        oled.setCursor(0, 14);
        snprintf(buf, sizeof(buf), "DID %s", lastKpis.deviceIdShort);
        oled.println(buf);

        snprintf(buf, sizeof(buf), "Up  %lus", (unsigned long)lastKpis.uptimeSec);
        oled.setCursor(0, 24); oled.println(buf);

        snprintf(buf, sizeof(buf), "Pen %lu", (unsigned long)lastKpis.pendingOffline);
        oled.setCursor(0, 34); oled.println(buf);

        snprintf(buf, sizeof(buf), "Mem %luK", (unsigned long)lastKpis.freeHeapKb);
        oled.setCursor(0, 44); oled.println(buf);

        // Right column — cache backup counts
        snprintf(buf, sizeof(buf), "Est %lu", (unsigned long)lastKpis.studentsCached);
        oled.setCursor(70, 14); oled.println(buf);

        snprintf(buf, sizeof(buf), "Jorn %lu", (unsigned long)lastKpis.schedulesCached);
        oled.setCursor(70, 24); oled.println(buf);

        snprintf(buf, sizeof(buf), "RSSI %d", (int)lastKpis.rssi);
        oled.setCursor(70, 34); oled.println(buf);

        oled.setCursor(0, 54);
        oled.print(F("fw "));
        oled.print(FIRMWARE_VERSION);
    }

    static void renderDormant() {
        oled.clearDisplay();
        drawHeader("DORMANT", lastKpis.wifiConnected, lastKpis.ntpSynced);
        oled.setTextSize(2);
        oled.setCursor(0, 16);
        oled.print(lastKpis.timeStr);
        oled.setTextSize(1);
        oled.setCursor(0, 38);
        oled.println(F("Fuera de horario"));
        oled.setCursor(0, 50);
        oled.print(F("next "));
        oled.print(dormantInfo);
    }

    static void renderError() {
        oled.clearDisplay();
        drawHeader("ERROR", lastKpis.wifiConnected, lastKpis.ntpSynced);
        oled.setTextSize(2);
        oled.setCursor(0, 14);
        oled.print(errCode[0] ? errCode : "ERR");
        oled.setTextSize(1);
        oled.setCursor(0, 38);
        oled.println(errDetail);
    }

    static void renderWifiSetup() {
        oled.clearDisplay();

        if (qrData[0]) {
            // Version 3 (29x29) — 53 bytes byte-mode at ECC_LOW (enough for WIFI:...)
            QRCode qrcode;
            uint8_t qrBuf[qrcode_getBufferSize(3)];
            int8_t err = qrcode_initText(&qrcode, qrBuf, 3, ECC_LOW, qrData);

            if (err == 0) {
                // Standard QR = BLACK modules on WHITE background.
                // Many phones can't read inverted (white-on-black) QR codes.
                const int qrSize = qrcode.size;  // 29 for v3
                const int scale  = 2;
                const int quiet  = 2;  // quiet-zone border (px) — needed for scanner detection
                const int totalPx = qrSize * scale + quiet * 2;  // 62
                const int xStart = 0;
                const int yStart = (OLED_HEIGHT - totalPx) / 2;  // vertically center (=1)

                // 1) White background rectangle (includes quiet zone)
                oled.fillRect(xStart, yStart, totalPx, totalPx, SSD1306_WHITE);

                // 2) Draw QR modules as BLACK on the white background
                const int xOff = xStart + quiet;
                const int yOff = yStart + quiet;
                for (int y = 0; y < qrSize; y++) {
                    for (int x = 0; x < qrSize; x++) {
                        if (qrcode_getModule(&qrcode, x, y)) {
                            oled.fillRect(xOff + x * scale, yOff + y * scale,
                                          scale, scale, SSD1306_BLACK);
                        }
                    }
                }
            } else {
                oled.setTextSize(1);
                oled.setCursor(0, 24);
                oled.printf("QR err: %d", err);
            }
        } else {
            oled.setTextSize(1);
            oled.setCursor(0, 24);
            oled.print(F("(no QR data)"));
        }

        // Right side text (x=66..127, 62 px wide)
        oled.setTextSize(1);
        oled.setCursor(66, 2);
        oled.print(F("Escanea QR"));
        oled.setCursor(66, 14);
        oled.print(F("Red:"));
        oled.setCursor(66, 24);
        oled.print(qrSsid);
        oled.setCursor(66, 38);
        oled.print(F("Clave:"));
        oled.setCursor(66, 48);
        oled.print(qrPass);
    }

    void renderTick() {
        const uint32_t now = millis();
        const uint32_t period = (mode == Mode::IDLE_KPIS) ? OLED_REFRESH_IDLE_MS : 500;
        if (!dirty && (now - lastRenderMs < period)) return;

        i2c::Lock lock;
        if (!lock.acquired) return;

        switch (mode) {
            case Mode::BOOT:         renderBoot();         break;
            case Mode::PROVISIONING: renderProvisioning(); break;
            case Mode::WIFI_SETUP:   renderWifiSetup();    break;
            case Mode::IDLE_KPIS:    renderIdleKpis();     break;
            case Mode::DIAG:         renderDiag();         break;
            case Mode::DORMANT:      renderDormant();      break;
            case Mode::ERROR:        renderError();        break;
        }
        oled.display();

        lastRenderMs = now;
        dirty = false;
    }
}

#endif  // !TRAZZO_USE_TFT
