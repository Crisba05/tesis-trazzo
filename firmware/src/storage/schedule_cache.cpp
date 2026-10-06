// src/storage/schedule_cache.cpp
// Schedule cache with MessagePack on flash + ETag in .meta sidecar.

#include "storage/schedule_cache.h"
#include "utils/evlog.h"
#include "storage/littlefs_init.h"
#include "network/http_client.h"
#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>

namespace schedule_cache {

    static std::vector<ScheduleRecord> _records;
    static char _etag[64] = {0};
    static bool _loaded = false;

    static int16_t parseHHMM(const char* s) {
        if (!s || !s[0]) return -1;
        int h = 0, m = 0;
        if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
        return (int16_t)(h * 60 + m);
    }

    static void loadMeta() {
        if (!storage::isReady()) return;
        File f = LittleFS.open(FS_PATH_SCHEDULES_META, "r");
        if (!f) { _etag[0] = '\0'; return; }
        size_t n = f.readBytes(_etag, sizeof(_etag) - 1);
        _etag[n] = '\0';
        for (size_t i = 0; i < n; i++) if (_etag[i] == '\n' || _etag[i] == '\r') { _etag[i] = '\0'; break; }
        f.close();
    }

    static void saveMeta(const char* etag) {
        File f = LittleFS.open(FS_PATH_SCHEDULES_META, "w");
        if (!f) return;
        f.print(etag);
        f.close();
    }

    static void parseDocIntoRecords(JsonDocument& doc) {
        JsonArray arr = doc["data"].as<JsonArray>();
        if (arr.isNull()) arr = doc["schedules"].as<JsonArray>();
        _records.clear();
        _records.reserve(arr.size());

        for (JsonObject s : arr) {
            ScheduleRecord r = {};
            strncpy(r.id, s["id"] | "", sizeof(r.id) - 1);
            strncpy(r.name, s["name"] | "", sizeof(r.name) - 1);
            strncpy(r.shiftName, s["shiftName"] | "", sizeof(r.shiftName) - 1);
            strncpy(r.levelName, s["levelName"] | "", sizeof(r.levelName) - 1);

            JsonObject p = s["policy"].as<JsonObject>();
            if (!p.isNull()) {
                r.entryTime          = parseHHMM(p["entryTime"]          | (const char*)nullptr);
                r.lateTime           = parseHHMM(p["lateTime"]           | (const char*)nullptr);
                r.entryLimitTime     = parseHHMM(p["entryLimitTime"]     | (const char*)nullptr);
                r.exitTime           = parseHHMM(p["exitTime"]           | (const char*)nullptr);
                r.exitLimitTime      = parseHHMM(p["exitLimitTime"]      | (const char*)nullptr);
                r.reinforcementStart = parseHHMM(p["reinforcementStart"] | (const char*)nullptr);
                r.reinforcementEnd   = parseHHMM(p["reinforcementEnd"]   | (const char*)nullptr);
                r.enableExit         = p["enableExit"]          | false;
                r.enableReinforcement = p["enableReinforcement"]| false;
                r.allowSaturday      = p["allowSaturday"]       | false;
                r.allowSunday        = p["allowSunday"]         | false;
            } else {
                // Legacy flat format (from earlier cache)
                r.entryTime          = s["entryTime"]          | -1;
                r.lateTime           = s["lateTime"]           | -1;
                r.entryLimitTime     = s["entryLimitTime"]     | -1;
                r.exitTime           = s["exitTime"]           | -1;
                r.exitLimitTime      = s["exitLimitTime"]      | -1;
                r.reinforcementStart = -1;
                r.reinforcementEnd   = -1;
                r.enableExit         = s["enableExit"]         | false;
                r.enableReinforcement = false;
                r.allowSaturday      = s["allowSaturday"]      | false;
                r.allowSunday        = s["allowSunday"]        | false;
            }
            _records.push_back(r);
        }
    }

    bool load() {
        if (!storage::isReady()) return false;
        loadMeta();

        File f = LittleFS.open(FS_PATH_SCHEDULES, "r");
        if (!f) {
            evlog::logln("[cache:sched] no cache file");
            return false;
        }

        JsonDocument doc;
        DeserializationError err = deserializeMsgPack(doc, f);
        f.close();
        if (err) {
            evlog::logf("[cache:sched] MsgPack parse error: %s\n", err.c_str());
            return false;
        }

        parseDocIntoRecords(doc);
        _loaded = true;
        evlog::logf("[cache:sched] loaded %d schedules (MsgPack)\n", (int)_records.size());
        return true;
    }

    bool pullFromBackend() {
        auto resp = http_client::get("/api/iot-roster/schedules",
                                     _etag[0] ? _etag : nullptr);

        if (resp.statusCode == 304) {
            evlog::logln("[cache:sched] 304 — cache is current");
            return false;
        }
        if (resp.statusCode != 200) {
            evlog::logf("[cache:sched] pull failed: %d\n", resp.statusCode);
            return false;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, resp.body);
        if (err) {
            evlog::logf("[cache:sched] JSON parse error: %s\n", err.c_str());
            return false;
        }

        File f = LittleFS.open(FS_PATH_SCHEDULES, "w");
        if (!f) return false;
        size_t written = serializeMsgPack(doc, f);
        f.close();
        evlog::logf("[cache:sched] wrote %lu bytes (MsgPack)\n", (unsigned long)written);

        if (resp.etag.length() > 0) {
            strncpy(_etag, resp.etag.c_str(), sizeof(_etag) - 1);
            saveMeta(_etag);
        }

        parseDocIntoRecords(doc);
        _loaded = true;
        return true;
    }

    bool lookupById(const char* scheduleId, ScheduleRecord& out) {
        if (!_loaded || !scheduleId || !scheduleId[0]) return false;
        for (const auto& s : _records) {
            if (strncmp(s.id, scheduleId, sizeof(s.id)) == 0) {
                out = s;
                return true;
            }
        }
        return false;
    }

    bool findActive(int16_t minutesNow, uint8_t dayOfWeek, ScheduleRecord& out) {
        if (!_loaded) return false;

        for (const auto& s : _records) {
            if (dayOfWeek == 0 && !s.allowSunday) continue;
            if (dayOfWeek == 6 && !s.allowSaturday) continue;

            if (s.entryTime < 0) continue;
            int16_t windowEnd = (s.entryLimitTime > 0) ? s.entryLimitTime
                              : (s.lateTime > 0)       ? s.lateTime
                              : s.entryTime + 120;

            if (minutesNow >= s.entryTime && minutesNow <= windowEnd) {
                out = s;
                return true;
            }

            if (s.enableExit && s.exitTime > 0) {
                int16_t exitEnd = (s.exitLimitTime > 0) ? s.exitLimitTime : s.exitTime + 60;
                if (minutesNow >= s.exitTime && minutesNow <= exitEnd) {
                    out = s;
                    return true;
                }
            }
        }
        return false;
    }

    uint8_t count() { return (uint8_t)_records.size(); }
    const char* etag() { return _etag; }
    bool isLoaded() { return _loaded; }
}
