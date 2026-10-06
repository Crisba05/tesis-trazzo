// src/storage/littlefs_init.cpp
#include "storage/littlefs_init.h"
#include <LittleFS.h>

namespace storage {

    static bool _ready = false;

    static void ensureDir(const char* path) {
        if (!LittleFS.exists(path)) {
            if (!LittleFS.mkdir(path)) {
                Serial.printf("[fs] mkdir %s FAILED\n", path);
            }
        }
    }

    bool beginFs() {
        // Our partitions.csv names the partition "littlefs" but the Arduino
        // LittleFS lib defaults to searching for "spiffs" — pass the label
        // explicitly to avoid "partition spiffs could not be found".
        // Signature: begin(formatOnFail, basePath, maxOpenFiles, partitionLabel)
        constexpr const char* PART_LABEL = "littlefs";

        if (LittleFS.begin(false, "/littlefs", 10, PART_LABEL)) {
            _ready = true;
        } else {
            Serial.println("[fs] mount failed — formatting partition...");
            if (!LittleFS.begin(true, "/littlefs", 10, PART_LABEL)) {
                // Last-ditch fallback: try the default partition name in case
                // an older build wrote a "spiffs"-named partition.
                Serial.println("[fs] retry with default label 'spiffs'...");
                if (!LittleFS.begin(true)) {
                    Serial.println("[fs] FATAL: LittleFS mount+format both failed");
                    _ready = false;
                    return false;
                }
            }
            _ready = true;
            Serial.println("[fs] formatted successfully");
        }

        ensureDir("/cache");
        ensureDir("/offline");
        ensureDir("/logs");

        Serial.printf("[fs] LittleFS ready — %lu/%lu bytes used\n",
                      (unsigned long)LittleFS.usedBytes(),
                      (unsigned long)LittleFS.totalBytes());
        return true;
    }

    bool isReady() { return _ready; }
    size_t totalBytes() { return _ready ? LittleFS.totalBytes() : 0; }
    size_t usedBytes() { return _ready ? LittleFS.usedBytes() : 0; }
}
