// src/utils/evlog.cpp
#include "utils/evlog.h"
#include "config.h"
#include "storage/littlefs_init.h"
#include "logic/offline_log.h"

#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <stdarg.h>
#include <strings.h>
#include <time.h>

namespace evlog {

    static SemaphoreHandle_t _mu      = nullptr;
    static bool              _ready   = false;
    static uint32_t          _seq     = 0;
    static uint32_t          _size    = 0;      // approx bytes in the active file
    static volatile bool     _dumping = false;

    static constexpr size_t  MAX_LINE = 300;    // text part of a record

    uint32_t bootSeq() { return _seq; }

    // ── Which lines stay Serial-only (periodic noise) ───────────────────
    // Success heartbeats / KPI pulls / NTP retries / WiFi reconnect attempts
    // would write to flash every few seconds for hours. Failures of those are
    // already summarized by [hb] failed / [sync] / [wifi] lines that ARE kept.
    static bool noisy(const char* s) {
        static const char* const PREFIX[] = {
            "[hb] heartbeat OK", "[hb] silent mode", "[kpis]",
            "[ntp] syncing", "[ntp] sync failed", "[wifi] connecting",
            "[scanner][raw]",
        };
        for (const char* p : PREFIX) {
            if (strncmp(s, p, strlen(p)) == 0) return true;
        }
        // Normal 10-byte scanner frames duplicate "[scan] code=…". Odd sizes
        // (e.g. a frame cut mid-transmission by a power drop) ARE kept.
        if (strncmp(s, "[scanner] frame:", 16) == 0 && strstr(s, "(10 bytes)")) return true;
        if (strncmp(s, "[http] ", 7) == 0) {
            if (strstr(s, "/iot-telemetry/heartbeat") ||
                strstr(s, "/iot-roster/attendance-summary") ||
                strstr(s, "/iot-attendance/scan -> 20")) return true;   // per-scan OK is in [upl] OK
        }
        return false;
    }

    static void rotateIfNeeded() {
        if (_size < EVLOG_MAX_FILE_BYTES) return;
        LittleFS.remove(FS_PATH_SYSLOG_OLD);
        LittleFS.rename(FS_PATH_SYSLOG, FS_PATH_SYSLOG_OLD);
        _size = 0;
    }

    // Append one record to the log file. `text` is NUL-terminated; trailing
    // CR/LF are stripped. Never blocks the caller for long: if the mutex is
    // busy (e.g. another task is mid-append) we wait <=250 ms, else drop.
    static void appendLine(const char* text, size_t len) {
        if (!_ready || !_mu) return;
        while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
        if (len == 0) return;
        if (noisy(text)) return;
        if (len > MAX_LINE) len = MAX_LINE;

        if (xSemaphoreTake(_mu, pdMS_TO_TICKS(250)) != pdTRUE) return;

        static char rec[MAX_LINE + 64];     // guarded by _mu (keeps task stacks small)
        time_t now = time(nullptr);
        unsigned long epoch = (now > 1700000000) ? (unsigned long)now : 0UL;   // 0 = clock not synced yet
        int n = snprintf(rec, sizeof(rec), "B%lu M%lu E%lu | %.*s\n",
                         (unsigned long)_seq, (unsigned long)millis(), epoch, (int)len, text);
        if (n > 0) {
            if (n >= (int)sizeof(rec)) { n = sizeof(rec) - 1; rec[n - 1] = '\n'; }
            rotateIfNeeded();
            File f = LittleFS.open(FS_PATH_SYSLOG, "a");
            if (f) {
                f.write((const uint8_t*)rec, (size_t)n);
                f.close();                     // close = commit: durable per line
                _size += (uint32_t)n;
            }
        }
        xSemaphoreGive(_mu);
    }

    void logf(const char* fmt, ...) {
        char stackBuf[192];
        va_list ap, ap2;
        va_start(ap, fmt);
        va_copy(ap2, ap);
        int n = vsnprintf(stackBuf, sizeof(stackBuf), fmt, ap);
        va_end(ap);

        const char* out = stackBuf;
        char* heap = nullptr;
        if (n >= (int)sizeof(stackBuf)) {              // long line (e.g. %s of a response body)
            heap = (char*)malloc((size_t)n + 1);
            if (heap) { vsnprintf(heap, (size_t)n + 1, fmt, ap2); out = heap; }
            else      { n = (int)sizeof(stackBuf) - 1; }  // OOM: keep the truncated stack copy
        }
        va_end(ap2);

        if (n > 0) {
            Serial.write((const uint8_t*)out, (size_t)n);   // identical bytes to Serial.printf
            appendLine(out, (size_t)n);
        }
        if (heap) free(heap);
    }

    void logln(const char* s) {
        if (!s) s = "";
        Serial.println(s);
        appendLine(s, strlen(s));
    }

    // ── boot ────────────────────────────────────────────────────────────
    void begin(const char* resetReason, int resetCode) {
        if (!_mu) _mu = xSemaphoreCreateMutex();
        if (!storage::isReady()) return;

        // Persistent boot counter (survives power loss).
        uint32_t s = 0;
        nvs_handle_t h;
        if (nvs_open(NVS_NS_STATE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_get_u32(h, NVS_KEY_BOOT_SEQ, &s);
            s++;
            nvs_set_u32(h, NVS_KEY_BOOT_SEQ, s);
            nvs_commit(h);
            nvs_close(h);
        }
        _seq = s;

        if (LittleFS.exists(FS_PATH_SYSLOG)) {
            File f = LittleFS.open(FS_PATH_SYSLOG, "r");
            if (f) { _size = (uint32_t)f.size(); f.close(); }
        }
        _ready = true;

        // The first two boot lines were printed before the FS was mounted —
        // replay them into the log (file only; they already went out on Serial).
        char l[128];
        snprintf(l, sizeof(l), "[TRAZZO] booting fw=%s heap=%lu", FIRMWARE_VERSION, (unsigned long)ESP.getFreeHeap());
        appendLine(l, strlen(l));
        snprintf(l, sizeof(l), "[TRAZZO] reset reason: %s (%d)", resetReason ? resetReason : "?", resetCode);
        appendLine(l, strlen(l));

        // Structured boot report (Serial + log).
        logf("[boot] seq=%lu build=%s %s\n", (unsigned long)_seq, __DATE__, __TIME__);
        // What survived: read-only look at the offline queue. tailBytes>0 means
        // the last append was cut mid-write (power loss) and never got its '\n'.
        offline_log::VerifyResult v = offline_log::verify();
        logf("[boot] pending file: bytes=%lu lines=%lu valid=%lu tailBytes=%lu\n",
             (unsigned long)v.bytes, (unsigned long)v.lines,
             (unsigned long)v.valid, (unsigned long)v.tailBytes);
        logf("[boot] fs used=%lu total=%lu\n",
             (unsigned long)storage::usedBytes(), (unsigned long)storage::totalBytes());
    }

    // ── USB-serial commands ─────────────────────────────────────────────
    // Framing for the dump: every payload line is "LD|<record>", bracketed by
    // LOGDUMP_BEGIN / LOGDUMP_END. Anything else on the wire (normal device
    // logs printed concurrently) is ignored by the host tool.
    static void dumpTask(void*) {
        Serial.printf("LOGDUMP_BEGIN seq=%lu fw=%s\n", (unsigned long)_seq, FIRMWARE_VERSION);
        static char line[MAX_LINE + 64];
        uint32_t totalLines = 0;
        const char* const paths[2] = { FS_PATH_SYSLOG_OLD, FS_PATH_SYSLOG };
        for (const char* path : paths) {
            if (!LittleFS.exists(path)) continue;
            File f = LittleFS.open(path, "r");
            if (!f) continue;
            const size_t snapshot = f.size();     // ignore lines appended while dumping
            Serial.printf("LOGDUMP_FILE %s size=%u\n", path, (unsigned)snapshot);
            size_t pos = 0, n = 0;
            while (pos < snapshot && f.available()) {
                int c = f.read();
                if (c < 0) break;
                pos++;
                if (c == '\n') {
                    line[n] = '\0';
                    Serial.printf("LD|%s\n", line);
                    totalLines++;
                    n = 0;
                    vTaskDelay(1);                // yield; also paces the UART
                } else if (n < sizeof(line) - 1) {
                    line[n++] = (char)c;
                }
            }
            f.close();
        }
        Serial.printf("LOGDUMP_END lines=%lu\n", (unsigned long)totalLines);
        _dumping = false;
        vTaskDelete(nullptr);
    }

    static void cmdDump() {
        if (!_ready) { Serial.println("LOGDUMP_ERR fs-not-ready"); return; }
        if (_dumping) return;                    // a dump is already streaming
        _dumping = true;
        if (xTaskCreatePinnedToCore(dumpTask, "logdump", 4096, nullptr, 1, nullptr, 1) != pdPASS) {
            _dumping = false;
            Serial.println("LOGDUMP_ERR task-create");
        }
    }

    static void cmdClear() {
        if (!_ready) { Serial.println("LOGCLEAR_ERR fs-not-ready"); return; }
        if (_dumping) { Serial.println("LOGCLEAR_ERR dump-in-progress"); return; }
        if (xSemaphoreTake(_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
            LittleFS.remove(FS_PATH_SYSLOG);
            LittleFS.remove(FS_PATH_SYSLOG_OLD);
            _size = 0;
            xSemaphoreGive(_mu);
        }
        Serial.println("LOGCLEAR_OK");
        logf("[evlog] cleared by command\n");
    }

    static void cmdInfo() {
        uint32_t active = 0, old = 0;
        if (_ready) {
            if (LittleFS.exists(FS_PATH_SYSLOG))     { File f = LittleFS.open(FS_PATH_SYSLOG, "r");     if (f) { active = (uint32_t)f.size(); f.close(); } }
            if (LittleFS.exists(FS_PATH_SYSLOG_OLD)) { File f = LittleFS.open(FS_PATH_SYSLOG_OLD, "r"); if (f) { old    = (uint32_t)f.size(); f.close(); } }
        }
        Serial.printf("LOGINFO seq=%lu active=%lu old=%lu cap=%lu fs_used=%lu fs_total=%lu\n",
                      (unsigned long)_seq, (unsigned long)active, (unsigned long)old,
                      (unsigned long)EVLOG_MAX_FILE_BYTES,
                      (unsigned long)storage::usedBytes(), (unsigned long)storage::totalBytes());
    }

    static void handleCommand(const char* c) {
        if      (strcasecmp(c, "LOG DUMP")  == 0) cmdDump();
        else if (strcasecmp(c, "LOG CLEAR") == 0) cmdClear();
        else if (strcasecmp(c, "LOG INFO")  == 0) cmdInfo();
        else if (strncasecmp(c, "LOG", 3)   == 0) Serial.println("LOG commands: LOG DUMP | LOG CLEAR | LOG INFO");
    }

    void pollCommands() {
#if defined(TRAZZO_SCANNER_MOCK) && TRAZZO_SCANNER_MOCK
        return;                                   // Serial is the (mock) scanner here
#else
        static char cmd[24];
        static uint8_t n = 0;
        while (Serial.available() > 0) {
            int c = Serial.read();
            if (c < 0) break;
            if (c == '\n' || c == '\r') {
                if (n > 0) { cmd[n] = '\0'; handleCommand(cmd); n = 0; }
            } else if (n < sizeof(cmd) - 1) {
                cmd[n++] = (char)c;
            } else {
                n = 0;                            // overflow -> garbage, resync on next newline
            }
        }
#endif
    }
}
