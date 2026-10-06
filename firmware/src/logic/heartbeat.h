// src/logic/heartbeat.h
// Periodic heartbeat to backend + offline sync + cache refresh.
#pragma once

#include <Arduino.h>

namespace heartbeat {
    void poll();
    void forceSyncNow();
    uint32_t lastSentAt();

    // Returns true if the current time (Lima) is within the operating window
    // fetched from /iot-roster/config. Falls back to `true` (permissive) if
    // config not yet loaded or NTP not synced — never silent by default.
    bool isInOperatingWindow();

    // Attendance KPIs pulled from /iot-roster/attendance-summary each
    // heartbeat. Consumed by the OLED/TFT dashboard for the top row.
    struct AttendanceKpis {
        bool     hasData;        // false until the first successful pull
        uint32_t entries;        // PRESENT / EXCUSED / EARLY_DEPARTURE + justifications
        uint32_t lates;          // AttendanceStatus.LATE
        uint32_t unregistered;   // totalEnrolled - attended
        uint32_t totalEnrolled;  // active students of the roster
        uint32_t fetchedAtMs;    // millis() when last updated
    };
    AttendanceKpis getAttendanceKpis();
}
