// src/drivers/display_lcd.cpp
#include "drivers/display_lcd.h"
#include "drivers/i2c_bus.h"
#include "config.h"
#include <LiquidCrystal_I2C.h>

namespace display_lcd {

    static LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

    static Mode               mode = Mode::BOOT;
    static LcdConfirmation    lastConfirm = {};
    static uint32_t           confirmShownAt = 0;
    static uint32_t           scansToday = 0;
    static uint32_t           pendingOffline = 0;
    static char               idleTime[8] = "--:--";
    static char               errL1[17] = {0};
    static char               errL2[17] = {0};
    static bool               dirty = true;
    static uint32_t           lastRenderMs = 0;

    bool begin() {
        i2c::Lock lock;
        if (!lock.acquired) return false;
        lcd.init();
        lcd.backlight();
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("TRAZZO IoT");
        lcd.setCursor(0, 1);
        lcd.print("booting...");
        return true;
    }

    void setMode(Mode m) {
        if (mode != m) {
            mode = m;
            dirty = true;
            if (m == Mode::CONFIRM) confirmShownAt = millis();
        }
    }

    void setIdleStats(uint32_t scans, const char* timeHHMM) {
        scansToday = scans;
        if (timeHHMM) {
            strncpy(idleTime, timeHHMM, sizeof(idleTime) - 1);
            idleTime[sizeof(idleTime) - 1] = 0;
        }
        if (mode == Mode::IDLE) dirty = true;
    }

    void setOfflineCount(uint32_t pending) {
        pendingOffline = pending;
        if (mode == Mode::OFFLINE || mode == Mode::IDLE) dirty = true;
    }

    void setConfirmation(const LcdConfirmation& c) {
        lastConfirm = c;
        setMode(Mode::CONFIRM);
        dirty = true;
    }

    void setErrorMessage(const char* l1, const char* l2) {
        strncpy(errL1, l1 ? l1 : "", 16); errL1[16] = 0;
        strncpy(errL2, l2 ? l2 : "", 16); errL2[16] = 0;
        setMode(Mode::ERROR);
        dirty = true;
    }

    // ─── helpers ────────────────────────────────────────────────────
    static void writeLine(uint8_t row, const char* text) {
        lcd.setCursor(0, row);
        char buf[17];
        size_t n = strnlen(text, 16);
        memcpy(buf, text, n);
        memset(buf + n, ' ', 16 - n);
        buf[16] = 0;
        lcd.print(buf);
    }

    static const char* decisionLabel(AttendanceDecision d) {
        switch (d) {
            case AttendanceDecision::PRESENTE:                return "PRESENTE";
            case AttendanceDecision::TARDANZA:                return "TARDANZA";
            case AttendanceDecision::SALIDA:                  return "SALIDA";
            case AttendanceDecision::REFORZAMIENTO:           return "REFUERZO";
            case AttendanceDecision::FUERA_DE_HORA:           return "FUERA HORA";
            case AttendanceDecision::ESTUDIANTE_NO_ENCONTRADO:return "NO REG.";
            case AttendanceDecision::JORNADA_NO_ACTIVA:       return "SIN JORNADA";
            default:                                          return "ERROR";
        }
    }

    static void renderIdle() {
        // Alternate every 5 seconds between "LISTO - ESCANEAR" and "TRAZZO IoT"
        // so the display doesn't get burnt in and to make the module feel alive.
        char l1[17], l2[17];
        bool phaseA = ((millis() / 5000) % 2) == 0;
        if (phaseA) {
            snprintf(l1, sizeof(l1), "LISTO - ESCANEAR");
        } else {
            snprintf(l1, sizeof(l1), "TRAZZO Asistencia");
        }
        snprintf(l2, sizeof(l2), "%s  Reg:%lu", idleTime, (unsigned long)scansToday);
        writeLine(0, l1);
        writeLine(1, l2);
    }

    static void renderConfirm() {
        // Line 1: nombre completo (truncado a 16). Sin grado/sección.
        writeLine(0, lastConfirm.studentName);

        // Line 2: TIPO + HH:MM (derecha). El label tiene máx 11 chars
        // ("SIN JORNADA"); la hora ocupa 5 → cabe en 16 con 1 espacio buffer.
        char l2[17];
        const char* label = decisionLabel(lastConfirm.decision);
        const char* tm    = lastConfirm.timeStr[0] ? lastConfirm.timeStr : "";
        if (tm[0]) {
            snprintf(l2, sizeof(l2), "%-10s %5s", label, tm);
        } else {
            snprintf(l2, sizeof(l2), "%s", label);
        }
        writeLine(1, l2);
    }

    static void renderError() {
        writeLine(0, errL1);
        writeLine(1, errL2);
    }

    static void renderOffline() {
        char l1[17] = "MODO OFFLINE";
        char l2[17];
        snprintf(l2, sizeof(l2), "Pendientes:%lu", (unsigned long)pendingOffline);
        writeLine(0, l1);
        writeLine(1, l2);
    }

    static void renderDormant() {
        writeLine(0, "INACTIVO");
        writeLine(1, idleTime);
    }

    void renderTick() {
        // Auto-exit CONFIRM after duration
        if (mode == Mode::CONFIRM && millis() - confirmShownAt > LCD_CONFIRM_DURATION_MS) {
            setMode(Mode::IDLE);
        }

        // Idle mode alternates message every 5s → force redraw on phase change.
        static bool prevPhase = false;
        if (mode == Mode::IDLE) {
            bool phase = ((millis() / 5000) % 2) == 0;
            if (phase != prevPhase) { dirty = true; prevPhase = phase; }
        }

        const uint32_t now = millis();
        // Fast path: any pending "dirty" render (nueva confirmación, cambio
        // de modo) va inmediatamente. Sin dirty coalesce a 200 ms para no
        // saturar el bus I2C con re-render del mismo contenido.
        if (!dirty && (now - lastRenderMs < 200)) return;

        i2c::Lock lock;
        if (!lock.acquired) return;

        switch (mode) {
            case Mode::BOOT:
                writeLine(0, "TRAZZO IoT");
                writeLine(1, "booting...");
                break;
            case Mode::IDLE:    renderIdle();   break;
            case Mode::CONFIRM: renderConfirm(); break;
            case Mode::ERROR:   renderError();  break;
            case Mode::OFFLINE: renderOffline(); break;
            case Mode::DORMANT: renderDormant(); break;
        }

        dirty = false;
        lastRenderMs = now;
    }
}
