// src/storage/schedule_cache.h
// Local cache of academic schedules from /api/iot-roster/schedules.
#pragma once

#include <Arduino.h>
#include "types.h"

namespace schedule_cache {
    bool load();
    bool pullFromBackend();

    // Find the active schedule for the current time.
    // Returns true if a matching schedule was found.
    // NOTE: prefer lookupById() below when you already know the student's schedule.
    bool findActive(int16_t minutesFromMidnight, uint8_t dayOfWeek, ScheduleRecord& out);

    // Look up a schedule by its id (the student.scheduleId from the roster).
    // Returns true if found in the local cache.
    bool lookupById(const char* scheduleId, ScheduleRecord& out);

    uint8_t count();
    const char* etag();
    bool isLoaded();
}
