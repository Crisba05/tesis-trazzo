// src/storage/student_cache.cpp
// Student roster cache. Backend sends JSON; we re-serialize to MessagePack
// for ~50% flash reduction + faster parsing on reload. ETag stored in .meta sidecar.

#include "storage/student_cache.h"
#include "utils/evlog.h"
#include "storage/littlefs_init.h"
#include "network/http_client.h"
#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>

namespace student_cache {

    struct IndexEntry {
        uint32_t hash;
        uint16_t arrayIndex;
    };

    static std::vector<StudentRecord> _records;
    static std::vector<IndexEntry>    _index;
    static char                       _etag[64] = {0};
    static bool                       _loaded = false;

    static uint32_t djb2(const char* str) {
        uint32_t h = 5381;
        while (*str) h = ((h << 5) + h) + (uint8_t)*str++;
        return h;
    }

    static void rebuildIndex() {
        _index.clear();
        _index.reserve(_records.size());
        for (uint16_t i = 0; i < _records.size(); i++) {
            _index.push_back({djb2(_records[i].documentNumber), i});
        }
        std::sort(_index.begin(), _index.end(),
                  [](const IndexEntry& a, const IndexEntry& b) { return a.hash < b.hash; });
    }

    static void loadMeta() {
        if (!storage::isReady()) return;
        File f = LittleFS.open(FS_PATH_STUDENTS_META, "r");
        if (!f) { _etag[0] = '\0'; return; }
        size_t n = f.readBytes(_etag, sizeof(_etag) - 1);
        _etag[n] = '\0';
        // Trim newline
        for (size_t i = 0; i < n; i++) if (_etag[i] == '\n' || _etag[i] == '\r') { _etag[i] = '\0'; break; }
        f.close();
    }

    static void saveMeta(const char* etag) {
        File f = LittleFS.open(FS_PATH_STUDENTS_META, "w");
        if (!f) return;
        f.print(etag);
        f.close();
    }

    static void parseDocIntoRecords(JsonDocument& doc) {
        JsonArray arr = doc["data"].as<JsonArray>();
        if (arr.isNull()) arr = doc["students"].as<JsonArray>();
        _records.clear();
        _records.reserve(arr.size());

        for (JsonObject s : arr) {
            StudentRecord r = {};
            strncpy(r.documentNumber, s["documentNumber"] | "", sizeof(r.documentNumber) - 1);
            strncpy(r.firstName, s["firstName"] | "", sizeof(r.firstName) - 1);
            strncpy(r.lastName, s["lastName"] | "", sizeof(r.lastName) - 1);
            strncpy(r.gradeName, s["gradeName"] | "", sizeof(r.gradeName) - 1);
            strncpy(r.section, s["section"] | "", sizeof(r.section) - 1);
            strncpy(r.levelName, s["levelName"] | "", sizeof(r.levelName) - 1);
            strncpy(r.scheduleId, s["scheduleId"] | "", sizeof(r.scheduleId) - 1);
            _records.push_back(r);
        }
    }

    bool load() {
        if (!storage::isReady()) return false;
        loadMeta();

        File f = LittleFS.open(FS_PATH_STUDENTS, "r");
        if (!f) {
            evlog::logln("[cache:students] no cache file");
            return false;
        }

        JsonDocument doc;
        DeserializationError err = deserializeMsgPack(doc, f);
        f.close();
        if (err) {
            evlog::logf("[cache:students] MsgPack parse error: %s\n", err.c_str());
            return false;
        }

        parseDocIntoRecords(doc);
        rebuildIndex();
        _loaded = true;
        evlog::logf("[cache:students] loaded %d records (MsgPack)\n", (int)_records.size());
        return true;
    }

    /**
     * Append one page worth of students into the global _records vector.
     * Returns the nextCursor (empty if no more pages) or "__err__" on failure.
     */
    static String pullOnePage(const char* cursor, String& outFirstPageEtag) {
        // Build URL with optional cursor query
        String path = "/api/iot-roster/students";
        if (cursor && cursor[0]) {
            path += "?cursor=";
            path += cursor;
        }

        const char* ifNoneMatch = (!cursor || !cursor[0]) && _etag[0] ? _etag : nullptr;
        auto resp = http_client::get(path.c_str(), ifNoneMatch);

        if (resp.statusCode == 304) {
            evlog::logln("[cache:students] 304 — cache is current");
            return String();   // empty → caller treats as "no more"
        }
        if (resp.statusCode != 200) {
            evlog::logf("[cache:students] pull page failed: %d\n", resp.statusCode);
            return String("__err__");
        }
        if (resp.body.length() == 0) {
            evlog::logln("[cache:students] empty body — likely OOM on device");
            return String("__err__");
        }

        // Parse one page into a JsonDocument (kept on stack scope)
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, resp.body);
        if (err) {
            evlog::logf("[cache:students] JSON parse error: %s (body=%d bytes)\n",
                          err.c_str(), (int)resp.body.length());
            return String("__err__");
        }

        // Append records from this page.
        // Note: NO per-page reserve() — pullFromBackend() already reserved
        // capacity upfront. Reserving here caused reallocation → bad_alloc.
        JsonArray arr = doc["data"].as<JsonArray>();
        if (arr.isNull()) arr = doc["students"].as<JsonArray>();
        size_t before = _records.size();
        for (JsonObject s : arr) {
            StudentRecord r = {};
            strncpy(r.documentNumber, s["documentNumber"] | "", sizeof(r.documentNumber) - 1);
            strncpy(r.firstName, s["firstName"] | "", sizeof(r.firstName) - 1);
            strncpy(r.lastName, s["lastName"] | "", sizeof(r.lastName) - 1);
            strncpy(r.gradeName, s["gradeName"] | "", sizeof(r.gradeName) - 1);
            strncpy(r.section, s["section"] | "", sizeof(r.section) - 1);
            strncpy(r.levelName, s["levelName"] | "", sizeof(r.levelName) - 1);
            strncpy(r.scheduleId, s["scheduleId"] | "", sizeof(r.scheduleId) - 1);
            _records.push_back(r);
        }
        evlog::logf("[cache:students] page +%d records (total=%d, heap=%lu)\n",
                      (int)(_records.size() - before), (int)_records.size(),
                      (unsigned long)ESP.getFreeHeap());

        // Capture ETag from the FIRST page only
        if ((!cursor || !cursor[0]) && resp.etag.length() > 0) {
            outFirstPageEtag = resp.etag;
        }

        // Return nextCursor (may be null/missing on last page)
        const char* nc = doc["nextCursor"] | "";
        return String(nc);
    }

    bool pullFromBackend() {
        _records.clear();
        _records.shrink_to_fit();
        // Reserve upfront for a typical school so we don't reallocate mid-loop.
        // Reallocation during page 2/3 was causing bad_alloc → PANIC. If cache
        // grows beyond this, vector will still grow (just once, near end).
        _records.reserve(300);
        String firstPageEtag;
        String cursor;
        int pageCount = 0;
        const int MAX_PAGES = 50;   // safety cap (~5000 students)

        while (pageCount < MAX_PAGES) {
            String next = pullOnePage(cursor.length() ? cursor.c_str() : nullptr, firstPageEtag);
            pageCount++;
            if (next == "__err__") {
                // _records was already clear()'d above and partially refilled
                // by the pages that DID succeed — but _index/_loaded still
                // describe the PREVIOUS (pre-refresh) roster. Leaving things
                // as-is here means lookup() binary-searches a stale _index
                // against a half-built _records vector: positions no longer
                // match, so valid students silently resolve as "not found"
                // (or worse, the wrong record) until the next refresh.
                // Revert to the last known-good snapshot on LittleFS instead
                // of serving a half-updated, internally inconsistent cache.
                evlog::logln("[cache:students] aborted due to page error — reverting to last known-good cache");
                if (!load()) {
                    // No usable snapshot on disk either (e.g. this was the
                    // very first pull). Drop everything so lookup() fails
                    // safely (empty cache) instead of returning garbage.
                    _records.clear();
                    _index.clear();
                    _loaded = false;
                }
                return false;
            }
            if (next.length() == 0) break;   // no more pages
            cursor = next;
        }
        evlog::logf("[cache:students] pulled %d pages, %d records total\n",
                      pageCount, (int)_records.size());

        // Persist to LittleFS as MessagePack (if FS available)
        if (storage::isReady()) {
            File f = LittleFS.open(FS_PATH_STUDENTS, "w");
            if (f) {
                JsonDocument out;
                JsonArray arr = out["data"].to<JsonArray>();
                for (const auto& r : _records) {
                    JsonObject o = arr.add<JsonObject>();
                    o["documentNumber"] = r.documentNumber;
                    o["firstName"] = r.firstName;
                    o["lastName"]  = r.lastName;
                    o["gradeName"] = r.gradeName;
                    o["section"]   = r.section;
                    o["levelName"] = r.levelName;
                    o["scheduleId"] = r.scheduleId;
                }
                size_t written = serializeMsgPack(out, f);
                f.close();
                evlog::logf("[cache:students] saved %lu bytes (MsgPack)\n", (unsigned long)written);
            } else {
                evlog::logln("[cache:students] LittleFS write failed (FS not mounted?)");
            }
        }

        // Save ETag for next conditional GET
        if (firstPageEtag.length() > 0) {
            strncpy(_etag, firstPageEtag.c_str(), sizeof(_etag) - 1);
            _etag[sizeof(_etag) - 1] = '\0';
            saveMeta(_etag);
        }

        rebuildIndex();
        _loaded = !_records.empty();
        return _loaded;
    }

    bool lookup(const char* documentNumber, StudentRecord& out) {
        if (!_loaded || _index.empty()) return false;

        uint32_t target = djb2(documentNumber);

        int lo = 0, hi = (int)_index.size() - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (_index[mid].hash == target) {
                const StudentRecord& r = _records[_index[mid].arrayIndex];
                if (strcmp(r.documentNumber, documentNumber) == 0) {
                    out = r;
                    return true;
                }
                for (int i = mid - 1; i >= lo && _index[i].hash == target; i--) {
                    const StudentRecord& r2 = _records[_index[i].arrayIndex];
                    if (strcmp(r2.documentNumber, documentNumber) == 0) { out = r2; return true; }
                }
                for (int i = mid + 1; i <= hi && _index[i].hash == target; i++) {
                    const StudentRecord& r2 = _records[_index[i].arrayIndex];
                    if (strcmp(r2.documentNumber, documentNumber) == 0) { out = r2; return true; }
                }
                return false;
            }
            if (_index[mid].hash < target) lo = mid + 1;
            else hi = mid - 1;
        }
        return false;
    }

    uint16_t count() { return (uint16_t)_records.size(); }
    const char* etag() { return _etag; }
    bool isLoaded() { return _loaded; }
}
