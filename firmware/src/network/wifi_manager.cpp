// src/network/wifi_manager.cpp
// WiFi station mode + NTP sync. Credentials come from NVS (set by captive portal)
// or from build flags (Wokwi).
#include "network/wifi_manager.h"
#include "utils/evlog.h"
#include "config.h"
#include "events.h"
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <nvs_flash.h>
#include <nvs.h>

namespace wifi_mgr {

    static bool     _connected = false;
    static bool     _ntpSynced = false;
    // Sticky flag: once NTP synced at least once, RTC keeps ticking even
    // during WiFi drops. Used for offline timestamping so scans keep real
    // times instead of falling back to the epoch placeholder.
    static bool     _ntpEverSynced = false;
    // Clock restored from NVS at boot. The ESP32 has no battery-backed RTC, so
    // after a power cut _ntpEverSynced is false and, if the network is also
    // down, NTP cannot fix it. Without this, every scan in that window was
    // queued with an empty timestamp and the backend rejected the whole batch
    // (Scenario C, 2026-09-26). A restored clock is off by the time the device
    // spent powered down, so it is usable but explicitly approximate.
    static bool     _clockRestored = false;
    static uint32_t _lastClockPersist = 0;
    // While the captive portal is up, we must NOT touch WiFi mode/STA —
    // any reconnect attempt during AP mode kills the phone's connection
    // to the setup portal. task_network still calls poll() every 500ms;
    // this flag makes it a no-op.
    static bool     _paused = false;
    static uint32_t _lastCheckMs = 0;
    static uint32_t _lastNtpAttempt = 0;
    static uint32_t _reconnectBackoff = 5000;
    static char     _ssid[33] = {0};
    static char     _pass[65] = {0};

    static void loadCredentials() {
#if defined(TRAZZO_BUILD_WOKWI) && TRAZZO_BUILD_WOKWI
        strncpy(_ssid, TRAZZO_WIFI_SSID, sizeof(_ssid) - 1);
        strncpy(_pass, TRAZZO_WIFI_PASS, sizeof(_pass) - 1);
        evlog::logf("[wifi] using build-time creds: SSID=%s\n", _ssid);
#else
        // Read from NVS — written by captive portal
        nvs_handle_t h;
        if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(_ssid);
            nvs_get_str(h, NVS_KEY_WIFI_SSID, _ssid, &len);
            len = sizeof(_pass);
            nvs_get_str(h, NVS_KEY_WIFI_PASS, _pass, &len);
            nvs_close(h);
            evlog::logf("[wifi] NVS creds loaded: SSID=%s\n", _ssid);
        } else {
            evlog::logln("[wifi] NVS: no credentials stored");
        }
#endif
    }

    static void tryConnect() {
        if (_ssid[0] == '\0') {
            evlog::logln("[wifi] no SSID configured — skipping connect");
            return;
        }
        evlog::logf("[wifi] connecting to %s ...\n", _ssid);
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.begin(_ssid, _pass);
    }

    /** True when the system clock holds something usable, exact or not. */
    static bool clockUsable() { return _ntpEverSynced || _clockRestored; }

    /** Save the current wall clock so the next boot can pick it back up. */
    static void persistClock() {
        if (!clockUsable()) return;
        time_t now;
        time(&now);
        if ((uint32_t)now < CLOCK_MIN_PLAUSIBLE_EPOCH) return;
        nvs_handle_t h;
        if (nvs_open(NVS_NS_CONFIG, NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_u32(h, NVS_KEY_LAST_EPOCH, (uint32_t)now);
        nvs_commit(h);
        nvs_close(h);
    }

    /**
     * Restore the clock saved by persistClock(). Called at boot, before the
     * network is up, so scans taken during a blackout still get a timestamp.
     * The value lags by the off time plus up to CLOCK_PERSIST_INTERVAL_MS.
     */
    static void restoreClock() {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &h) != ESP_OK) return;
        uint32_t saved = 0;
        const esp_err_t err = nvs_get_u32(h, NVS_KEY_LAST_EPOCH, &saved);
        nvs_close(h);
        if (err != ESP_OK || saved < CLOCK_MIN_PLAUSIBLE_EPOCH) return;

        struct timeval tv;
        tv.tv_sec  = (time_t)saved;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        // configTime() also installs the timezone rules that strftime() needs.
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER, "time.google.com");
        _clockRestored = true;

        struct tm t;
        if (getLocalTime(&t, 0)) {
            char buf[24];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
            evlog::logf("[clock] restored from NVS: %s (approximate — pending NTP)\n", buf);
        }
    }

    static void syncNtp() {
        if (_ntpSynced) return;
        if (millis() - _lastNtpAttempt < 10000) return;
        _lastNtpAttempt = millis();

        evlog::logln("[ntp] syncing...");
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER, "time.google.com");

        struct tm t;
        if (getLocalTime(&t, 3000)) {
            _ntpSynced = true;
            _ntpEverSynced = true;
            _clockRestored = false;        // we now have the real thing
            xEventGroupSetBits(events::systemEvents, events::NTP_SYNCED);
            char buf[24];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
            evlog::logf("[ntp] synced: %s (Lima)\n", buf);
            persistClock();
        } else {
            evlog::logln("[ntp] sync failed, will retry in 10s");
        }
    }

    void begin() {
        // Before anything else: recover the clock, so a scan taken during a
        // blackout (no network, hence no NTP) still carries a timestamp.
        restoreClock();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        loadCredentials();
        tryConnect();
    }

    void pause() {
        _paused = true;
        evlog::logln("[wifi] paused (captive portal owns radio)");
    }

    void resume() {
        _paused = false;
        _lastCheckMs = 0;              // trigger immediate reconnect
        _reconnectBackoff = 5000;
        _connected = false;
        evlog::logln("[wifi] resumed");
    }

    void poll() {
        if (_paused) return;           // captive portal owns the radio
        uint32_t now = millis();
        bool wasConnected = _connected;
        _connected = (WiFi.status() == WL_CONNECTED);

        if (_connected && !wasConnected) {
            _reconnectBackoff = 5000;
            xEventGroupSetBits(events::systemEvents, events::WIFI_CONNECTED);
            xEventGroupClearBits(events::systemEvents, events::WIFI_DISCONNECTED);
            evlog::logf("[wifi] connected — IP=%s RSSI=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        }

        if (!_connected && wasConnected) {
            _ntpSynced = false;
            xEventGroupSetBits(events::systemEvents, events::WIFI_DISCONNECTED);
            xEventGroupClearBits(events::systemEvents, events::WIFI_CONNECTED);
            xEventGroupClearBits(events::systemEvents, events::NTP_SYNCED);
            evlog::logln("[wifi] disconnected");
        }

        // Reconnect with backoff
        if (!_connected && (now - _lastCheckMs > _reconnectBackoff)) {
            _lastCheckMs = now;
            if (_reconnectBackoff < 60000) _reconnectBackoff *= 2;
            tryConnect();
        }

        // NTP sync once connected
        if (_connected && !_ntpSynced) {
            syncNtp();
        }

        // Keep the saved clock fresh so the next power cut loses at most
        // CLOCK_PERSIST_INTERVAL_MS of accuracy on top of the off time.
        if (clockUsable() && (now - _lastClockPersist > CLOCK_PERSIST_INTERVAL_MS)) {
            _lastClockPersist = now;
            persistClock();
        }
    }

    Status getStatus() {
        Status s = {};
        s.connected = _connected;
        s.ntpSynced = _ntpSynced;
        s.rssi = _connected ? WiFi.RSSI() : 0;
        if (_connected) {
            strncpy(s.ip, WiFi.localIP().toString().c_str(), sizeof(s.ip) - 1);
        }
        strncpy(s.ssid, _ssid, sizeof(s.ssid) - 1);
        return s;
    }

    bool isConnected() { return _connected; }
    bool isNtpSynced() { return _ntpSynced; }

    int8_t getRssi() {
        return _connected ? (int8_t)WiFi.RSSI() : 0;
    }

    bool isClockApproximate() { return _clockRestored && !_ntpEverSynced; }

    bool getTimeStr(char* dst, size_t n) {
        // Usable if NTP synced this boot (RTC keeps ticking across WiFi drops)
        // OR if we restored the clock from NVS after a power cut. The caller
        // marks the latter with `ntpUncertain` — an approximate time is far
        // better than an empty one, which the backend rejects outright.
        if (!clockUsable()) return false;
        struct tm t;
        if (!getLocalTime(&t, 0)) return false;
        strftime(dst, n, "%Y-%m-%dT%H:%M:%S-05:00", &t);
        return true;
    }

    bool getClockStr(char* dst, size_t n) {
        if (!clockUsable()) return false;
        struct tm t;
        if (!getLocalTime(&t, 0)) return false;
        strftime(dst, n, "%H:%M", &t);
        return true;
    }

    time_t getEpoch() {
        if (!clockUsable()) return 0;
        time_t now;
        time(&now);
        return now;
    }

    int16_t getMinutesFromMidnight() {
        if (!clockUsable()) return -1;
        struct tm t;
        if (!getLocalTime(&t, 0)) return -1;
        return (int16_t)(t.tm_hour * 60 + t.tm_min);
    }
}
