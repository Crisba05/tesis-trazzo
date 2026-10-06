// src/logic/offline_log.cpp
#include "logic/offline_log.h"
#include "utils/evlog.h"
#include "storage/littlefs_init.h"
#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <algorithm>

namespace offline_log {

    // Cached counts. -1 = "we haven't measured yet"; non-negative = authoritative.
    // _count is everything persisted; _sent is what the backend already took
    // through the live path. Pending = _count - _sent.
    static int32_t _count    = -1;
    static int32_t _sent     = 0;
    static bool    _measured = false;

    // Guards every operation on the queue files. taskLogic appends on the scan
    // path while taskUploader marks records delivered and compacts, so without
    // this a compaction could delete a scan appended microseconds earlier.
    static SemaphoreHandle_t _mtx = nullptr;

    struct Lock {
        bool held;
        explicit Lock() : held(false) {
            if (_mtx) held = (xSemaphoreTakeRecursive(_mtx, pdMS_TO_TICKS(2000)) == pdTRUE);
        }
        ~Lock() { if (held) xSemaphoreGiveRecursive(_mtx); }
    };

    void begin() {
        if (!_mtx) _mtx = xSemaphoreCreateRecursiveMutex();
    }

    // 64-bit FNV-1a. Used to hold the "already sent" set in a few KB instead of
    // a vector of 36-char ids — this runs on a device that has hit OOM during
    // roster pulls before. At 64 bits a collision across a realistic queue is
    // far below any other failure mode here.
    static uint64_t hashScanId(const char* s) {
        uint64_t h = 1469598103934665603ULL;
        for (; s && *s; ++s) { h ^= (uint8_t)*s; h *= 1099511628211ULL; }
        return h;
    }

    /** Pull the scanId out of an NDJSON line without parsing the whole record. */
    static bool scanIdOf(const String& line, String& out) {
        const int k = line.indexOf("\"scanId\":\"");
        if (k < 0) return false;
        const int start = k + 10;
        const int end = line.indexOf('"', start);
        if (end < 0) return false;
        out = line.substring(start, end);
        return true;
    }

    /** Load the acknowledged scanIds. Caller owns the vector; sync-time only. */
    static void loadSentHashes(std::vector<uint64_t>& out) {
        if (!storage::isReady() || !LittleFS.exists(FS_PATH_SENT)) return;
        File f = LittleFS.open(FS_PATH_SENT, "r");
        if (!f) return;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            out.push_back(hashScanId(line.c_str()));
        }
        f.close();
    }

    static void countLines() {
        if (!storage::isReady()) {
            _count = 0;
            return;
        }
        File f = LittleFS.open(FS_PATH_OFFLINE, "r");
        if (!f) { _count = 0; _measured = true; return; }
        int32_t n = 0;
        while (f.available()) {
            if (f.read() == '\n') n++;
        }
        f.close();
        _count = n;

        _sent = 0;
        if (LittleFS.exists(FS_PATH_SENT)) {
            File g = LittleFS.open(FS_PATH_SENT, "r");
            if (g) {
                while (g.available()) if (g.read() == '\n') _sent++;
                g.close();
            }
        }
        _measured = true;
    }

    bool append(const char* scanId, const char* code, const char* scannedAtIso,
                const char* scheduleId, AttendanceDecision decision,
                bool ntpUncertain) {
        Lock lk;
        if (!storage::isReady()) {
            evlog::logln("[offline] FS not ready — drop");
            return false;
        }

        // No clock at all (first boot ever: no NTP, nothing saved in NVS).
        // Persist the scan with a placeholder the backend recognises instead of
        // dropping it — an imprecise attendance beats a missing one. What we
        // must never write is an empty string: that is what backend validation
        // rejects, and one rejected record used to take its whole batch down.
        if (!scannedAtIso || !scannedAtIso[0]) {
            evlog::logf("[offline] no clock for %s (%s) — stored with placeholder\n",
                        scanId, code);
            scannedAtIso = SCAN_EPOCH_PLACEHOLDER;
            ntpUncertain = true;
        }
        File f = LittleFS.open(FS_PATH_OFFLINE, "a");
        if (!f) {
            evlog::logln("[offline] file open failed");
            return false;
        }

        // Derive mode string from local decision so it survives to the batch
        // upload. Backend expects "entry" | "exit" | "reinforcement".
        const char* mode = "entry";
        if (decision == AttendanceDecision::SALIDA)        mode = "exit";
        else if (decision == AttendanceDecision::REFORZAMIENTO) mode = "reinforcement";

        JsonDocument doc;
        doc["scanId"] = scanId;
        doc["documentNumber"] = code;   // matches backend IotScanDto
        doc["scannedAt"] = scannedAtIso;
        doc["mode"] = mode;
        if (scheduleId && scheduleId[0]) doc["scheduleId"] = scheduleId;
        doc["decision"] = (uint8_t)decision;
        doc["offline"] = true;
        if (ntpUncertain) doc["ntpUncertain"] = true;

        serializeJson(doc, f);
        f.print('\n');
        f.close();

        if (_count < 0) _count = 0;
        _count++;
        _measured = true;
        evlog::logf("[offline] appended scanId=%s pending=%ld\n", scanId, (long)_count);
        return true;
    }

    String readBatch(uint16_t maxRecords, std::vector<String>& ids) {
        Lock lk;
        ids.clear();
        if (!storage::isReady()) return "{\"scans\":[]}";
        File f = LittleFS.open(FS_PATH_OFFLINE, "r");
        if (!f) return "{\"scans\":[]}";

        std::vector<uint64_t> sent;
        loadSentHashes(sent);
        std::sort(sent.begin(), sent.end());

        // Built as the final request body, not as a bare array: wrapping it
        // afterwards with `"{\"scans\":" + records + "}"` allocates a second
        // full copy, and both are still alive when mbedTLS asks for its
        // handshake buffer. That is what fails as
        // `SSL - Memory allocation failed (-32512)` — the transport never even
        // opens. One buffer, reserved once.
        String result;
        result.reserve((size_t)maxRecords * 260 + 32);
        result = "{\"scans\":[";
        bool first = true;
        while (f.available() && ids.size() < maxRecords) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            if (line.indexOf("\"studentCode\":") >= 0) {
                line.replace("\"studentCode\":", "\"documentNumber\":");
            }
            String id;
            if (!scanIdOf(line, id)) continue;      // unreadable record: skip it
            if (!sent.empty() &&
                std::binary_search(sent.begin(), sent.end(), hashScanId(id.c_str()))) {
                continue;                            // already delivered live
            }
            if (!first) result += ",";
            result += line;
            first = false;
            ids.push_back(id);
        }
        result += "]}";
        f.close();
        return result;
    }

    String readAll() {
        Lock lk;
        if (!storage::isReady()) return "[]";
        File f = LittleFS.open(FS_PATH_OFFLINE, "r");
        if (!f) return "[]";

        // Write-ahead means the queue also holds records the live path already
        // delivered. Skip those instead of re-sending them.
        std::vector<uint64_t> sent;
        loadSentHashes(sent);
        std::sort(sent.begin(), sent.end());

        String result = "[";
        bool first = true;
        int migrated = 0;
        int skipped = 0;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            // On-the-fly migration for records written before the DTO rename:
            // "studentCode" -> "documentNumber". Cheap string replace.
            if (line.indexOf("\"studentCode\":") >= 0) {
                line.replace("\"studentCode\":", "\"documentNumber\":");
                migrated++;
            }
            String id;
            if (!sent.empty() && scanIdOf(line, id) &&
                std::binary_search(sent.begin(), sent.end(), hashScanId(id.c_str()))) {
                skipped++;
                continue;
            }
            if (!first) result += ",";
            result += line;
            first = false;
        }
        result += "]";
        f.close();
        if (migrated > 0) {
            evlog::logf("[offline] migrated %d record(s) studentCode->documentNumber\n", migrated);
        }
        if (skipped > 0) {
            evlog::logf("[offline] %d record(s) already delivered online — not re-sending\n", skipped);
        }
        return result;
    }

    bool markSent(const char* scanId) {
        Lock lk;
        if (!storage::isReady() || !scanId || !scanId[0]) return false;
        File f = LittleFS.open(FS_PATH_SENT, "a");
        if (!f) {
            evlog::logln("[offline] sent-marker open failed");
            return false;
        }
        f.print(scanId);
        f.print('\n');
        f.close();
        if (!_measured) countLines(); else _sent++;

        // Write-ahead means a device that is always online still appends every
        // scan here. Once everything persisted has been acknowledged, the two
        // files describe nothing and are dropped — otherwise they would grow
        // for the life of the device. Safe to do under the lock: no append can
        // slip in between the check and the removal.
        if (_count > 0 && _sent >= _count) {
            clear();
        }
        return true;
    }

    uint32_t quarantineAll() {
        Lock lk;
        if (!storage::isReady()) return 0;
        if (!LittleFS.exists(FS_PATH_OFFLINE)) return 0;

        File src = LittleFS.open(FS_PATH_OFFLINE, "r");
        if (!src) return 0;
        File dst = LittleFS.open(FS_PATH_REJECTED, "a");
        if (!dst) {
            src.close();
            evlog::logln("[offline] quarantine file open failed");
            return 0;
        }

        uint32_t moved = 0;
        while (src.available()) {
            String line = src.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            dst.print(line);
            dst.print('\n');
            moved++;
        }
        dst.close();
        src.close();

        clear();
        evlog::logf("[offline] quarantined %lu record(s) to %s\n",
                    (unsigned long)moved, FS_PATH_REJECTED);
        return moved;
    }

    void clear() {
        Lock lk;
        if (storage::isReady()) {
            LittleFS.remove(FS_PATH_OFFLINE);
            // The markers only describe records of the queue we just dropped.
            LittleFS.remove(FS_PATH_SENT);
        }
        _count = 0;
        _sent = 0;
        _measured = true;
        evlog::logln("[offline] log cleared");
    }

    uint32_t count() {
        Lock lk;
        if (!_measured) countLines();
        // Pending = persisted minus already acknowledged by the live path.
        const int32_t pending = (_count < 0 ? 0 : _count) - _sent;
        return pending > 0 ? (uint32_t)pending : 0;
    }

    VerifyResult verify() {
        Lock lk;
        VerifyResult r = {0, 0, 0, 0};
        if (!storage::isReady()) return r;
        // exists() first: opening a missing file for read makes the VFS layer
        // log a scary (but harmless) [E] line.
        if (!LittleFS.exists(FS_PATH_OFFLINE)) return r;
        File f = LittleFS.open(FS_PATH_OFFLINE, "r");
        if (!f) return r;
        r.bytes = (uint32_t)f.size();

        static char line[384];          // records are ~200 B; anything longer is invalid
        size_t n = 0;
        bool overflow = false;
        while (f.available()) {
            int c = f.read();
            if (c < 0) break;
            if (c == '\n') {
                r.lines++;
                if (!overflow && n > 0) {
                    line[n] = '\0';
                    JsonDocument doc;
                    if (deserializeJson(doc, line) == DeserializationError::Ok &&
                        doc.is<JsonObject>() && doc["scanId"].is<const char*>()) {
                        r.valid++;
                    }
                }
                n = 0; overflow = false;
            } else if (n < sizeof(line) - 1) {
                line[n++] = (char)c;
            } else {
                overflow = true;
            }
        }
        r.tailBytes = (uint32_t)n + (overflow ? 1u : 0u);   // bytes after the last '\n'
        f.close();
        return r;
    }
}
