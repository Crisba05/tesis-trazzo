// src/network/wifi_manager.h
// WiFi connection manager with auto-reconnect and NTP sync.
#pragma once

#include <Arduino.h>

namespace wifi_mgr {

    struct Status {
        bool     connected;
        bool     ntpSynced;
        int8_t   rssi;
        char     ip[16];
        char     ssid[33];
    };

    void begin();
    void poll();

    // Pause STA reconnect logic — used by the captive portal so its AP
    // doesn't get stomped by an auto-reconnect attempt.
    void pause();
    void resume();

    Status getStatus();
    bool   isConnected();
    bool   isNtpSynced();
    int8_t getRssi();

    // True when the clock is usable but was restored from NVS rather than set
    // by NTP — i.e. it drifts by however long the device was powered off.
    // Timestamps produced while this is true must be marked `ntpUncertain`.
    bool isClockApproximate();

    // Get current time as ISO 8601 string with Lima timezone.
    // Returns false only if there is no clock at all (never synced and nothing
    // restorable from NVS).
    bool getTimeStr(char* dst, size_t n);

    // Get HH:MM string. Returns false if there is no clock.
    bool getClockStr(char* dst, size_t n);

    // Get current time as unix epoch seconds. Returns 0 if there is no clock.
    time_t getEpoch();

    // Minutes from midnight (for schedule matching). Returns -1 if no clock.
    int16_t getMinutesFromMidnight();
}
