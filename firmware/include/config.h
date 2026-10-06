// include/config.h
// TRAZZO IoT — Global configuration constants
#pragma once

// ─────────────────────────────────────────────────────────────────────
// Hardware pinout (ESP32 DevKit V1 30P)
// ─────────────────────────────────────────────────────────────────────
namespace pins {
    constexpr uint8_t I2C_SDA      = 21;
    constexpr uint8_t I2C_SCL      = 22;
    constexpr uint8_t LED_GREEN    = 26;
    constexpr uint8_t LED_RED      = 27;
    constexpr uint8_t BUZZER       = 25;
    constexpr uint8_t BUTTON       = 33;
    constexpr int     SCANNER_RX   = 16;   // ESP32 RX2 ← GM65 TX
    constexpr int     SCANNER_TX   = 17;   // ESP32 TX2 → GM65 RX

    // SD card adapter (VSPI bus) — wired now, activated in Fase B.5.
    constexpr uint8_t SD_CS        = 5;
    constexpr uint8_t SD_SCK       = 18;
    constexpr uint8_t SD_MOSI      = 23;
    constexpr uint8_t SD_MISO      = 19;
}

// ─────────────────────────────────────────────────────────────────────
// I2C addresses & display geometry
// ─────────────────────────────────────────────────────────────────────
constexpr uint8_t  OLED_I2C_ADDR  = 0x3C;
constexpr uint8_t  LCD_I2C_ADDR   = 0x27;
constexpr uint8_t  OLED_WIDTH     = 128;
constexpr uint8_t  OLED_HEIGHT    = 64;
constexpr uint8_t  LCD_COLS       = 16;
constexpr uint8_t  LCD_ROWS       = 2;

// ─────────────────────────────────────────────────────────────────────
// TFT ILI9341 2.8" (SPI) — landscape mode = 320×240
// Pins are declared in TFT_eSPI build flags (see platformio.ini).
// ─────────────────────────────────────────────────────────────────────
constexpr uint16_t TFT_W = 320;
constexpr uint16_t TFT_H = 240;
constexpr uint32_t TFT_REFRESH_IDLE_MS = 2000;
constexpr uint8_t  TFT_BACKLIGHT_PIN   = 32;   // must match -DTFT_BL in platformio.ini
constexpr uint8_t  SD_CS_PIN           = 5;    // SD chip select (shares SPI bus)

// ─────────────────────────────────────────────────────────────────────
// Timing constants (milliseconds unless noted)
// ─────────────────────────────────────────────────────────────────────
constexpr uint32_t HEARTBEAT_INTERVAL_MS    = 60000;
constexpr uint32_t HEARTBEAT_INTERVAL_DEGRADED_MS = 5 * 60000;  // 5 min when failing repeatedly
constexpr uint8_t  HEARTBEAT_FAIL_THRESHOLD = 3;                // after 3 failures, enter degraded mode
constexpr uint32_t WIFI_CHECK_INTERVAL_MS   = 30000;
// Records per sync request. The queue used to go out in a single POST, which
// the device cannot build once it grows: readAll() holds the whole payload as a
// String, the batch wrapper copies it again, then HMAC and TLS need their own
// buffers. With ~83 KB of free heap a 146-record queue (~31 KB of JSON) failed
// allocation and returned -1 forever, while 60 records still went through
// (Scenario D, 2026-09-27). Chunking keeps each request small and bounded, and
// stays well under the backend's 200-record cap.
constexpr uint16_t SYNC_BATCH_MAX_RECORDS   = 25;

constexpr uint32_t SYNC_BACKOFF_INITIAL_MS  = 30000;
constexpr uint32_t SYNC_BACKOFF_MAX_MS      = 300000;
constexpr uint32_t SYNC_BACKOFF_JITTER_PCT  = 20;               // ±20% jitter
constexpr uint32_t CACHE_RETRY_AFTER_FAIL_MS = 60000;           // SWR retry after fetch fail
constexpr uint32_t DEDUP_WINDOW_MS          = 10000;   // permite re-escanear tras 10s
constexpr uint16_t BUTTON_DEBOUNCE_MS       = 50;
constexpr uint16_t BUTTON_LONG_PRESS_MS     = 2000;   // 2s  = claim/pair or force sync
constexpr uint16_t BUTTON_VERY_LONG_MS      = 5000;   // 5s  = captive portal WiFi
constexpr uint16_t BUTTON_DOUBLE_GAP_MS     = 600;
constexpr uint32_t TWDT_TIMEOUT_S           = 30;
constexpr uint32_t OLED_REFRESH_IDLE_MS     = 2000;
constexpr uint32_t LCD_CONFIRM_DURATION_MS  = 1500;   // shorter → next scan hits fresh screen

// ─────────────────────────────────────────────────────────────────────
// Deprovision detection — a device deleted on the backend gets its
// credential nulled → its authenticated requests return 401/403. After a
// sustained streak (count AND time window, to ignore transient backend
// hiccups) the module concludes it was deprovisioned and unpairs itself.
// Only 401/403 responses count; network errors / 5xx / 429 do NOT.
// ─────────────────────────────────────────────────────────────────────
constexpr uint8_t  DEPROVISION_AUTH_REJECT_THRESHOLD = 5;          // ≥5 auth rejections
constexpr uint32_t DEPROVISION_MIN_WINDOW_MS         = 10 * 60000; // AND spread over ≥10 min

// ─────────────────────────────────────────────────────────────────────
// NVS namespaces / keys
// ─────────────────────────────────────────────────────────────────────
constexpr const char* NVS_NS_CONFIG = "trazzo_cfg";
constexpr const char* NVS_NS_DEVICE = "trazzo_dev";
constexpr const char* NVS_NS_STATE  = "trazzo_st";

constexpr const char* NVS_KEY_WIFI_SSID       = "wifi_ssid";
constexpr const char* NVS_KEY_WIFI_PASS       = "wifi_pass";
constexpr const char* NVS_KEY_BACKEND_URL     = "backend_url";
constexpr const char* NVS_KEY_TENANT          = "tenant_sub";
constexpr const char* NVS_KEY_DEVICE_ID       = "device_id";
constexpr const char* NVS_KEY_API_SECRET      = "api_secret";
constexpr const char* NVS_KEY_HW_SERIAL       = "hw_serial";
constexpr const char* NVS_KEY_PROVISIONED     = "provisioned";
constexpr const char* NVS_KEY_CRASH_COUNT     = "crash_count";
constexpr const char* NVS_KEY_CRASH_TS        = "crash_ts";
constexpr const char* NVS_KEY_TENANT_AT_PAIR  = "tenant_pair";  // tenant subdomain captured at pair time
constexpr const char* NVS_KEY_BOOT_SEQ        = "boot_seq";     // monotonically increasing boot counter (evlog)
constexpr const char* NVS_KEY_LAST_EPOCH      = "last_epoch";   // last known wall-clock time, to survive power loss

// The ESP32 has no battery-backed RTC: after a power cut the clock is lost and,
// if the network is also down, NTP cannot restore it. Scans taken in that window
// used to be queued with an empty timestamp, which the backend rejects. We
// persist the clock periodically so it can be restored (approximately) at boot;
// records stamped from a restored clock are flagged `ntpUncertain`.
constexpr uint32_t CLOCK_PERSIST_INTERVAL_MS = 60000;           // how often to save the clock
constexpr uint32_t CLOCK_MIN_PLAUSIBLE_EPOCH = 1750000000;      // 2025-06-15; anything older is garbage

// ─────────────────────────────────────────────────────────────────────
// LittleFS paths
// ─────────────────────────────────────────────────────────────────────
constexpr const char* FS_PATH_STUDENTS  = "/cache/students.bin";   // MessagePack
constexpr const char* FS_PATH_STUDENTS_META = "/cache/students.meta";
constexpr const char* FS_PATH_SCHEDULES = "/cache/schedules.bin";  // MessagePack
constexpr const char* FS_PATH_SCHEDULES_META = "/cache/schedules.meta";
constexpr const char* FS_PATH_CONFIG    = "/cache/config.json";
constexpr const char* FS_PATH_OFFLINE   = "/offline/pending.ndjson";
// Records the backend rejected as permanently invalid (4xx). Parked here so a
// single bad record can never block the rest of the offline queue.
constexpr const char* FS_PATH_REJECTED  = "/offline/rejected.ndjson";
// Scan ids already accepted by the backend through the live (online) path.
// Write-ahead means EVERY scan is persisted before we try to upload it, so the
// queue needs a way to know what is already in, without rewriting the whole
// file on each scan. Cleared together with the pending queue.
constexpr const char* FS_PATH_SENT      = "/offline/sent.ndjson";

// Placeholder timestamp for a scan taken before any clock was available (first
// boot ever: no NTP and nothing saved in NVS). The record is persisted anyway
// and flagged `ntpUncertain`; the backend substitutes its own receive time.
// Losing the scan would be worse than dating it imprecisely.
constexpr const char* SCAN_EPOCH_PLACEHOLDER = "1970-01-01T00:00:00-05:00";
constexpr const char* FS_PATH_SYSLOG    = "/logs/system.log";
constexpr const char* FS_PATH_SYSLOG_OLD = "/logs/system.1.log";   // rotated (previous) log
// Persistent event log (evlog): each file is capped; when the active one
// reaches the cap it becomes ".1" and a fresh one starts. Worst case on flash
// = 2 x cap, well inside the 896 KB LittleFS partition.
constexpr uint32_t EVLOG_MAX_FILE_BYTES = 128 * 1024;

// ─────────────────────────────────────────────────────────────────────
// Firmware metadata
// ─────────────────────────────────────────────────────────────────────
constexpr const char* FIRMWARE_VERSION = "2.0.0-dev";

// ─────────────────────────────────────────────────────────────────────
// Backend defaults (overridden via captive portal / NVS)
// ─────────────────────────────────────────────────────────────────────
#ifndef TRAZZO_DEFAULT_BACKEND_URL
#define TRAZZO_DEFAULT_BACKEND_URL "https://api.example.org"
#endif
#ifndef TRAZZO_DEFAULT_TENANT
#define TRAZZO_DEFAULT_TENANT "demo"
#endif

// ─────────────────────────────────────────────────────────────────────
// NTP (Peru = UTC-5, no DST)
// ─────────────────────────────────────────────────────────────────────
constexpr const char* NTP_SERVER       = "pool.ntp.org";
constexpr long        GMT_OFFSET_SEC   = -5 * 3600;
constexpr int         DAYLIGHT_OFFSET  = 0;
