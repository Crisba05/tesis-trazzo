// src/logic/dedup.cpp
#include "logic/dedup.h"
#include "config.h"

namespace dedup {

    static const uint8_t MAX_ENTRIES = 20;

    struct Entry {
        char     code[40];
        uint32_t seenAt;
    };

    static Entry  _ring[MAX_ENTRIES];
    static uint8_t _head = 0;
    static uint8_t _count = 0;

    void reset() {
        _head = 0;
        _count = 0;
        memset(_ring, 0, sizeof(_ring));
    }

    bool isDuplicate(const char* code) {
        uint32_t now = millis();
        for (uint8_t i = 0; i < _count; i++) {
            Entry& e = _ring[i];
            if (now - e.seenAt < DEDUP_WINDOW_MS && strcmp(e.code, code) == 0) {
                return true;
            }
        }
        return false;
    }

    void record(const char* code) {
        Entry& e = _ring[_head];
        strncpy(e.code, code, sizeof(e.code) - 1);
        e.code[sizeof(e.code) - 1] = '\0';
        e.seenAt = millis();
        _head = (_head + 1) % MAX_ENTRIES;
        if (_count < MAX_ENTRIES) _count++;
    }
}
