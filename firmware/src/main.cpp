// src/main.cpp
// TRAZZO IoT — B.9 entry point. setup() initializes subsystems and starts
// 5 FreeRTOS tasks. loop() is unused (idle task only).

#include <Arduino.h>
#include "utils/evlog.h"
#include "logic/offline_log.h"
#include <esp_system.h>
#include <nvs_flash.h>

#include "config.h"
#include "events.h"

#include "drivers/i2c_bus.h"
#include "drivers/leds.h"
#include "drivers/buzzer.h"
#include "drivers/button.h"
#include "drivers/display_oled.h"
#include "drivers/display_lcd.h"
#include "drivers/scanner.h"

#include "network/wifi_manager.h"
#include "network/provisioning.h"
#include "network/captive_portal.h"
#include <nvs.h>

#include "storage/littlefs_init.h"
#include "storage/student_cache.h"
#include "storage/schedule_cache.h"

#include "logic/dedup.h"

#include "tasks/tasks.h"

void setup() {
    Serial.begin(115200);
    delay(100);
    evlog::logln();
    evlog::logf("[TRAZZO] booting fw=%s heap=%lu\n", FIRMWARE_VERSION, ESP.getFreeHeap());

    // Log the reset reason — critical for diagnosing brownout loops.
    esp_reset_reason_t rr = esp_reset_reason();
    const char* rrStr = "?";
    switch (rr) {
        case ESP_RST_POWERON:  rrStr = "POWERON"; break;
        case ESP_RST_EXT:      rrStr = "EXT_PIN"; break;
        case ESP_RST_SW:       rrStr = "SW_RESET"; break;
        case ESP_RST_PANIC:    rrStr = "PANIC (crash)"; break;
        case ESP_RST_INT_WDT:  rrStr = "INT_WDT"; break;
        case ESP_RST_TASK_WDT: rrStr = "TASK_WDT"; break;
        case ESP_RST_WDT:      rrStr = "OTHER_WDT"; break;
        case ESP_RST_DEEPSLEEP:rrStr = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT: rrStr = "!!!BROWNOUT — FUENTE INSUFICIENTE!!!"; break;
        case ESP_RST_SDIO:     rrStr = "SDIO"; break;
        default: break;
    }
    evlog::logf("[TRAZZO] reset reason: %s (%d)\n", rrStr, (int)rr);

    leds::begin();
    buzzer::begin();
    button::begin();
    scanner::begin();
    dedup::reset();

    if (!i2c::begin()) evlog::logln("[TRAZZO][ERR] I2C init failed");
    i2c::scan();   // diagnostic — should list 0x27 for the LCD backpack

    if (!display_oled::begin()) {
        evlog::logln("[TRAZZO][ERR] TFT init failed");
        leds::redOn();
    } else {
        display_oled::setMode(display_oled::Mode::BOOT);
    }

    // Hardware self-test — visual confirmation that LEDs + buzzer work.
    // Green flash + red flash + short beep. Total ~360ms.
    leds::blinkGreen(120);
    delay(60);
    leds::blinkRed(120);
    buzzer::patternBoot();
    if (!display_lcd::begin()) {
        evlog::logln("[TRAZZO][ERR] LCD init failed");
    } else {
        display_lcd::setMode(display_lcd::Mode::BOOT);
    }

    events::init();
    buzzer::patternBoot();

    if (!storage::beginFs()) evlog::logln("[TRAZZO][ERR] LittleFS init failed");
    // Queue mutex must exist before any task can touch the offline log.
    offline_log::begin();

    // Persistent event log: from here on every logged line also lands in flash,
    // and a [boot] report records what survived (offline queue integrity).
    evlog::begin(rrStr, (int)rr);

    // Check if WiFi credentials exist in NVS. If not, auto-start the captive portal
    // (avoids requiring the user to know about the long-press gesture on first boot).
    bool hasWifiCreds = false;
    {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &h) == ESP_OK) {
            char ssid[33] = {0};
            size_t len = sizeof(ssid);
            if (nvs_get_str(h, NVS_KEY_WIFI_SSID, ssid, &len) == ESP_OK && ssid[0]) {
                hasWifiCreds = true;
            }
            nvs_close(h);
        }
    }

#if !defined(TRAZZO_BUILD_WOKWI)
    evlog::logf("[TRAZZO] WiFi creds in NVS? %s\n", hasWifiCreds ? "YES" : "NO");
    if (!hasWifiCreds) {
        evlog::logln("[TRAZZO] >>> AUTO-STARTING CAPTIVE PORTAL <<<");
        evlog::logln("[TRAZZO] Look at the TFT for the WiFi QR code");
        display_lcd::setErrorMessage("Primer inicio", "Setup WiFi");
        display_lcd::renderTick();
        delay(800);
        captive_portal::start();
    }
#endif

    display_lcd::setErrorMessage("Conectando...", "WiFi");
    display_lcd::renderTick();
    wifi_mgr::begin();

    uint32_t waitStart = millis();
    while (!wifi_mgr::isConnected() && (millis() - waitStart < 8000)) {
        wifi_mgr::poll();
        delay(100);
    }

    if (wifi_mgr::isConnected()) {
        auto st = wifi_mgr::getStatus();
        evlog::logf("[TRAZZO] WiFi OK — IP=%s\n", st.ip);
        display_lcd::setErrorMessage("WiFi OK", st.ip);
        display_lcd::renderTick();
        // Wait up to 15s for NTP — DeviceAuth backend rejects requests with
        // timestamps outside ±300s. If NTP fails, the heartbeat task will
        // retry pulling caches once NTP eventually syncs.
        waitStart = millis();
        while (!wifi_mgr::isNtpSynced() && (millis() - waitStart < 15000)) {
            wifi_mgr::poll();
            delay(200);
        }
        if (wifi_mgr::isNtpSynced()) {
            evlog::logln("[TRAZZO] NTP synced — caches will pull");
        } else {
            evlog::logln("[TRAZZO][WARN] NTP not synced — deferring cache pull");
        }
    } else {
        evlog::logln("[TRAZZO] WiFi not connected — offline mode");
        display_lcd::setErrorMessage("SIN WiFi", "Modo offline");
        display_lcd::renderTick();
    }

    if (provisioning::checkState() == provisioning::State::PROVISIONED) {
        if (provisioning::loadCredentials()) {
            // SECURITY: detect tenant subdomain change since pairing.
            // If it changed, this call wipes NVS and reboots — won't return.
            provisioning::checkTenantConsistency();
            tasks::isProvisioned = true;
            evlog::logln("[TRAZZO] provisioned");
        }
    } else {
        evlog::logln("[TRAZZO] NOT provisioned — double-press to pair");
    }

    student_cache::load();
    schedule_cache::load();

    // Only pull from backend if NTP is synced AND cache is empty.
    // If we already have records from LittleFS, trust them; the periodic
    // heartbeat::refreshCaches() will pull deltas via ETag every 30 min.
    // Pulling on every boot caused fragmentation → bad_alloc → PANIC crash.
    if (wifi_mgr::isConnected() && tasks::isProvisioned && wifi_mgr::isNtpSynced()) {
        if (student_cache::count() == 0) {
            evlog::logln("[TRAZZO] student cache empty — pulling from backend");
            student_cache::pullFromBackend();
        } else {
            evlog::logf("[TRAZZO] student cache has %d records — skipping initial pull\n",
                          (int)student_cache::count());
        }
        if (schedule_cache::count() == 0) {
            schedule_cache::pullFromBackend();
        }
    }

    evlog::logln();
    evlog::logln("──────────────────────────────────────────");
    evlog::logf("TRAZZO IoT — Fase B.9 (%s) students=%d schedules=%d\n",
                  tasks::isProvisioned ? "PROVISIONED" : "UNPAIRED",
                  student_cache::count(), schedule_cache::count());
    evlog::logln("──────────────────────────────────────────");

    display_oled::setMode(display_oled::Mode::IDLE_KPIS);
    display_lcd::setMode(display_lcd::Mode::IDLE);

    tasks::startAll();
}

void loop() {
    // All work happens in FreeRTOS tasks. Idle this loop.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
