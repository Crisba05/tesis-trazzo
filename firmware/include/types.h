// include/types.h
// Shared structs passed across FreeRTOS tasks via queues.
#pragma once

#include <Arduino.h>

// ─── Scan result from the scanner driver to the logic task ───────────
struct ScanEvent {
    char     code[40];        // raw document number (truncated for safety)
    uint8_t  len;
    uint64_t timestampMs;     // millis() at capture
};

// ─── Student record cached locally ───────────────────────────────────
struct StudentRecord {
    char documentNumber[24];
    char firstName[32];
    char lastName[32];
    char gradeName[16];        // e.g. "3°"
    char section[4];           // e.g. "A"
    char levelName[20];        // e.g. "PRIMARIA"
    char scheduleId[40];
};

// ─── Schedule + policy snapshot ──────────────────────────────────────
struct ScheduleRecord {
    char id[40];
    char name[40];
    char shiftName[20];
    char levelName[20];
    // Policy times (HH:MM as minutes-from-midnight, -1 if not set)
    int16_t entryTime;
    int16_t lateTime;
    int16_t entryLimitTime;
    int16_t exitTime;
    int16_t exitLimitTime;
    // Reforzamiento window (extra check-in after regular hours)
    int16_t reinforcementStart;
    int16_t reinforcementEnd;
    bool    enableExit;
    bool    enableReinforcement;
    bool    allowSaturday;
    bool    allowSunday;
};

// ─── Attendance classification result ────────────────────────────────
enum class AttendanceDecision : uint8_t {
    PRESENTE = 0,
    TARDANZA = 1,
    SALIDA   = 2,
    FUERA_DE_HORA = 3,
    ESTUDIANTE_NO_ENCONTRADO = 4,
    JORNADA_NO_ACTIVA = 5,
    ERROR = 6,
    REFORZAMIENTO = 7,
};

// ─── Confirmation payload sent to the LCD ────────────────────────────
struct LcdConfirmation {
    AttendanceDecision decision;
    char studentName[48];
    char gradeSection[8];      // e.g. "3°A"
    char shiftLevel[20];       // e.g. "Mañana Prim."
    char timeStr[8];           // "HH:MM"
};

// ─── KPI snapshot shown on OLED ──────────────────────────────────────
struct OledKpis {
    char     deviceIdShort[8];   // last 6 chars of deviceId
    uint32_t scansToday;         // total scans processed by the module today
    uint32_t scansMorning;       // scans before 13:00 (kept for DIAG screen)
    uint32_t scansAfternoon;     // scans at/after 13:00 (kept for DIAG screen)
    uint32_t pendingOffline;
    int8_t   rssi;
    uint32_t uptimeSec;
    uint32_t freeHeapKb;
    bool     wifiConnected;
    bool     ntpSynced;
    char     timeStr[6];         // "HH:MM"
    uint32_t studentsCached;     // count in student_cache (local backup)
    uint32_t schedulesCached;    // count in schedule_cache
    // Remote attendance KPIs (from /iot-roster/attendance-summary).
    // hasRemoteKpis = false → top row renders "--" placeholders (grey).
    bool     hasRemoteKpis;
    uint32_t remoteEntries;
    uint32_t remoteLates;
    uint32_t remoteUnregistered;
};

// ─── Button gestures from button task ────────────────────────────────
enum class ButtonGesture : uint8_t {
    SHORT_PRESS     = 0,        // <2s  — cicla OLED (IDLE <-> DIAG)
    LONG_PRESS      = 1,        // 2-5s — claim/pair o force sync
    VERY_LONG_PRESS = 2,        // >5s  — portal cautivo WiFi
    DOUBLE_PRESS    = 3,        // reservado
    TRIPLE_AND_HOLD = 4,        // 3 clicks + hold 4s — factory reset
};

// ─── Operating window for "should I be active right now?" ────────────
struct OperatingWindow {
    int16_t startMinutes;       // -1 = no window (always allowed)
    int16_t endMinutes;
    uint8_t daysMask;           // bit0=Sun ... bit6=Sat
};
