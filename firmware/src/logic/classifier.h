// src/logic/classifier.h
// Local attendance classifier — precalculates PRESENTE/TARDANZA for LCD display.
// Backend is authoritative; this is only for immediate visual feedback.
#pragma once

#include <Arduino.h>
#include "types.h"

namespace classifier {

    // Classify a scan time against a schedule. Returns the local decision.
    AttendanceDecision classify(int16_t minutesNow, const ScheduleRecord& schedule);
}
