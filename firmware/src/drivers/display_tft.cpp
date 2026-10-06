// src/drivers/display_tft.cpp
// ILI9341 320×240 landscape UI. Anti-flicker: full clear only when mode
// changes; individual KPI values erase-and-redraw via background color.

#include "drivers/display_tft.h"
#include "config.h"
#include <TFT_eSPI.h>
#include <qrcode.h>

namespace display_tft {

    // ─── Colors (RGB565) ───────────────────────────────────────────────
    static constexpr uint16_t COLOR_BG     = 0x0000;   // black
    static constexpr uint16_t COLOR_FG     = 0xFFFF;   // white
    static constexpr uint16_t COLOR_HEADER = 0x0451;   // deep blue
    static constexpr uint16_t COLOR_ACCENT = 0x07FF;   // cyan
    static constexpr uint16_t COLOR_OK     = 0x07E0;   // green
    static constexpr uint16_t COLOR_WARN   = 0xFD20;   // amber
    static constexpr uint16_t COLOR_ERR    = 0xF800;   // red
    static constexpr uint16_t COLOR_MUTED  = 0x7BEF;   // grey
    static constexpr uint16_t COLOR_CARD   = 0x1082;   // dark grey (card bg)
    static constexpr uint16_t COLOR_BORDER = 0x3186;   // card border

    // ─── State ─────────────────────────────────────────────────────────
    static TFT_eSPI  tft;
    static Mode      mode = Mode::BOOT;
    static Mode      renderedMode = Mode::BOOT;
    static OledKpis  lastKpis = {};
    static char      pairingCode[10] = {0};
    static char      errCode[12]     = {0};
    static char      errDetail[40]   = {0};
    static char      dormantInfo[24] = {0};
    static char      qrData[200]     = {0};
    static char      qrSsid[24]      = {0};
    static char      qrPass[24]      = {0};
    static uint32_t  lastRenderMs    = 0;
    static bool      dirty           = true;
    static bool      modeChanged     = true;

    // Backlight (LEDC PWM channel 0)
    static constexpr uint8_t  BL_CHANNEL   = 0;
    static constexpr uint32_t BL_FREQ_HZ   = 5000;
    static constexpr uint8_t  BL_RESOLUTION = 8;

    bool begin() {
        Serial.println("[tft] init ILI9341 320x240 landscape...");
        tft.init();
        tft.setRotation(1);          // landscape 320×240
        tft.fillScreen(COLOR_BG);
        Serial.printf("[tft] init OK — w=%d h=%d\n",
                      (int)tft.width(), (int)tft.height());

        // Boot splash
        tft.setTextColor(COLOR_ACCENT, COLOR_BG);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("TRAZZO", 160, 100, 6);
        tft.setTextColor(COLOR_MUTED, COLOR_BG);
        tft.drawString("IoT Attendance", 160, 150, 2);
        tft.drawString(FIRMWARE_VERSION, 160, 220, 2);
        tft.setTextDatum(TL_DATUM);   // restore

        Serial.println("[tft] begin() done — splash drawn");
        return true;
    }

    void setBacklight(uint8_t /*duty*/) {
        // No-op while backlight is wired to 3.3V directly. Re-enable LEDC
        // here if you wire the LED pin back to GPIO 32.
    }

    void setMode(Mode m) {
        if (mode != m) {
            mode = m;
            dirty = true;
            modeChanged = true;
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
        if (mode == Mode::IDLE_KPIS || mode == Mode::DIAG) dirty = true;
    }

    void setErrorMessage(const char* code, const char* detail) {
        strncpy(errCode,   code   ? code   : "", sizeof(errCode)   - 1); errCode[sizeof(errCode) - 1] = 0;
        strncpy(errDetail, detail ? detail : "", sizeof(errDetail) - 1); errDetail[sizeof(errDetail) - 1] = 0;
        dirty = true;
    }

    void setDormantInfo(const char* nextWindowStr) {
        strncpy(dormantInfo, nextWindowStr ? nextWindowStr : "", sizeof(dormantInfo) - 1);
        dormantInfo[sizeof(dormantInfo) - 1] = 0;
        dirty = true;
    }

    void setWifiQr(const char* qrPayload, const char* ssid, const char* pass) {
        strncpy(qrData, qrPayload ? qrPayload : "", sizeof(qrData) - 1); qrData[sizeof(qrData) - 1] = 0;
        strncpy(qrSsid, ssid       ? ssid       : "", sizeof(qrSsid) - 1); qrSsid[sizeof(qrSsid) - 1] = 0;
        strncpy(qrPass, pass       ? pass       : "", sizeof(qrPass) - 1); qrPass[sizeof(qrPass) - 1] = 0;
        dirty = true;
    }

    // ─── Scan toast state ─────────────────────────────────────────────
    static char     _toastName[48]  = {0};
    static char     _toastGrade[8]  = {0};
    static char     _toastTime[8]   = {0};
    static uint8_t  _toastDecision  = 0;
    static char     _toastExtra[24] = {0};
    static uint32_t _toastShownAt   = 0;
    static Mode     _toastReturnTo  = Mode::IDLE_KPIS;
    static constexpr uint32_t TOAST_DURATION_MS = 2800;

    void showScanToast(const char* name, const char* grade, const char* timeStr,
                       uint8_t decisionCode, const char* extraMsg) {
        strncpy(_toastName,  name    ? name    : "", sizeof(_toastName)  - 1); _toastName[sizeof(_toastName) - 1] = 0;
        strncpy(_toastGrade, grade   ? grade   : "", sizeof(_toastGrade) - 1); _toastGrade[sizeof(_toastGrade) - 1] = 0;
        strncpy(_toastTime,  timeStr ? timeStr : "", sizeof(_toastTime)  - 1); _toastTime[sizeof(_toastTime) - 1] = 0;
        strncpy(_toastExtra, extraMsg ? extraMsg : "", sizeof(_toastExtra) - 1); _toastExtra[sizeof(_toastExtra) - 1] = 0;
        _toastDecision = decisionCode;
        _toastShownAt  = millis();
        _toastReturnTo = (mode == Mode::SCAN_TOAST) ? _toastReturnTo : mode;
        setMode(Mode::SCAN_TOAST);
    }

    // ─── Last scan indicator (footer of IDLE dashboard) ─────────────────
    static char     _lastScanName[32] = {0};
    static char     _lastScanTime[8]  = {0};
    static uint8_t  _lastScanDecision = 255;   // 255 = never scanned yet

    void setLastScan(const char* name, const char* timeStr, uint8_t decisionCode) {
        strncpy(_lastScanName, name ? name : "", sizeof(_lastScanName) - 1);
        _lastScanName[sizeof(_lastScanName) - 1] = 0;
        strncpy(_lastScanTime, timeStr ? timeStr : "", sizeof(_lastScanTime) - 1);
        _lastScanTime[sizeof(_lastScanTime) - 1] = 0;
        _lastScanDecision = decisionCode;
        if (mode == Mode::IDLE_KPIS) dirty = true;
    }

    // ─── Device IP for DIAG ───────────────────────────────────────────
    static char _deviceIp[16] = {0};
    void setDeviceIp(const char* ip) {
        strncpy(_deviceIp, ip ? ip : "", sizeof(_deviceIp) - 1);
        _deviceIp[sizeof(_deviceIp) - 1] = 0;
        if (mode == Mode::DIAG) dirty = true;
    }

    // ─── Render helpers ────────────────────────────────────────────────

    static void drawHeader(const char* label, bool wifi, bool ntp) {
        tft.fillRect(0, 0, TFT_W, 28, COLOR_HEADER);
        tft.setTextColor(COLOR_FG, COLOR_HEADER);
        tft.setTextDatum(ML_DATUM);
        tft.drawString(label, 8, 14, 4);

        // Right-aligned status pills
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(wifi ? COLOR_OK : COLOR_ERR, COLOR_HEADER);
        tft.drawString(wifi ? "WiFi" : "wifi", TFT_W - 60, 14, 2);
        tft.setTextColor(ntp ? COLOR_OK : COLOR_MUTED, COLOR_HEADER);
        tft.drawString(ntp ? "NTP" : "ntp", TFT_W - 10, 14, 2);
        tft.setTextDatum(TL_DATUM);
    }

    // ─── Anti-flicker state — keep last drawn values so we only redraw diffs ─
    struct IdlePrev {
        char header[8];
        bool wifi;
        bool ntp;
        char clock[8];
        char rssi[8];
        char heap[8];
        char scans[12];
        char pend[12];
        char estud[12];
        char jorn[12];
        char foot[48];
    };
    static IdlePrev prevIdle = {};

    static bool strDiffers(const char* a, const char* b) {
        return strncmp(a, b, 8) != 0;
    }

    // Draw one KPI card slot. Draws label/frame once (on mode change), and only
    // rewrites the numeric value when it changed. Uses setTextPadding so text
    // erases its own background — no fillRect flicker.
    static void drawKpiIfChanged(int col, int row, const char* label,
                                 const char* newValue, char* prevValue, uint16_t valueColor) {
        constexpr int cols   = 3;
        constexpr int gutter = 6;
        constexpr int gridX  = 6;
        constexpr int gridY  = 90;
        constexpr int cardW  = (TFT_W - gridX * 2 - gutter * (cols - 1)) / cols;
        constexpr int cardH  = 60;

        int x = gridX + col * (cardW + gutter);
        int y = gridY + row * (cardH + gutter);

        if (modeChanged) {
            tft.fillRoundRect(x, y, cardW, cardH, 6, COLOR_CARD);
            tft.drawRoundRect(x, y, cardW, cardH, 6, COLOR_BORDER);
            tft.setTextColor(COLOR_MUTED, COLOR_CARD);
            tft.setTextDatum(TL_DATUM);
            tft.drawString(label, x + 6, y + 6, 2);
            prevValue[0] = 0;  // force redraw
        }

        if (strcmp(newValue, prevValue) == 0) return;   // no change → skip

        tft.setTextColor(valueColor, COLOR_CARD);
        tft.setTextDatum(TL_DATUM);
        tft.setTextPadding(cardW - 12);      // auto-erase old text on same line
        tft.drawString(newValue, x + 6, y + 28, 4);
        tft.setTextPadding(0);

        strncpy(prevValue, newValue, 11);
        prevValue[11] = 0;
    }

    static void renderIdleKpis() {
        if (modeChanged) {
            tft.fillScreen(COLOR_BG);
            memset(&prevIdle, 0, sizeof(prevIdle));
        }

        const char* headerLabel = lastKpis.deviceIdShort[0] ? lastKpis.deviceIdShort : "TRAZZO";
        if (modeChanged
            || strDiffers(prevIdle.header, headerLabel)
            || prevIdle.wifi != lastKpis.wifiConnected
            || prevIdle.ntp  != lastKpis.ntpSynced) {
            drawHeader(headerLabel, lastKpis.wifiConnected, lastKpis.ntpSynced);
            strncpy(prevIdle.header, headerLabel, sizeof(prevIdle.header) - 1);
            prevIdle.wifi = lastKpis.wifiConnected;
            prevIdle.ntp  = lastKpis.ntpSynced;
        }

        // Big clock — color changes with sync/offline status:
        //   green   = WiFi+NTP OK, no pending scans
        //   amber   = pending scans (offline queue)
        //   red     = WiFi disconnected
        //   white   = default (NTP not synced yet)
        const char* clockStr = lastKpis.timeStr[0] ? lastKpis.timeStr : "--:--";
        uint16_t clockColor = COLOR_FG;
        if (!lastKpis.wifiConnected)         clockColor = COLOR_ERR;
        else if (lastKpis.pendingOffline > 0) clockColor = COLOR_WARN;
        else if (lastKpis.wifiConnected && lastKpis.ntpSynced) clockColor = COLOR_OK;
        if (modeChanged || strcmp(prevIdle.clock, clockStr) != 0) {
            tft.setTextColor(clockColor, COLOR_BG);
            tft.setTextDatum(MC_DATUM);
            tft.setTextPadding(220);
            tft.drawString(clockStr, 160, 60, 7);
            tft.setTextPadding(0);
            tft.setTextDatum(TL_DATUM);
            strncpy(prevIdle.clock, clockStr, sizeof(prevIdle.clock) - 1);
        }

        // KPI grid 3×2 — TOP row (backend authoritative): ENTRADAS / TARDANZAS
        // / SIN REG. from /iot-roster/attendance-summary. Grey "--" until the
        // first successful pull. BOTTOM row (local): ESTUD / PEND / SCANS.
        // RSSI/heap moved to DIAG mode (short-press).
        char buf[16];
        const char* MISSING = "--";
        const uint16_t GREY = COLOR_MUTED;

        if (lastKpis.hasRemoteKpis) {
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastKpis.remoteEntries);
            drawKpiIfChanged(0, 0, "ENTRADAS", buf, prevIdle.rssi, COLOR_OK);

            snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastKpis.remoteLates);
            drawKpiIfChanged(1, 0, "TARDANZAS", buf, prevIdle.heap,
                             lastKpis.remoteLates ? COLOR_WARN : COLOR_FG);

            snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastKpis.remoteUnregistered);
            drawKpiIfChanged(2, 0, "SIN REG.", buf, prevIdle.scans,
                             lastKpis.remoteUnregistered ? COLOR_ERR : COLOR_FG);
        } else {
            drawKpiIfChanged(0, 0, "ENTRADAS",  MISSING, prevIdle.rssi,  GREY);
            drawKpiIfChanged(1, 0, "TARDANZAS", MISSING, prevIdle.heap,  GREY);
            drawKpiIfChanged(2, 0, "SIN REG.",  MISSING, prevIdle.scans, GREY);
        }

        snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastKpis.studentsCached);
        drawKpiIfChanged(0, 1, "ESTUD", buf, prevIdle.estud, COLOR_FG);

        snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastKpis.pendingOffline);
        drawKpiIfChanged(1, 1, "PEND", buf, prevIdle.pend,
                         lastKpis.pendingOffline ? COLOR_WARN : COLOR_FG);

        snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastKpis.scansToday);
        drawKpiIfChanged(2, 1, "SCANS", buf, prevIdle.jorn, COLOR_ACCENT);

        // Last scan indicator (persistent until next scan). Overwrites footer
        // when we have data; otherwise show the fw/uptime line as before.
        if (_lastScanDecision != 255 && _lastScanName[0]) {
            const char* declabel = "OK";
            uint16_t deccolor = COLOR_OK;
            switch (_lastScanDecision) {
                case 0: declabel = "PRESENTE";  deccolor = COLOR_OK;     break;
                case 1: declabel = "TARDANZA";  deccolor = COLOR_WARN;   break;
                case 2: declabel = "SALIDA";    deccolor = COLOR_ACCENT; break;
                case 3: declabel = "F.HORA";    deccolor = COLOR_ERR;    break;
                case 4: declabel = "NO REG.";   deccolor = COLOR_ERR;    break;
                case 6: declabel = "S/JORN.";   deccolor = COLOR_WARN;   break;
                case 9: declabel = "REFUERZO";  deccolor = COLOR_ACCENT; break;
                default: declabel = "?";        deccolor = COLOR_MUTED;  break;
            }
            char foot[80];
            snprintf(foot, sizeof(foot), "Ult: %-14s %s  %s",
                     _lastScanName, _lastScanTime, declabel);
            if (modeChanged || strcmp(prevIdle.foot, foot) != 0) {
                tft.setTextColor(deccolor, COLOR_BG);
                tft.setTextDatum(TL_DATUM);
                tft.setTextPadding(TFT_W - 16);
                tft.drawString(foot, 8, 224, 2);
                tft.setTextPadding(0);
                strncpy(prevIdle.foot, foot, sizeof(prevIdle.foot) - 1);
            }
        } else {
            // No scan yet — show fw + uptime
            char foot[48];
            snprintf(foot, sizeof(foot), "fw %s   up %lus",
                     FIRMWARE_VERSION, (unsigned long)lastKpis.uptimeSec);
            if (modeChanged || strcmp(prevIdle.foot, foot) != 0) {
                tft.setTextColor(COLOR_MUTED, COLOR_BG);
                tft.setTextDatum(TL_DATUM);
                tft.setTextPadding(TFT_W - 16);
                tft.drawString(foot, 8, 224, 2);
                tft.setTextPadding(0);
                strncpy(prevIdle.foot, foot, sizeof(prevIdle.foot) - 1);
            }
        }
    }

    static void renderDiag() {
        if (modeChanged) tft.fillScreen(COLOR_BG);
        drawHeader("DIAG", lastKpis.wifiConnected, lastKpis.ntpSynced);

        char buf[48];
        int y = 44;
        const int lineH = 22;

        auto row = [&](const char* label, const char* value, uint16_t color) {
            tft.fillRect(0, y, TFT_W, lineH, COLOR_BG);
            tft.setTextColor(COLOR_MUTED, COLOR_BG);
            tft.setTextDatum(TL_DATUM);
            tft.drawString(label, 10, y + 3, 2);
            tft.setTextColor(color, COLOR_BG);
            tft.setTextDatum(TR_DATUM);
            tft.drawString(value, TFT_W - 10, y + 3, 4);
            tft.setTextDatum(TL_DATUM);
            y += lineH + 2;
        };

        row("Device ID",  lastKpis.deviceIdShort, COLOR_ACCENT);
        row("IP",         _deviceIp[0] ? _deviceIp : "-", COLOR_FG);
        snprintf(buf, sizeof(buf), "%lus", (unsigned long)lastKpis.uptimeSec);
        row("Uptime",     buf, COLOR_FG);
        snprintf(buf, sizeof(buf), "%lu K", (unsigned long)lastKpis.freeHeapKb);
        row("Free heap",  buf, COLOR_FG);
        snprintf(buf, sizeof(buf), "%d dBm", (int)lastKpis.rssi);
        row("WiFi RSSI",  buf, COLOR_FG);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastKpis.scansToday);
        row("Scans hoy",  buf, COLOR_OK);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastKpis.pendingOffline);
        row("Offline",    buf, lastKpis.pendingOffline ? COLOR_WARN : COLOR_FG);
        snprintf(buf, sizeof(buf), "%lu",  (unsigned long)lastKpis.studentsCached);
        row("Estudiantes",buf, COLOR_FG);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastKpis.schedulesCached);
        row("Jornadas",   buf, COLOR_FG);
    }

    static void renderProvisioning() {
        if (modeChanged) tft.fillScreen(COLOR_BG);
        drawHeader("PAIRING", false, false);

        tft.setTextColor(COLOR_MUTED, COLOR_BG);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Codigo de emparejamiento", 160, 60, 2);

        // Big centered code — no QR, cleaner look
        tft.fillRect(40, 82, 240, 96, COLOR_CARD);
        tft.drawRoundRect(40, 82, 240, 96, 10, COLOR_ACCENT);
        tft.setTextColor(COLOR_ACCENT, COLOR_CARD);
        tft.drawString(pairingCode[0] ? pairingCode : "-----", 160, 130, 7);

        tft.setTextColor(COLOR_FG, COLOR_BG);
        tft.drawString("Ingresa este codigo en el", 160, 195, 2);
        tft.drawString("panel del colegio", 160, 213, 2);
        tft.setTextColor(COLOR_MUTED, COLOR_BG);
        tft.drawString("expira en 15 minutos", 160, 232, 2);
        tft.setTextDatum(TL_DATUM);
    }

    // ─── Scan toast (full-screen result) ────────────────────────────────
    static void renderScanToast() {
        // Choose color + label based on decision code.
        uint16_t banner = COLOR_OK;
        const char* label = "PRESENTE";
        switch (_toastDecision) {
            case 0: banner = COLOR_OK;     label = "PRESENTE";      break;
            case 1: banner = COLOR_WARN;   label = "TARDANZA";      break;
            case 2: banner = COLOR_ACCENT; label = "SALIDA";        break;
            case 3: banner = COLOR_ERR;    label = "FUERA DE HORA"; break;
            case 4: banner = COLOR_ERR;    label = "NO REGISTRADO"; break;
            case 5: banner = COLOR_WARN;   label = "SIN CONEXION";  break;
            case 6: banner = COLOR_WARN;   label = "SIN JORNADA";   break;
            case 7: banner = COLOR_ERR;    label = "ERROR SERVER";  break;
            case 8: banner = COLOR_WARN;   label = "QR INVALIDO";   break;
            case 9: banner = COLOR_ACCENT; label = "REFORZAMIENTO"; break;
            default: break;
        }
        if (modeChanged) tft.fillScreen(COLOR_BG);
        // Full-width banner
        tft.fillRect(0, 0, TFT_W, 60, banner);
        tft.setTextColor(COLOR_FG, banner);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(label, TFT_W / 2, 30, 6);

        // Student name (big)
        tft.fillRect(0, 70, TFT_W, 90, COLOR_BG);
        tft.setTextColor(COLOR_FG, COLOR_BG);
        tft.drawString(_toastName, TFT_W / 2, 110, 4);

        // Optional sub-label (extra message from backend or hint)
        if (_toastExtra[0]) {
            tft.setTextColor(COLOR_MUTED, COLOR_BG);
            tft.drawString(_toastExtra, TFT_W / 2, 148, 2);
        }

        // Grade / time footer
        tft.fillRect(0, 170, TFT_W, 40, COLOR_BG);
        tft.setTextColor(COLOR_MUTED, COLOR_BG);
        char row[24];
        snprintf(row, sizeof(row), "%s  %s",
                 _toastGrade[0] ? _toastGrade : "-",
                 _toastTime[0]  ? _toastTime  : "-");
        tft.drawString(row, TFT_W / 2, 195, 4);
        tft.setTextDatum(TL_DATUM);
    }

    static void renderDormant() {
        if (modeChanged) tft.fillScreen(COLOR_BG);
        drawHeader("DORMANT", lastKpis.wifiConnected, lastKpis.ntpSynced);

        tft.fillRect(0, 40, TFT_W, 100, COLOR_BG);
        tft.setTextColor(COLOR_MUTED, COLOR_BG);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(lastKpis.timeStr, 160, 80, 7);

        tft.setTextColor(COLOR_WARN, COLOR_BG);
        tft.drawString("Fuera de horario", 160, 160, 4);

        tft.setTextColor(COLOR_MUTED, COLOR_BG);
        tft.drawString(dormantInfo[0] ? dormantInfo : "-", 160, 200, 2);
        tft.setTextDatum(TL_DATUM);
    }

    static void renderError() {
        if (modeChanged) tft.fillScreen(COLOR_BG);
        drawHeader("ERROR", lastKpis.wifiConnected, lastKpis.ntpSynced);

        tft.fillRect(20, 60, 280, 140, COLOR_ERR);
        tft.drawRoundRect(20, 60, 280, 140, 8, COLOR_FG);
        tft.setTextColor(COLOR_FG, COLOR_ERR);
        tft.setTextDatum(MC_DATUM);
        tft.drawString(errCode[0] ? errCode : "ERR", 160, 110, 6);
        tft.drawString(errDetail, 160, 170, 2);
        tft.setTextDatum(TL_DATUM);
    }

    static void renderWifiSetup() {
        if (modeChanged) tft.fillScreen(COLOR_BG);
        drawHeader("WiFi SETUP", false, false);

        if (qrData[0]) {
            // QR version 4 (33×33) — up to 78 bytes at ECC_LOW
            QRCode qrcode;
            uint8_t qrBuf[qrcode_getBufferSize(4)];
            int8_t err = qrcode_initText(&qrcode, qrBuf, 4, ECC_LOW, qrData);
            if (err == 0) {
                const int size = qrcode.size;  // 33
                const int scale = 5;           // 33 * 5 = 165 px
                const int quiet = 4;           // 4 * scale is the recommended quiet zone
                const int totalPx = size * scale + quiet * 2 * scale;
                const int xStart = 15;
                const int yStart = 40;

                // White background w/ quiet zone
                tft.fillRect(xStart, yStart, totalPx, totalPx, COLOR_FG);
                const int xOff = xStart + quiet * scale;
                const int yOff = yStart + quiet * scale;
                for (int y = 0; y < size; y++) {
                    for (int x = 0; x < size; x++) {
                        if (qrcode_getModule(&qrcode, x, y)) {
                            tft.fillRect(xOff + x * scale, yOff + y * scale,
                                         scale, scale, COLOR_BG);
                        }
                    }
                } // for
            }
        }

        // Right side info
        tft.setTextColor(COLOR_ACCENT, COLOR_BG);
        tft.setTextDatum(TL_DATUM);
        tft.drawString("Escanea el QR", 210, 40, 2);
        tft.setTextColor(COLOR_MUTED, COLOR_BG);
        tft.drawString("Red:", 210, 90, 2);
        tft.setTextColor(COLOR_FG, COLOR_BG);
        tft.drawString(qrSsid[0] ? qrSsid : "-", 210, 108, 2);
        tft.setTextColor(COLOR_MUTED, COLOR_BG);
        tft.drawString("Clave:", 210, 140, 2);
        tft.setTextColor(COLOR_FG, COLOR_BG);
        tft.drawString(qrPass[0] ? qrPass : "-", 210, 158, 2);
    }

    static void renderBoot() {
        if (modeChanged) {
            tft.fillScreen(COLOR_BG);
            tft.setTextColor(COLOR_ACCENT, COLOR_BG);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("TRAZZO", 160, 90, 6);
            tft.setTextColor(COLOR_MUTED, COLOR_BG);
            tft.drawString("booting...", 160, 150, 4);
            tft.drawString(FIRMWARE_VERSION, 160, 210, 2);
            tft.setTextDatum(TL_DATUM);
        }
    }

    void renderTick() {
        const uint32_t now = millis();

        // Auto-exit SCAN_TOAST after ~3s → return to previous screen.
        if (mode == Mode::SCAN_TOAST && now - _toastShownAt > TOAST_DURATION_MS) {
            setMode(_toastReturnTo);
        }

        // Dynamic modes need periodic refresh (values change over time).
        // Static modes (QR, pairing code, error banner) draw once and stay.
        const bool isDynamic = (mode == Mode::IDLE_KPIS || mode == Mode::DIAG);

        if (isDynamic) {
            const uint32_t period = (mode == Mode::IDLE_KPIS) ? TFT_REFRESH_IDLE_MS : 1000;
            if (now - lastRenderMs < period) return;
        } else {
            // Static — only render on explicit dirty (mode change or new content).
            if (!dirty) return;
        }

        switch (mode) {
            case Mode::BOOT:         renderBoot();         break;
            case Mode::PROVISIONING: renderProvisioning(); break;
            case Mode::WIFI_SETUP:   renderWifiSetup();    break;
            case Mode::IDLE_KPIS:    renderIdleKpis();     break;
            case Mode::DIAG:         renderDiag();         break;
            case Mode::DORMANT:      renderDormant();      break;
            case Mode::ERROR:        renderError();        break;
            case Mode::SCAN_TOAST:   renderScanToast();    break;
        }
        renderedMode = mode;
        modeChanged  = false;
        lastRenderMs = now;
        dirty        = false;
    }
}
