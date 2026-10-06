// src/storage/littlefs_init.h
// LittleFS initialization and directory structure setup.
#pragma once

#include <Arduino.h>

namespace storage {
    bool beginFs();
    bool isReady();
    size_t totalBytes();
    size_t usedBytes();
}
