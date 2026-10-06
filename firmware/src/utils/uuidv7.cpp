// src/utils/uuidv7.cpp
// UUIDv7 generation using NTP-synced wall clock + ESP32 hardware RNG.

#include "utils/uuidv7.h"
#include <esp_random.h>
#include <sys/time.h>

namespace uuidv7 {

    void generate(char* out) {
        // 1. Get wall-clock milliseconds since unix epoch
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        uint64_t ms = (uint64_t)tv.tv_sec * 1000ULL + (tv.tv_usec / 1000ULL);

        // If NTP not synced (epoch < 2020), fall back to millis() + arbitrary offset.
        // Records still get unique IDs; backend can detect ntp_uncertain via timestamp.
        if (ms < 1577836800000ULL) {
            ms = 1577836800000ULL + millis();
        }

        // 2. Build 16-byte UUID
        uint8_t b[16];

        // Bytes 0-5: 48-bit timestamp (big-endian)
        b[0] = (ms >> 40) & 0xff;
        b[1] = (ms >> 32) & 0xff;
        b[2] = (ms >> 24) & 0xff;
        b[3] = (ms >> 16) & 0xff;
        b[4] = (ms >> 8)  & 0xff;
        b[5] =  ms        & 0xff;

        // Bytes 6-15: random
        uint32_t r1 = esp_random();
        uint32_t r2 = esp_random();
        uint32_t r3 = esp_random();
        b[6]  = (r1 >> 24) & 0xff;
        b[7]  = (r1 >> 16) & 0xff;
        b[8]  = (r1 >> 8)  & 0xff;
        b[9]  =  r1        & 0xff;
        b[10] = (r2 >> 24) & 0xff;
        b[11] = (r2 >> 16) & 0xff;
        b[12] = (r2 >> 8)  & 0xff;
        b[13] =  r2        & 0xff;
        b[14] = (r3 >> 8)  & 0xff;
        b[15] =  r3        & 0xff;

        // Set version (7) in byte 6 top nibble
        b[6] = (b[6] & 0x0f) | 0x70;
        // Set variant (10) in byte 8 top 2 bits
        b[8] = (b[8] & 0x3f) | 0x80;

        // 3. Format as "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
        static const char* hex = "0123456789abcdef";
        int p = 0;
        for (int i = 0; i < 16; i++) {
            out[p++] = hex[(b[i] >> 4) & 0xf];
            out[p++] = hex[b[i] & 0xf];
            if (i == 3 || i == 5 || i == 7 || i == 9) out[p++] = '-';
        }
        out[p] = '\0';
    }
}
