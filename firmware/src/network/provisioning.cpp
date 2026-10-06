// src/network/provisioning.cpp
// Device claim + poll provisioning flow (PLAN_V2 §3).
// 1. Generate 8-char pairing code
// 2. POST /iot-provisioning/claim with hwSerial + pairingCode
// 3. Poll GET /iot-provisioning/poll?code=XXX every 5s
// 4. On success, save deviceId + apiSecret to NVS

#include "network/provisioning.h"
#include "network/http_client.h"
#include "network/hmac_auth.h"
#include "config.h"
#include "drivers/display_oled.h"
#include "drivers/display_lcd.h"
#include "drivers/buzzer.h"
#include "tasks/tasks.h"        // livenessGroup + LIVENESS_LOGIC
#include <WiFi.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_random.h>
#include <esp_task_wdt.h>       // esp_task_wdt_reset()
#include <ArduinoJson.h>

namespace provisioning {

    static char _pairingCode[10] = {0};
    static char _backendUrl[128] = {0};
    static char _tenant[64] = {0};
    static char _deviceId[32] = {0};
    static char _apiSecret[65] = {0};
    static State _state = State::NOT_PROVISIONED;

    // Pairing code: 5 numeric digits — easier to read on the LCD and to type
    // in the admin panel. 100,000 combinations + 15-min TTL + rate limiting
    // is safe against brute-force.
    static void generatePairingCode() {
        uint8_t rnd[16];
        esp_fill_random(rnd, sizeof(rnd));
        int j = 0;
        for (int i = 0; i < (int)sizeof(rnd) && j < 5; i++) {
            uint8_t v = rnd[i] & 0x0F;   // 0..15
            if (v < 10) _pairingCode[j++] = '0' + v;   // reject-sample to remove modulo bias
        }
        while (j < 5) _pairingCode[j++] = '0' + (esp_random() % 10);
        _pairingCode[5] = '\0';
    }

    static String getHwSerial() {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        char buf[24];
        snprintf(buf, sizeof(buf), "esp32:%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return String(buf);
    }

    static bool loadNvsStr(const char* key, char* buf, size_t bufLen) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_DEVICE, NVS_READONLY, &h) != ESP_OK) {
            // Try config namespace for backend/tenant
            if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &h) != ESP_OK) return false;
        }
        size_t len = bufLen;
        esp_err_t err = nvs_get_str(h, key, buf, &len);
        nvs_close(h);
        return err == ESP_OK && len > 1;
    }

    static void saveDeviceCredentials(const char* deviceId, const char* apiSecret) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &h) != ESP_OK) {
            Serial.println("[prov] NVS open DEVICE failed");
            return;
        }
        nvs_set_str(h, NVS_KEY_DEVICE_ID, deviceId);
        nvs_set_str(h, NVS_KEY_API_SECRET, apiSecret);
        nvs_set_u8(h, NVS_KEY_PROVISIONED, 1);
        // Capture tenant subdomain at pair time so we can detect cross-tenant moves later
        if (_tenant[0]) {
            nvs_set_str(h, NVS_KEY_TENANT_AT_PAIR, _tenant);
        }
        nvs_commit(h);
        nvs_close(h);
        Serial.printf("[prov] saved deviceId=%s tenantAtPair=%s\n", deviceId, _tenant);
    }

    bool checkTenantConsistency() {
        char tenantAtPair[64] = {0};
        char tenantNow[64] = {0};

        // Load tenant captured at pair time (device namespace)
        nvs_handle_t hDev;
        if (nvs_open(NVS_NS_DEVICE, NVS_READONLY, &hDev) == ESP_OK) {
            size_t len = sizeof(tenantAtPair);
            esp_err_t err = nvs_get_str(hDev, NVS_KEY_TENANT_AT_PAIR, tenantAtPair, &len);
            nvs_close(hDev);
            if (err != ESP_OK || tenantAtPair[0] == '\0') {
                // First boot after pairing pre-watchdog firmware — record it lazily
                // (we'll capture on next successful pair). No action.
                Serial.println("[prov] tenant watchdog: no baseline yet, skipping");
                return false;
            }
        } else {
            return false;
        }

        // Load current tenant (config namespace, set by captive portal)
        nvs_handle_t hCfg;
        if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &hCfg) == ESP_OK) {
            size_t len = sizeof(tenantNow);
            esp_err_t err = nvs_get_str(hCfg, NVS_KEY_TENANT, tenantNow, &len);
            nvs_close(hCfg);
            if (err != ESP_OK) return false;
        } else {
            return false;
        }

        if (strncmp(tenantAtPair, tenantNow, sizeof(tenantAtPair)) == 0) {
            return false;  // matches, OK
        }

        // MISMATCH — wipe credentials + caches, reboot.
        Serial.printf("[prov][SECURITY] tenant changed: was=%s now=%s — forcing factory reset\n",
                      tenantAtPair, tenantNow);
        display_lcd::setErrorMessage("CAMBIO TENANT", "Reset forzado");
        display_lcd::renderTick();

        nvs_handle_t hWipe;
        if (nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &hWipe) == ESP_OK) {
            nvs_erase_all(hWipe);
            nvs_commit(hWipe);
            nvs_close(hWipe);
        }

        // Give the LCD a moment to display, then reboot.
        delay(2500);
        ESP.restart();
        return true;  // unreachable
    }

    State checkState() {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_DEVICE, NVS_READONLY, &h) != ESP_OK) {
            _state = State::NOT_PROVISIONED;
            return _state;
        }
        uint8_t prov = 0;
        nvs_get_u8(h, NVS_KEY_PROVISIONED, &prov);
        nvs_close(h);

        _state = (prov == 1) ? State::PROVISIONED : State::NOT_PROVISIONED;
        return _state;
    }

    bool loadCredentials() {
        // Load backend URL and tenant from config namespace
        nvs_handle_t hCfg;
        if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &hCfg) == ESP_OK) {
            size_t len = sizeof(_backendUrl);
            if (nvs_get_str(hCfg, NVS_KEY_BACKEND_URL, _backendUrl, &len) != ESP_OK) {
                strncpy(_backendUrl, TRAZZO_DEFAULT_BACKEND_URL, sizeof(_backendUrl) - 1);
            }
            len = sizeof(_tenant);
            if (nvs_get_str(hCfg, NVS_KEY_TENANT, _tenant, &len) != ESP_OK) {
                strncpy(_tenant, TRAZZO_DEFAULT_TENANT, sizeof(_tenant) - 1);
            }
            nvs_close(hCfg);
        }

        // Load device credentials
        nvs_handle_t hDev;
        if (nvs_open(NVS_NS_DEVICE, NVS_READONLY, &hDev) != ESP_OK) {
            return false;
        }
        size_t len = sizeof(_deviceId);
        nvs_get_str(hDev, NVS_KEY_DEVICE_ID, _deviceId, &len);
        len = sizeof(_apiSecret);
        nvs_get_str(hDev, NVS_KEY_API_SECRET, _apiSecret, &len);
        nvs_close(hDev);

        if (_deviceId[0] && _apiSecret[0]) {
            hmac_auth::setCredentials(_deviceId, _apiSecret);
            http_client::begin(_backendUrl, _tenant);
            Serial.printf("[prov] credentials loaded — device=%s backend=%s\n",
                          _deviceId, _backendUrl);
            return true;
        }
        return false;
    }

    bool runClaimAndPoll() {
        // Load backend URL + tenant
        nvs_handle_t hCfg;
        if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &hCfg) == ESP_OK) {
            size_t len = sizeof(_backendUrl);
            if (nvs_get_str(hCfg, NVS_KEY_BACKEND_URL, _backendUrl, &len) != ESP_OK)
                strncpy(_backendUrl, TRAZZO_DEFAULT_BACKEND_URL, sizeof(_backendUrl) - 1);
            len = sizeof(_tenant);
            if (nvs_get_str(hCfg, NVS_KEY_TENANT, _tenant, &len) != ESP_OK)
                strncpy(_tenant, TRAZZO_DEFAULT_TENANT, sizeof(_tenant) - 1);
            nvs_close(hCfg);
        }

        http_client::begin(_backendUrl, _tenant);

        generatePairingCode();
        _state = State::CLAIMING;

        Serial.printf("[prov] pairing code: %s\n", _pairingCode);
        Serial.printf("[prov] hwSerial: %s\n", getHwSerial().c_str());

        display_oled::setMode(display_oled::Mode::PROVISIONING);
        display_oled::setPairingCode(_pairingCode);
        display_oled::renderTick();
        display_lcd::setErrorMessage("CODIGO:", _pairingCode);
        display_lcd::renderTick();

        // Step 1: POST claim
        JsonDocument claimDoc;
        claimDoc["hardwareSerial"] = getHwSerial();
        claimDoc["pairingCode"] = _pairingCode;
        claimDoc["firmwareVersion"] = FIRMWARE_VERSION;

        String claimBody;
        serializeJson(claimDoc, claimBody);

        auto claimResp = http_client::postPublic("/api/iot-provisioning/claim", claimBody.c_str());

        if (claimResp.statusCode != 200 && claimResp.statusCode != 201) {
            Serial.printf("[prov] claim failed: %d %s\n", claimResp.statusCode, claimResp.body.c_str());
            if (claimResp.statusCode == 409) {
                display_lcd::setErrorMessage("YA PAREADO", "Factory reset");
            } else if (claimResp.statusCode < 0) {
                display_lcd::setErrorMessage("SIN CONEXION", "al backend");
            } else {
                display_lcd::setErrorMessage("ERROR CLAIM", String(claimResp.statusCode).c_str());
            }
            display_lcd::renderTick();
            // Feed WDT during 3s wait
            for (int i = 0; i < 6; i++) {
                xEventGroupSetBits(tasks::livenessGroup, tasks::LIVENESS_LOGIC);
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            _state = State::NOT_PROVISIONED;
            return false;
        }

        Serial.println("[prov] claim OK — polling for admin pairing...");
        display_lcd::setErrorMessage("ESPERANDO", "admin panel...");
        display_lcd::renderTick();

        // Step 2: Poll every 5s. Backend TTL is 15 min per code. If the admin
        // doesn't act within 5 min, we auto-regenerate the code and re-claim,
        // up to MAX_ROTATIONS times (gives admin ~15 min of fresh codes).
        // Also, if we see 5 consecutive 404 (code not found on backend, edge
        // case: backend restart), regenerate immediately.
        String pollPath = String("/api/iot-provisioning/poll?code=") + _pairingCode;
        uint32_t pollStart = millis();
        const uint32_t POLL_TIMEOUT = 5 * 60 * 1000;   // 5 min per rotation
        const uint8_t  MAX_ROTATIONS = 3;              // total ~15 min
        uint8_t  rotationsDone = 0;
        uint8_t  consecutive404 = 0;

        while (millis() - pollStart < POLL_TIMEOUT) {
            // Feed watchdog during the 5s poll interval.
            // Split into 500ms slices so LIVENESS_LOGIC + TWDT stay alive.
            for (int i = 0; i < 10; i++) {
                xEventGroupSetBits(tasks::livenessGroup, tasks::LIVENESS_LOGIC);
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            auto pollResp = http_client::getPublic(pollPath.c_str());

            if (pollResp.statusCode == 200) {
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, pollResp.body);
                if (err) {
                    Serial.printf("[prov] JSON parse error: %s\n", err.c_str());
                    continue;
                }

                const char* status = doc["status"] | "";
                const char* did    = doc["deviceId"] | "";
                const char* secret = doc["apiSecret"] | "";

                if (strcmp(status, "PAIRED") == 0 && strlen(did) > 0 && strlen(secret) > 0) {
                    // Backend now derives the tenant from the admin's JWT during
                    // /pair, so the firmware no longer asks the operator for the
                    // subdomain in the captive portal. Adopt whatever tenant the
                    // backend hands back and persist it — both in config (used
                    // by every HMAC request) and as the pairing baseline (used
                    // by the cross-tenant watchdog).
                    const char* respTenant = doc["tenantSubdomain"] | "";
                    if (respTenant[0]) {
                        strncpy(_tenant, respTenant, sizeof(_tenant) - 1);
                        _tenant[sizeof(_tenant) - 1] = 0;
                        nvs_handle_t hCfg;
                        if (nvs_open(NVS_NS_CONFIG, NVS_READWRITE, &hCfg) == ESP_OK) {
                            nvs_set_str(hCfg, NVS_KEY_TENANT, _tenant);
                            nvs_commit(hCfg);
                            nvs_close(hCfg);
                        }
                        http_client::begin(_backendUrl, _tenant);
                    }
                    saveDeviceCredentials(did, secret);
                    hmac_auth::setCredentials(did, secret);
                    strncpy(_deviceId, did, sizeof(_deviceId) - 1);
                    strncpy(_apiSecret, secret, sizeof(_apiSecret) - 1);
                    _state = State::PROVISIONED;

                    Serial.printf("[prov] PAIRED! deviceId=%s\n", did);
                    buzzer::patternOk();
                    display_lcd::setErrorMessage("PAREADO!", did);
                    display_lcd::renderTick();
                    // Feed WDT during 2s celebration
                    for (int i = 0; i < 4; i++) {
                        xEventGroupSetBits(tasks::livenessGroup, tasks::LIVENESS_LOGIC);
                        esp_task_wdt_reset();
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                    return true;
                }

                // status == "PENDING" — keep waiting
                consecutive404 = 0;
                uint32_t elapsed = (millis() - pollStart) / 1000;
                Serial.printf("[prov] poll: PENDING (%lus elapsed, rotation %u/%u)\n",
                              (unsigned long)elapsed,
                              (unsigned)(rotationsDone + 1), (unsigned)MAX_ROTATIONS);
            } else if (pollResp.statusCode == 410) {
                // 410 GONE = explicitly expired by backend. Try to rotate.
                Serial.println("[prov] pairing code expired (410) — attempting rotation");
                consecutive404 = 5;   // force rotation logic below
            } else if (pollResp.statusCode == 404) {
                consecutive404++;
                uint32_t elapsed = (millis() - pollStart) / 1000;
                Serial.printf("[prov] poll: 404 #%u (%lus elapsed)\n",
                              (unsigned)consecutive404, (unsigned long)elapsed);
            } else {
                consecutive404 = 0;
                Serial.printf("[prov] poll: HTTP %d — retrying...\n", pollResp.statusCode);
            }

            // Rotation trigger: 5 consecutive 404s or explicit 410.
            if (consecutive404 >= 5 && rotationsDone < MAX_ROTATIONS - 1) {
                rotationsDone++;
                consecutive404 = 0;
                generatePairingCode();
                Serial.printf("[prov] rotating pairing code -> %s (rotation %u/%u)\n",
                              _pairingCode, (unsigned)(rotationsDone + 1), (unsigned)MAX_ROTATIONS);
                display_oled::setPairingCode(_pairingCode);
                display_oled::renderTick();
                display_lcd::setErrorMessage("NUEVO CODIGO:", _pairingCode);
                display_lcd::renderTick();

                // Re-claim with the new code
                JsonDocument reclaimDoc;
                reclaimDoc["hardwareSerial"] = getHwSerial();
                reclaimDoc["pairingCode"] = _pairingCode;
                reclaimDoc["firmwareVersion"] = FIRMWARE_VERSION;
                String reclaimBody;
                serializeJson(reclaimDoc, reclaimBody);
                auto rc = http_client::postPublic("/api/iot-provisioning/claim", reclaimBody.c_str());
                if (rc.statusCode != 200 && rc.statusCode != 201) {
                    Serial.printf("[prov] re-claim failed: %d %s\n", rc.statusCode, rc.body.c_str());
                    // Give up rotations — the backend refused. Let outer timeout kick in.
                    consecutive404 = 0;
                } else {
                    pollPath = String("/api/iot-provisioning/poll?code=") + _pairingCode;
                    pollStart = millis();   // reset the 5-min window
                    display_lcd::setErrorMessage("ESPERANDO", "admin panel...");
                    display_lcd::renderTick();
                }
            }
        }

        Serial.println("[prov] pairing timed out");
        display_lcd::setErrorMessage("TIMEOUT", "Reintentar");
        display_lcd::renderTick();
        // Feed WDT during 2s display
        for (int i = 0; i < 4; i++) {
            xEventGroupSetBits(tasks::livenessGroup, tasks::LIVENESS_LOGIC);
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        _state = State::NOT_PROVISIONED;
        return false;
    }

    const char* getPairingCode() { return _pairingCode; }
    const char* getBackendUrl() { return _backendUrl; }
    const char* getTenantSubdomain() { return _tenant; }
}
