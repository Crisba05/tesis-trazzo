// src/logic/heartbeat.cpp
// Sends heartbeat every 60s, syncs offline records, refreshes caches.
// Resilience features (this sprint):
//   - Consecutive failure counter → degraded interval (5 min) after N fails
//   - Backoff with ±20% jitter on offline sync (avoids thundering herd)
//   - SWR-style cache retry (cache_*.cpp handles its own retry timers)

#include "logic/heartbeat.h"
#include "utils/evlog.h"
#include "logic/offline_log.h"
#include <vector>
#include "network/http_client.h"
#include "network/wifi_manager.h"
#include "network/hmac_auth.h"
#include "storage/student_cache.h"
#include "storage/schedule_cache.h"
#include "tasks/tasks.h"
#include "config.h"
#include <ArduinoJson.h>
#include <esp_random.h>

namespace heartbeat {

    static uint32_t _lastHeartbeat = 0;
    static uint32_t _lowHeapStreak = 0;   // consecutive heartbeats with heap<30KB
    static uint32_t _lastCacheRefresh = 0;
    static uint32_t _lastConfigPull  = 0;
    static uint32_t _syncBackoff = SYNC_BACKOFF_INITIAL_MS;
    static uint32_t _lastSyncAttempt = 0;
    static bool     _syncPending = false;
    static uint8_t  _consecutiveFailures = 0;
    static bool     _degradedLogged = false;

    // Deprovision detection: streak of 401/403 (device-auth rejected) responses
    // and when the streak started. Reset on any 2xx. Network errors / 5xx / 429
    // do NOT touch these (only a real auth rejection means "deleted server-side").
    static uint8_t  _authRejectStreak = 0;
    static uint32_t _firstAuthRejectMs = 0;

    // Feed an HTTP status into the deprovision detector. Returns true if the
    // device should unpair itself now.
    static bool trackAuthRejection(int statusCode) {
        const bool authRejected = (statusCode == 401 || statusCode == 403);
        if (!authRejected) {
            // Any non-auth outcome (2xx, 5xx, 429, network error) clears the
            // streak — we only unpair on a *sustained* auth rejection, which
            // uniquely means the backend no longer recognizes our credential.
            if (statusCode >= 200 && statusCode < 300) {
                _authRejectStreak = 0;
                _firstAuthRejectMs = 0;
            }
            return false;
        }
        if (_authRejectStreak == 0) _firstAuthRejectMs = millis();
        if (_authRejectStreak < 255) _authRejectStreak++;
        const bool enoughCount = _authRejectStreak >= DEPROVISION_AUTH_REJECT_THRESHOLD;
        const bool enoughTime  = (millis() - _firstAuthRejectMs) >= DEPROVISION_MIN_WINDOW_MS;
        evlog::logf("[hb] auth rejection %d streak=%u window=%lums\n",
                      statusCode, (unsigned)_authRejectStreak,
                      (unsigned long)(millis() - _firstAuthRejectMs));
        return enoughCount && enoughTime;
    }

    // Operating window (fetched from /iot-roster/config).
    // -1 = not loaded / "always on" (permissive fallback).
    static int16_t  _opStartMin  = -1;
    static int16_t  _opEndMin    = -1;
    static uint8_t  _opDaysMask  = 0xFF;   // bit0=Sun, bit6=Sat; 0xFF = all days
    static bool     _opLoaded    = false;

    // Attendance KPIs (top OLED row) — refreshed each heartbeat.
    static AttendanceKpis _kpis = {};

    static int16_t parseHHMM(const char* s) {
        if (!s || !s[0]) return -1;
        int h = 0, m = 0;
        if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
        return (int16_t)(h * 60 + m);
    }

    static void pullOperatingConfig() {
        auto resp = http_client::get("/api/iot-roster/config", nullptr);
        if (resp.statusCode != 200) {
            evlog::logf("[cfg] pull failed: %d\n", resp.statusCode);
            return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
            evlog::logln("[cfg] parse error");
            return;
        }
        JsonObject win = doc["operatingWindow"].as<JsonObject>();
        if (!win.isNull()) {
            _opStartMin = parseHHMM(win["startTime"] | (const char*)nullptr);
            _opEndMin   = parseHHMM(win["endTime"]   | (const char*)nullptr);
            _opDaysMask = win["daysMask"] | 0xFF;
            _opLoaded   = true;
            evlog::logf("[cfg] operating window: %d..%d min, daysMask=0x%02X\n",
                          (int)_opStartMin, (int)_opEndMin, (unsigned)_opDaysMask);
        }
        _lastConfigPull = millis();
    }

    bool isInOperatingWindow() {
        // Permissive default: if no config loaded yet, treat as always inside.
        if (!_opLoaded || _opStartMin < 0 || _opEndMin < 0) return true;
        if (!wifi_mgr::isNtpSynced()) return true;
        struct tm t;
        if (!getLocalTime(&t, 0)) return true;
        // Day check (bit0=Sun ... bit6=Sat)
        uint8_t dayBit = 1u << t.tm_wday;
        if ((_opDaysMask & dayBit) == 0) return false;
        int16_t nowMin = (int16_t)(t.tm_hour * 60 + t.tm_min);
        if (_opStartMin <= _opEndMin) {
            return nowMin >= _opStartMin && nowMin <= _opEndMin;
        }
        // Window crosses midnight (unusual for schools but handle it):
        return nowMin >= _opStartMin || nowMin <= _opEndMin;
    }

    static uint32_t currentHeartbeatInterval() {
        return (_consecutiveFailures >= HEARTBEAT_FAIL_THRESHOLD)
            ? HEARTBEAT_INTERVAL_DEGRADED_MS
            : HEARTBEAT_INTERVAL_MS;
    }

    /** Apply ±SYNC_BACKOFF_JITTER_PCT% random jitter, clamped to [INITIAL, MAX]. */
    static uint32_t applyJitter(uint32_t base) {
        const uint32_t maxOffset = (base * SYNC_BACKOFF_JITTER_PCT) / 100;
        if (maxOffset == 0) return base;
        // esp_random() returns uint32_t; map to [-maxOffset, +maxOffset]
        const int32_t signedOffset =
            (int32_t)(esp_random() % (2 * maxOffset + 1)) - (int32_t)maxOffset;
        int64_t jittered = (int64_t)base + signedOffset;
        if (jittered < (int64_t)SYNC_BACKOFF_INITIAL_MS) jittered = SYNC_BACKOFF_INITIAL_MS;
        if (jittered > (int64_t)SYNC_BACKOFF_MAX_MS)     jittered = SYNC_BACKOFF_MAX_MS;
        return (uint32_t)jittered;
    }

    // Handle a single remote command dispatched via the heartbeat response.
    // Backend enum: factory_reset | sync_now | rotate_key | wipe_nvs.
    // rotate_key is silently ignored — the firmware doesn't have a plaintext
    // rotate path yet (Fase F2 will use ECDSA re-provisioning instead).
    static void handleRemoteCommand(const char* cmd) {
        if (!cmd || !cmd[0]) return;
        evlog::logf("[hb] remote command received: %s\n", cmd);

        if (strcmp(cmd, "factory_reset") == 0 || strcmp(cmd, "wipe_nvs") == 0) {
            tasks::factoryResetTotal();   // never returns — reboots
            return;
        }
        if (strcmp(cmd, "sync_now") == 0) {
            forceSyncNow();
            return;
        }
        evlog::logf("[hb] ignoring unknown command: %s\n", cmd);
    }

    static void pullAttendanceSummary() {
        auto resp = http_client::get("/api/iot-roster/attendance-summary", nullptr);
        if (resp.statusCode != 200) {
            evlog::logf("[kpis] pull failed: %d\n", resp.statusCode);
            return;
        }
        JsonDocument doc;
        if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
            evlog::logln("[kpis] parse error");
            return;
        }
        _kpis.entries       = doc["entries"]       | 0u;
        _kpis.lates         = doc["lates"]         | 0u;
        _kpis.unregistered  = doc["unregistered"]  | 0u;
        _kpis.totalEnrolled = doc["totalEnrolled"] | 0u;
        _kpis.fetchedAtMs   = millis();
        _kpis.hasData       = true;
        evlog::logf("[kpis] E=%lu L=%lu SR=%lu (total=%lu)\n",
                      (unsigned long)_kpis.entries, (unsigned long)_kpis.lates,
                      (unsigned long)_kpis.unregistered, (unsigned long)_kpis.totalEnrolled);
    }

    AttendanceKpis getAttendanceKpis() { return _kpis; }

    static void sendHeartbeat() {
        const uint32_t freeHeap = ESP.getFreeHeap();
        if (freeHeap < 30 * 1024) {
            _lowHeapStreak++;
            if (_lowHeapStreak >= 3) {
                evlog::logf("[hb][WARN] low-heap streak=%u free=%luB — skipping cache refresh soon\n",
                              (unsigned)_lowHeapStreak, (unsigned long)freeHeap);
            }
        } else {
            _lowHeapStreak = 0;
        }

        // Field names MUST match HeartbeatDto in saas-backend:
        // uptimeSeconds, heapFreeBytes, wifiRssi, pendingScans, firmwareVersion, ipAddress
        JsonDocument doc;
        doc["uptimeSeconds"]   = millis() / 1000;
        doc["heapFreeBytes"]   = freeHeap;
        doc["wifiRssi"]        = wifi_mgr::getRssi();
        doc["pendingScans"]    = offline_log::count();
        doc["firmwareVersion"] = FIRMWARE_VERSION;
        auto wifiStatus = wifi_mgr::getStatus();
        if (wifiStatus.ip[0]) doc["ipAddress"] = wifiStatus.ip;

        String body;
        serializeJson(doc, body);

        auto resp = http_client::post("/api/iot-telemetry/heartbeat", body.c_str());
        _lastHeartbeat = millis();

        // Deprovision check: a deleted device gets 401 (nulled key) or 403
        // (revoked/decommissioned). Sustained → unpair self (keeps WiFi).
        if (trackAuthRejection(resp.statusCode)) {
            evlog::logln("[hb][SECURITY] sustained auth rejection — device was deprovisioned. Unpairing.");
            tasks::unpairSelf();   // never returns — reboots into unpaired mode
        }

        if (resp.statusCode == 200 || resp.statusCode == 201) {
            if (_consecutiveFailures > 0) {
                evlog::logf("[hb] recovered (was fail=%u) — back to normal interval\n",
                              _consecutiveFailures);
            }
            _consecutiveFailures = 0;
            _degradedLogged = false;
            evlog::logln("[hb] heartbeat OK");

            // Parse pendingCommands from the response and dispatch them.
            // Backend returns `pendingCommands: [{type: "..."}]`. Keep parsing
            // permissive: accept either an array of strings OR objects with a
            // `type` field so the wire format can evolve without breaking.
            JsonDocument respDoc;
            if (deserializeJson(respDoc, resp.body) == DeserializationError::Ok) {
                JsonArray cmds = respDoc["pendingCommands"].as<JsonArray>();
                for (JsonVariant v : cmds) {
                    const char* c = nullptr;
                    if (v.is<const char*>()) c = v.as<const char*>();
                    else if (v.is<JsonObject>()) c = v["type"] | (const char*)nullptr;
                    if (c) handleRemoteCommand(c);
                }
            }
        } else {
            if (_consecutiveFailures < 255) _consecutiveFailures++;
            evlog::logf("[hb] heartbeat failed: %d (fail=%u) body=%s\n",
                          resp.statusCode, _consecutiveFailures, resp.body.c_str());
            if (_consecutiveFailures >= HEARTBEAT_FAIL_THRESHOLD && !_degradedLogged) {
                evlog::logf("[hb] degraded mode — interval = %lus\n",
                              (unsigned long)(HEARTBEAT_INTERVAL_DEGRADED_MS / 1000));
                _degradedLogged = true;
            }
        }
    }

    static void syncOfflineRecords() {
        uint32_t pending = offline_log::count();
        if (pending == 0) return;

        evlog::logf("[sync] %lu records pending\n", (unsigned long)pending);

        // One chunk per request: the whole queue in a single POST cannot be
        // built on this device past ~100 records and fails with -1 forever
        // (Scenario D, 2026-09-27). See SYNC_BATCH_MAX_RECORDS.
        std::vector<String> batchIds;
        // Comes back as the complete body; wrapping it here would allocate a
        // second copy and leave mbedTLS without room for its handshake.
        String body = offline_log::readBatch(SYNC_BATCH_MAX_RECORDS, batchIds);
        if (batchIds.empty()) return;

        evlog::logf("[sync] sending %u of %lu record(s), body=%u B, heap=%lu\n",
                    (unsigned)batchIds.size(), (unsigned long)pending,
                    (unsigned)body.length(), (unsigned long)ESP.getFreeHeap());

        auto resp = http_client::post("/api/iot-attendance/scan/batch", body.c_str());
        body = String();   // release before parsing the response

        if (resp.statusCode == 200 || resp.statusCode == 201) {
            // The backend adjudicates every record individually and reports how
            // many it refused; surface that instead of assuming all went in.
            JsonDocument respDoc;
            if (deserializeJson(respDoc, resp.body) == DeserializationError::Ok &&
                !respDoc["rejected"].isNull()) {
                const uint32_t accepted = respDoc["accepted"] | 0;
                const uint32_t rejected = respDoc["rejected"] | 0;
                if (rejected > 0) {
                    evlog::logf("[sync] batch OK — %lu accepted, %lu REJECTED by backend\n",
                                (unsigned long)accepted, (unsigned long)rejected);
                } else {
                    evlog::logf("[sync] batch sync OK — %lu accepted\n", (unsigned long)accepted);
                }
            } else {
                evlog::logln("[sync] batch sync OK — log cleared");
            }
            // Retire exactly what went in this chunk — never the whole queue,
            // which may still hold records this request did not carry.
            for (const String& id : batchIds) offline_log::markSent(id.c_str());
            _syncBackoff = SYNC_BACKOFF_INITIAL_MS;

            const uint32_t left = offline_log::count();
            if (left > 0) {
                // More chunks waiting: come back promptly instead of sitting
                // out the full interval. Backend allows 6 batches/min.
                evlog::logf("[sync] %lu record(s) still queued — next chunk shortly\n",
                            (unsigned long)left);
                _lastSyncAttempt = millis() - (SYNC_BACKOFF_INITIAL_MS - 12000);
                return;
            }
        } else if (resp.statusCode >= 400 && resp.statusCode < 500 && resp.statusCode != 429) {
            // Permanent rejection: the same bytes will be refused forever, so
            // retrying blocks the queue indefinitely and starves every later
            // scan — which is how Scenario C (2026-09-26) lost 6 events and
            // took two watchdog reboots. Park the batch and move on.
            // 429 is excluded on purpose: it means "later", not "never".
            evlog::logf("[sync] batch REJECTED permanently (%d) — quarantining\n",
                        resp.statusCode);
            const uint32_t moved = offline_log::quarantineAll();
            evlog::logf("[sync] %lu record(s) parked in %s; queue unblocked\n",
                        (unsigned long)moved, FS_PATH_REJECTED);
            _syncBackoff = SYNC_BACKOFF_INITIAL_MS;
        } else {
            // Exponential backoff capped at MAX, with ±20% jitter to
            // avoid synchronized retries across a fleet when internet returns.
            uint32_t doubled = _syncBackoff * 2;
            if (doubled > SYNC_BACKOFF_MAX_MS) doubled = SYNC_BACKOFF_MAX_MS;
            _syncBackoff = applyJitter(doubled);
            evlog::logf("[sync] batch failed: %d — backoff %lums (jittered)\n",
                          resp.statusCode, (unsigned long)_syncBackoff);
        }
        _lastSyncAttempt = millis();
    }

    static void refreshCaches() {
        evlog::logln("[cache] refreshing from backend...");
        student_cache::pullFromBackend();
        schedule_cache::pullFromBackend();
        pullOperatingConfig();
        _lastCacheRefresh = millis();
    }

    void poll() {
        if (!wifi_mgr::isConnected() || !hmac_auth::hasCredentials()) return;

        // DeviceAuth requires a valid timestamp — skip all backend calls until NTP synced.
        if (!wifi_mgr::isNtpSynced()) return;

        uint32_t now = millis();

        // First cache refresh: if either cache is empty AND we've never pulled
        // successfully (lastCacheRefresh == 0), try ASAP after NTP sync.
        if (_lastCacheRefresh == 0 && (student_cache::count() == 0 || schedule_cache::count() == 0)) {
            refreshCaches();
            return;  // give the cache calls a tick before heartbeat
        }

        // Heartbeat — interval auto-adjusts to degraded mode after N failures
        if (now - _lastHeartbeat >= currentHeartbeatInterval()) {
            sendHeartbeat();
            // Piggyback KPI pull on the heartbeat tick so the OLED top row
            // stays fresh without introducing an extra timer. Skip if
            // degraded (backend is flaky — no point spamming another request).
            if (_consecutiveFailures < HEARTBEAT_FAIL_THRESHOLD) {
                pullAttendanceSummary();
            }
        }

        // Sync offline records with backoff
        if (offline_log::count() > 0 && (now - _lastSyncAttempt >= _syncBackoff)) {
            syncOfflineRecords();
        }

        // Refresh caches every 30 min — pero saltamos si estamos bajos de heap
        // (student roster puede pedir 25 KB+ contiguos por página → OOM).
        if (now - _lastCacheRefresh >= 30 * 60 * 1000UL) {
            if (_lowHeapStreak >= 3) {
                evlog::logln("[hb] low-heap — skipping cache refresh");
                _lastCacheRefresh = now;   // reintentamos en la próxima ventana
            } else {
                refreshCaches();
            }
        }
    }

    void forceSyncNow() {
        _syncPending = true;
        _lastSyncAttempt = 0;
        _syncBackoff = SYNC_BACKOFF_INITIAL_MS;
        if (wifi_mgr::isConnected() && hmac_auth::hasCredentials()) {
            syncOfflineRecords();
            refreshCaches();
        }
    }

    uint32_t lastSentAt() { return _lastHeartbeat; }
}
