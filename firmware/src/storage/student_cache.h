// src/storage/student_cache.h
// Local cache of student records pulled from /api/iot-roster/students.
// Stored as JSON in LittleFS, indexed in RAM for fast binary search.
#pragma once

#include <Arduino.h>
#include "types.h"

namespace student_cache {

    // Load cache from LittleFS into RAM index. Call after storage::beginFs().
    bool load();

    // Pull fresh data from backend (uses http_client). Returns true if updated.
    bool pullFromBackend();

    // Lookup a student by document number. Returns true if found.
    bool lookup(const char* documentNumber, StudentRecord& out);

    // Stats
    uint16_t count();
    const char* etag();
    bool isLoaded();
}
