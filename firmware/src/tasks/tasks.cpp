// src/tasks/tasks.cpp
// FreeRTOS task implementations.

#include "tasks/tasks.h"
#include "utils/evlog.h"
#include "config.h"
#include "events.h"

#include "drivers/scanner.h"
#include "drivers/button.h"
#include "drivers/display_oled.h"
#include "drivers/display_tft.h"
#include "drivers/display_lcd.h"
#include "drivers/leds.h"
#include "drivers/buzzer.h"

#include "network/wifi_manager.h"
#include "network/captive_portal.h"
#include "network/http_client.h"
#include "network/hmac_auth.h"
#include "network/provisioning.h"

#include "storage/student_cache.h"
#include "storage/schedule_cache.h"

#include "logic/dedup.h"
#include "logic/classifier.h"
#include "logic/offline_log.h"
#include "logic/heartbeat.h"

#include "utils/uuidv7.h"
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <LittleFS.h>

namespace tasks {

    QueueHandle_t      qScanCodes      = nullptr;
    QueueHandle_t      qButtonGestures = nullptr;
    QueueHandle_t      qScanUploads    = nullptr;
    EventGroupHandle_t livenessGroup   = nullptr;

    volatile uint32_t scansToday     = 0;
    volatile uint32_t scansMorning   = 0;
    volatile uint32_t scansAfternoon = 0;
    volatile bool     isProvisioned  = false;

    // ─────────────────────────────────────────────────────────────────
    // Full factory reset: wipe all NVS namespaces used by the module and
    // reformat LittleFS (drops cached roster + offline scan queue). Blocks
    // ~2.5s to show a confirmation to the operator and then reboots.
    // Callable from the button gesture handler AND from the remote
    // `factory_reset` command dispatched by heartbeat::handleCommand.
    // ─────────────────────────────────────────────────────────────────
    void factoryResetTotal() {
        evlog::logln("[factory] FULL WIPE start");
        display_oled::setMode(display_oled::Mode::ERROR);
        display_oled::setErrorMessage("FACTORY", "Reset TOTAL");
        display_lcd::setErrorMessage("FACTORY RESET", "Borrando todo...");
        buzzer::patternError();

        auto wipeNs = [](const char* ns) {
            nvs_handle_t h;
            if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
                evlog::logf("[factory] wiped NVS ns=%s\n", ns);
            }
        };
        wipeNs(NVS_NS_DEVICE);
        wipeNs(NVS_NS_CONFIG);
        wipeNs(NVS_NS_STATE);

        // LittleFS holds student cache, schedule cache, offline scans, syslog.
        // Format is the only reliable way to drop everything without walking
        // the filesystem file by file.
        if (LittleFS.begin(false)) {
            if (LittleFS.format()) {
                evlog::logln("[factory] LittleFS formatted");
            } else {
                evlog::logln("[factory][WARN] LittleFS format failed");
            }
        } else {
            // Not mounted yet — try formatting directly.
            LittleFS.format();
            evlog::logln("[factory] LittleFS format attempted (was unmounted)");
        }

        vTaskDelay(pdMS_TO_TICKS(2500));
        evlog::logln("[factory] rebooting");
        ESP.restart();
    }

    // ─────────────────────────────────────────────────────────────────
    // Unpair-self: used when the backend deprovisions the device (deleted).
    // Wipes device credentials + LittleFS caches but KEEPS WiFi config, so the
    // module reboots into unpaired mode ready to re-pair (BLE) without asking
    // for the network again. Distinct from factoryResetTotal (which wipes WiFi
    // too — reserved for factory reset / reported-stolen).
    // ─────────────────────────────────────────────────────────────────
    void unpairSelf() {
        evlog::logln("[unpair] deprovisioned — wiping credentials, keeping WiFi");
        display_oled::setMode(display_oled::Mode::ERROR);
        display_oled::setErrorMessage("SIN PAREAR", "Eliminado");
        display_lcd::setErrorMessage("MODULO ELIMINADO", "Volviendo a parear");
        buzzer::patternError();

        // Only the device namespace (device_id, api_secret, provisioned,
        // tenant_pair). NVS_NS_CONFIG (wifi_ssid/pass/backend_url) is kept.
        nvs_handle_t h;
        if (nvs_open(NVS_NS_DEVICE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
            evlog::logln("[unpair] wiped NVS ns=trazzo_dev");
        }

        // Drop cached roster/schedules + offline queue — they belong to the old
        // tenant and can never be uploaded now. Format is the simplest clean drop.
        if (LittleFS.begin(false)) {
            LittleFS.format();
            evlog::logln("[unpair] LittleFS formatted (dropped stale caches)");
        } else {
            LittleFS.format();
        }

        vTaskDelay(pdMS_TO_TICKS(2500));
        evlog::logln("[unpair] rebooting into unpaired mode");
        ESP.restart();
    }

    // ─────────────────────────────────────────────────────────────────
    // Scan processing — runs on logic task, may block on network
    // ─────────────────────────────────────────────────────────────────
    // Derive the mode string ("entry"/"exit"/"reinforcement") the backend
    // expects from a local classifier decision. Kept as a helper so the same
    // logic is used both for online scans and offline records.
    static const char* modeFromDecision(AttendanceDecision d) {
        switch (d) {
            case AttendanceDecision::SALIDA:        return "exit";
            case AttendanceDecision::REFORZAMIENTO: return "reinforcement";
            default:                                return "entry";   // PRESENTE, TARDANZA, FUERA_DE_HORA
        }
    }

    // Reject anything that isn't a plausible document number (6-12 numeric).
    // Keeps URLs and misconfigured GM65 output out of the backend & offline log.
    static bool isValidDocumentNumber(const char* code) {
        if (!code) return false;
        size_t len = strlen(code);
        if (len < 6 || len > 12) return false;
        for (size_t i = 0; i < len; i++) {
            if (code[i] < '0' || code[i] > '9') return false;
        }
        return true;
    }

    static void processScan(const char* code) {
        evlog::logf("[scan] code=%s\n", code);

        // Filter garbage before touching any downstream state (dedup, LCD, HTTP).
        if (!isValidDocumentNumber(code)) {
            evlog::logf("[scan] INVALID doc number '%s' — dropped locally\n", code);
            display_lcd::setErrorMessage("QR INVALIDO", "DNI no valido");
            display_tft::showScanToast("QR INVALIDO", "no numerico", "", 8);
            leds::blinkRed(180);
            buzzer::patternError();
            return;
        }

        if (dedup::isDuplicate(code)) {
            evlog::logln("[scan] duplicate — ignored");
            buzzer::patternWarn();
            return;
        }
        dedup::record(code);

        // 1. Look up student in local cache. The student carries their own
        //    scheduleId → we don't need to search "which schedule is active".
        StudentRecord student = {};
        bool found = student_cache::lookup(code, student);
        // Visible on serial now — the LCD-only "NO REGISTRADO" was undiagnosable
        // from a capture alone. Cache size printed too: a low count vs. the
        // tenant's real roster is the tell for a stale/partial cache.
        evlog::logf("[scan] lookup code=%s found=%d cacheSize=%u\n",
                      code, (int)found, (unsigned)student_cache::count());

        // 2. If found, load the STUDENT's schedule (not "the active one for
        //    this device"). This makes the module correct for multi-schedule
        //    schools: a morning-shift student scanned at 08:00 uses their own
        //    entry window, and an afternoon-shift student at 08:00 falls
        //    outside their own window (FUERA_DE_HORA) — both cases correct.
        ScheduleRecord schedule = {};
        bool hasSchedule = false;
        AttendanceDecision decision = AttendanceDecision::ESTUDIANTE_NO_ENCONTRADO;

        if (found) {
            hasSchedule = schedule_cache::lookupById(student.scheduleId, schedule);
            if (!hasSchedule) {
                evlog::logf("[scan] student found but schedule %s not in cache\n",
                              student.scheduleId);
                decision = AttendanceDecision::JORNADA_NO_ACTIVA;
            } else if (wifi_mgr::isNtpSynced()) {
                struct tm t;
                if (getLocalTime(&t, 0)) {
                    int16_t mins = t.tm_hour * 60 + t.tm_min;
                    decision = classifier::classify(mins, schedule);
                } else {
                    decision = AttendanceDecision::PRESENTE;  // best-effort
                }
            } else {
                // No NTP yet: assume PRESENTE locally; backend re-evaluates.
                decision = AttendanceDecision::PRESENTE;
            }
        }

        // 3. Build LCD confirmation
        LcdConfirmation c = {};
        c.decision = decision;
        if (found) {
            snprintf(c.studentName, sizeof(c.studentName), "%s %s",
                     student.firstName, student.lastName);
            snprintf(c.gradeSection, sizeof(c.gradeSection), "%s%s",
                     student.gradeName, student.section);
            snprintf(c.shiftLevel, sizeof(c.shiftLevel), "%s",
                     hasSchedule ? schedule.shiftName : "");
        } else {
            snprintf(c.studentName, sizeof(c.studentName), "NO REGISTRADO");
            snprintf(c.gradeSection, sizeof(c.gradeSection), "---");
            snprintf(c.shiftLevel, sizeof(c.shiftLevel), "Consulte oficina");
        }
        wifi_mgr::getClockStr(c.timeStr, sizeof(c.timeStr));
        display_lcd::setConfirmation(c);

        // 4. TFT toast — SOLO para errores/edge cases. Los éxitos van al LCD.
        //    Códigos: 0=PRESENTE 1=TARDANZA 2=SALIDA 3=FUERA HORA
        //             4=NO REGISTRADO 6=SIN JORNADA 9=REFORZAMIENTO
        //    Éxitos (PRESENTE/TARDANZA/SALIDA/REFUERZO) no interrumpen el
        //    dashboard TFT — el operador lee el nombre en el LCD 16x2. El
        //    "último scan" del footer TFT sigue actualizándose.
        uint8_t toastCode = 0;
        if (!found) toastCode = 4;
        else if (decision == AttendanceDecision::TARDANZA)              toastCode = 1;
        else if (decision == AttendanceDecision::SALIDA)                toastCode = 2;
        else if (decision == AttendanceDecision::FUERA_DE_HORA)         toastCode = 3;
        else if (decision == AttendanceDecision::JORNADA_NO_ACTIVA)     toastCode = 6;
        else if (decision == AttendanceDecision::REFORZAMIENTO)         toastCode = 9;
        const bool isTftErrorToast =
            (toastCode == 3 || toastCode == 4 || toastCode == 6 || toastCode == 7);
        if (isTftErrorToast) {
            display_tft::showScanToast(c.studentName, c.gradeSection, c.timeStr, toastCode);
        }

        // 5. Sound + LED feedback (differentiated per decision).
        //    Silent mode: outside the tenant's operating hours (fetched via
        //    /iot-roster/config), mute buzzer + LED but STILL register the
        //    scan. Useful for early/late arrivals without disturbing the room.
        // LED durations bumped so a busy scan queue still leaves the light on
        // long enough for the operator to read the decision. Blinks are
        // non-blocking (see leds::tick()).
        constexpr uint16_t LED_MS_ERROR = 2000;
        constexpr uint16_t LED_MS_OK    = 1500;
        const bool silent = !heartbeat::isInOperatingWindow();
        if (silent) {
            evlog::logln("[hb] silent mode — no sound/led feedback");
        } else if (!found) {
            leds::blinkRed(LED_MS_ERROR);
            buzzer::patternNoRegister();
        } else if (decision == AttendanceDecision::FUERA_DE_HORA ||
                   decision == AttendanceDecision::JORNADA_NO_ACTIVA) {
            leds::blinkRed(LED_MS_ERROR);
            buzzer::patternOutOfHour();
        } else if (decision == AttendanceDecision::TARDANZA) {
            leds::blinkRed(LED_MS_OK);
            buzzer::patternLate();
        } else if (decision == AttendanceDecision::REFORZAMIENTO) {
            leds::blinkGreen(LED_MS_OK);
            buzzer::patternReinforcement();
        } else if (decision == AttendanceDecision::SALIDA) {
            leds::blinkGreen(LED_MS_OK);
            buzzer::patternExit();
        } else {
            leds::blinkGreen(LED_MS_OK);
            buzzer::patternPresent();
        }

        scansToday++;
        {
            // Split by shift: morning (< 13:00) vs afternoon (>= 13:00).
            // Fallback: if NTP not synced, count as morning.
            struct tm t;
            bool isAfternoon = false;
            if (wifi_mgr::isNtpSynced() && getLocalTime(&t, 0)) {
                isAfternoon = (t.tm_hour >= 13);
            }
            if (isAfternoon) scansAfternoon++;
            else             scansMorning++;
        }

        char scanId[40];
        uuidv7::generate(scanId);

        char isoTime[32] = {0};
        bool ntpOk = wifi_mgr::getTimeStr(isoTime, sizeof(isoTime));

        // Auto-detected mode (entry/exit/reinforcement) from local classifier.
        const char* mode = modeFromDecision(decision);

        // Enqueue the upload job — taskUploader (Core 0) handles HTTP + offline
        // fallback. This keeps taskLogic responsive: the next scan can be
        // classified + displayed in <50 ms even while the network is slow.
        ScanUploadJob job = {};
        strncpy(job.scanId,         scanId, sizeof(job.scanId) - 1);
        strncpy(job.documentNumber, code,   sizeof(job.documentNumber) - 1);
        strncpy(job.scannedAtIso,   isoTime, sizeof(job.scannedAtIso) - 1);
        if (hasSchedule) strncpy(job.scheduleId, schedule.id, sizeof(job.scheduleId) - 1);
        strncpy(job.studentNameShort,
                found ? student.firstName : "NO REG",
                sizeof(job.studentNameShort) - 1);
        strncpy(job.gradeSection, c.gradeSection, sizeof(job.gradeSection) - 1);
        strncpy(job.timeStr,      c.timeStr,      sizeof(job.timeStr) - 1);
        job.decision       = (uint8_t)decision;
        job.localToastCode = toastCode;
        // Uncertain when there is no clock at all, or when the clock was
        // restored from NVS after a power cut instead of set by NTP.
        job.ntpUncertain   = !ntpOk || wifi_mgr::isClockApproximate();

        // WRITE-AHEAD: the scan goes to flash BEFORE we try to upload it.
        //
        // It used to be the other way round — upload first, persist only if the
        // upload failed — which left the event in RAM alone for as long as the
        // HTTP attempt took to give up: 4.1 to 19.4 s measured with no
        // connectivity. A power cut in that window erased it. Scenario C on
        // 2026-09-27 lost 6 of 14 scans exactly there, while not one of the 22
        // cuts in the campaign ever damaged a flash write. Persisting first
        // moves the exposure to the append itself.
        //
        // taskUploader marks the record as delivered once the backend accepts
        // it, so the batch sync does not send it twice.
        if (!offline_log::append(scanId, code, isoTime,
                                 hasSchedule ? schedule.id : "", decision,
                                 job.ntpUncertain)) {
            evlog::logf("[scan] WARNING: could not persist %s (%s) — upload is its only chance\n",
                        scanId, code);
        }

        // Backpressure: si la cola está llena, el registro ya está en flash,
        // así que lo recogerá el sync por lote. Nunca bloqueamos taskLogic.
        if (!qScanUploads || xQueueSend(qScanUploads, &job, 0) != pdTRUE) {
            evlog::logln("[scan] upload queue FULL — persisted; batch sync will pick it up");
        }

        // Update the persistent "last scan" indicator in the dashboard footer
        // right away. taskUploader may refine it later if the backend disagrees.
        display_tft::setLastScan(found ? student.firstName : "NO REG",
                                 c.timeStr, toastCode);
    }

    // ─────────────────────────────────────────────────────────────────
    // Button gesture handler — runs on logic task
    // ─────────────────────────────────────────────────────────────────
    // ─────────────────────────────────────────────────────────────────
    // Button gestures:
    //   SHORT  (<2s)  — cicla OLED: IDLE <-> DIAG
    //   LONG   (2-5s) — si no provisionado: claim/pair. Si ya: force sync
    //   V.LONG (>5s)  — portal cautivo WiFi
    //   3x+hold 4s    — factory reset
    // ─────────────────────────────────────────────────────────────────
    static void handleButton(ButtonGesture g) {
        static display_oled::Mode oledMode = display_oled::Mode::IDLE_KPIS;
        static display_tft::Mode  tftMode  = display_tft::Mode::IDLE_KPIS;
        switch (g) {
            case ButtonGesture::SHORT_PRESS:
                // Cycle both drivers so it works on the Wokwi OLED build AND
                // the TFT hardware build. Whichever is compiled in reacts;
                // the other one is a stub.
                oledMode = (oledMode == display_oled::Mode::IDLE_KPIS)
                               ? display_oled::Mode::DIAG
                               : display_oled::Mode::IDLE_KPIS;
                display_oled::setMode(oledMode);
                tftMode = (tftMode == display_tft::Mode::IDLE_KPIS)
                              ? display_tft::Mode::DIAG
                              : display_tft::Mode::IDLE_KPIS;
                display_tft::setMode(tftMode);
                buzzer::patternBoot();
                evlog::logf("[btn] SHORT — cycled to %s\n",
                              tftMode == display_tft::Mode::DIAG ? "DIAG" : "IDLE");
                break;

            case ButtonGesture::LONG_PRESS:
                // 2-5s hold: claim/pair if not provisioned, force sync if provisioned
                if (!isProvisioned && wifi_mgr::isConnected()) {
                    evlog::logln("[btn] LONG — claim/pair");
                    buzzer::patternBoot();
                    display_lcd::setErrorMessage("Pareando...", "Espere");
                    if (provisioning::runClaimAndPoll()) {
                        isProvisioned = true;
                        buzzer::patternOk();
                    } else {
                        buzzer::patternError();
                    }
                    display_oled::setMode(display_oled::Mode::IDLE_KPIS);
                    display_tft::setMode(display_tft::Mode::IDLE_KPIS);
                    display_lcd::setMode(display_lcd::Mode::IDLE);
                    oledMode = display_oled::Mode::IDLE_KPIS;
                    tftMode  = display_tft::Mode::IDLE_KPIS;
                } else if (isProvisioned) {
                    evlog::logln("[btn] LONG — force sync");
                    buzzer::patternBoot();
                    heartbeat::forceSyncNow();
                } else {
                    evlog::logln("[btn] LONG — no WiFi, cannot pair");
                    display_lcd::setErrorMessage("Sin WiFi", "Conecte primero");
                    buzzer::patternError();
                }
                break;

            case ButtonGesture::VERY_LONG_PRESS:
                // >5s hold: open captive portal for WiFi config
                evlog::logln("[btn] VERY_LONG — captive portal");
                display_lcd::setErrorMessage("CONFIG WiFi", "Portal abierto");
                captive_portal::start();   // blocks until user submits or timeout
                display_oled::setMode(display_oled::Mode::IDLE_KPIS);
                display_tft::setMode(display_tft::Mode::IDLE_KPIS);
                display_lcd::setMode(display_lcd::Mode::IDLE);
                oledMode = display_oled::Mode::IDLE_KPIS;
                tftMode  = display_tft::Mode::IDLE_KPIS;
                wifi_mgr::begin();
                break;

            case ButtonGesture::DOUBLE_PRESS:
                // Legacy/reservado — tratar como short press
                oledMode = (oledMode == display_oled::Mode::IDLE_KPIS)
                               ? display_oled::Mode::DIAG
                               : display_oled::Mode::IDLE_KPIS;
                display_oled::setMode(oledMode);
                tftMode = (tftMode == display_tft::Mode::IDLE_KPIS)
                              ? display_tft::Mode::DIAG
                              : display_tft::Mode::IDLE_KPIS;
                display_tft::setMode(tftMode);
                break;

            case ButtonGesture::TRIPLE_AND_HOLD: {
                evlog::logln("[btn] TRIPLE+HOLD — factory reset (FULL WIPE)");
                factoryResetTotal();
                break;
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // task_network — Core 0, prio 5
    // ─────────────────────────────────────────────────────────────────
    static void taskNetwork(void*) {
        esp_task_wdt_add(NULL);
        for (;;) {
            wifi_mgr::poll();
            heartbeat::poll();
            xEventGroupSetBits(livenessGroup, LIVENESS_NETWORK);
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // task_input — Core 1, prio 4 — scanner + button polling
    // ─────────────────────────────────────────────────────────────────
    static void taskInput(void*) {
        esp_task_wdt_add(NULL);
        char buf[40];
        for (;;) {
            if (scanner::poll(buf, sizeof(buf))) {
                xQueueSend(qScanCodes, buf, 0);
            }
            ButtonGesture g;
            if (button::poll(g)) {
                xQueueSend(qButtonGestures, &g, 0);
            }
            evlog::pollCommands();   // "LOG DUMP" / "LOG CLEAR" / "LOG INFO" over USB serial
            xEventGroupSetBits(livenessGroup, LIVENESS_INPUT);
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // task_logic — Core 1, prio 3 — consumes scan/button queues
    // ─────────────────────────────────────────────────────────────────
    static void taskLogic(void*) {
        esp_task_wdt_add(NULL);
        char scanBuf[40];
        ButtonGesture g;
        for (;;) {
            if (xQueueReceive(qScanCodes, scanBuf, pdMS_TO_TICKS(50)) == pdTRUE) {
                processScan(scanBuf);
            }
            if (xQueueReceive(qButtonGestures, &g, 0) == pdTRUE) {
                handleButton(g);
            }
            xEventGroupSetBits(livenessGroup, LIVENESS_LOGIC);
            esp_task_wdt_reset();
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // task_display — Core 1, prio 2 — refresh OLED + LCD
    // ─────────────────────────────────────────────────────────────────
    static void taskDisplay(void*) {
        esp_task_wdt_add(NULL);
        for (;;) {
            OledKpis k = {};
            if (isProvisioned) {
                // Show tenant subdomain in header (more useful than "PAIRED")
                const char* sub = provisioning::getTenantSubdomain();
                if (sub && sub[0]) {
                    // Truncate to fit header
                    strncpy(k.deviceIdShort, sub, sizeof(k.deviceIdShort) - 1);
                } else {
                    strncpy(k.deviceIdShort, "PAIRED", sizeof(k.deviceIdShort) - 1);
                }
            } else {
                strncpy(k.deviceIdShort, "UNPAIR", sizeof(k.deviceIdShort) - 1);
            }
            k.scansToday      = scansToday;
            k.scansMorning    = scansMorning;
            k.scansAfternoon  = scansAfternoon;
            k.pendingOffline  = offline_log::count();
            {
                auto rk = heartbeat::getAttendanceKpis();
                k.hasRemoteKpis     = rk.hasData;
                k.remoteEntries     = rk.entries;
                k.remoteLates       = rk.lates;
                k.remoteUnregistered = rk.unregistered;
            }
            k.rssi            = wifi_mgr::getRssi();
            k.uptimeSec       = millis() / 1000;
            k.freeHeapKb      = ESP.getFreeHeap() / 1024;
            k.wifiConnected   = wifi_mgr::isConnected();
            k.ntpSynced       = wifi_mgr::isNtpSynced();
            k.studentsCached  = student_cache::count();
            k.schedulesCached = schedule_cache::count();
            if (!wifi_mgr::getClockStr(k.timeStr, sizeof(k.timeStr))) {
                uint32_t total = millis() / 1000;
                snprintf(k.timeStr, sizeof(k.timeStr), "%02u:%02u",
                         (unsigned)((7 + total / 3600) % 24), (unsigned)((total / 60) % 60));
            }
            display_oled::setKpis(k);

            // Push IP to TFT DIAG (only when connected)
            if (wifi_mgr::isConnected()) {
                auto st = wifi_mgr::getStatus();
                display_tft::setDeviceIp(st.ip);
            }

            char nowStr[8];
            if (!wifi_mgr::getClockStr(nowStr, sizeof(nowStr))) {
                strncpy(nowStr, k.timeStr, sizeof(nowStr));
            }
            display_lcd::setIdleStats(scansToday, nowStr);
            display_lcd::setOfflineCount(offline_log::count());

            display_oled::renderTick();
            display_lcd::renderTick();
            leds::tick();

            xEventGroupSetBits(livenessGroup, LIVENESS_DISPLAY);
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // task_watchdog — Core 1, prio 7 — checks all liveness bits ORed every 30s
    // Timeout raised from 15s → 30s because HTTP calls during cache pull can
    // block the network task for 10-20s when WiFi reconnects mid-request.
    // ─────────────────────────────────────────────────────────────────
    // ─────────────────────────────────────────────────────────────────
    // task_uploader — Core 0, prio 3 — dequeue scan jobs y POST al backend.
    // Aísla taskLogic del HTTP: si el POST tarda 5 s por WiFi lento, la
    // lógica sigue procesando los siguientes escaneos sin bloqueo perceptible.
    // ─────────────────────────────────────────────────────────────────
    static void taskUploader(void*) {
        esp_task_wdt_add(NULL);
        ScanUploadJob job;
        for (;;) {
            // Bloqueamos hasta 200 ms esperando trabajo. Si no llega, seteamos
            // liveness igual y volvemos a esperar. TWDT reset cada vuelta.
            if (xQueueReceive(qScanUploads, &job, pdMS_TO_TICKS(200)) == pdTRUE) {
                const char* mode =
                    (job.decision == (uint8_t)AttendanceDecision::SALIDA)        ? "exit" :
                    (job.decision == (uint8_t)AttendanceDecision::REFORZAMIENTO) ? "reinforcement"
                                                                                 : "entry";

                if (wifi_mgr::isConnected() && hmac_auth::hasCredentials()) {
                    JsonDocument doc;
                    doc["scanId"] = job.scanId;
                    doc["documentNumber"] = job.documentNumber;
                    doc["scannedAt"] = job.scannedAtIso[0]
                                        ? job.scannedAtIso
                                        : "1970-01-01T00:00:00-05:00";
                    doc["mode"] = mode;
                    if (job.ntpUncertain) doc["ntpUncertain"] = true;
                    if (job.scheduleId[0]) doc["scheduleId"] = job.scheduleId;
                    doc["offline"] = false;
                    String body;
                    serializeJson(doc, body);

                    uint32_t t0 = millis();
                    auto resp = http_client::post("/api/iot-attendance/scan", body.c_str());
                    uint32_t dur = millis() - t0;

                    if (resp.statusCode == 200 || resp.statusCode == 201) {
                        evlog::logf("[upl] OK %s in %lums\n", job.scanId, (unsigned long)dur);
                        // Delivered: retire it from the write-ahead queue so the
                        // batch sync skips it. If a cut lands before this marker
                        // the record is simply re-sent, which ingestion dedups.
                        offline_log::markSent(job.scanId);

                        // Actualiza el toast/footer solo si el backend disagreea
                        // y es una condición de error.
                        JsonDocument respDoc;
                        if (deserializeJson(respDoc, resp.body) == DeserializationError::Ok) {
                            const char* bstatus = respDoc["status"] | "";
                            const char* bstudent = respDoc["studentName"] | "";
                            uint8_t bcode = job.localToastCode;
                            if      (strcmp(bstatus, "out_of_window") == 0)      bcode = 3;
                            else if (strcmp(bstatus, "student_not_found") == 0)  bcode = 4;
                            else if (strcmp(bstatus, "no_active_schedule") == 0) bcode = 6;
                            else if (strcmp(bstatus, "error") == 0)              bcode = 7;
                            else if (strcmp(bstatus, "late") == 0)               bcode = 1;
                            else if (strcmp(bstatus, "reinforcement") == 0)      bcode = 9;
                            const bool needToast = (bcode == 3 || bcode == 4 || bcode == 6 || bcode == 7);
                            if (needToast) {
                                const char* nm = (bstudent && bstudent[0]) ? bstudent : job.studentNameShort;
                                display_tft::showScanToast(nm, job.gradeSection, job.timeStr, bcode, nullptr);
                            }
                        }
                    } else if (resp.statusCode == 409) {
                        // Already ingested — for us the same outcome as a 200.
                        evlog::logf("[upl] 409 idempotent %s\n", job.scanId);
                        offline_log::markSent(job.scanId);
                    } else {
                        // No append here any more: taskLogic persisted this
                        // record before the attempt started. It stays in the
                        // queue and the batch sync will carry it.
                        evlog::logf("[upl] fail %d dur=%lums — ya persistido, queda en cola\n",
                                      resp.statusCode, (unsigned long)dur);
                        display_tft::showScanToast(job.studentNameShort, job.gradeSection, job.timeStr,
                                                   resp.statusCode >= 500 ? 7 : 5,
                                                   resp.statusCode >= 500 ? "guardado offline" : "sin conexion");
                    }
                } else {
                    // Sin WiFi/credenciales — ya está en flash, nada que hacer.
                    evlog::logf("[upl] no WiFi/creds — %s queda en cola\n", job.scanId);
                    display_tft::showScanToast(job.studentNameShort, job.gradeSection, job.timeStr,
                                               5, "sin conexion");
                }
            }
            xEventGroupSetBits(livenessGroup, LIVENESS_UPLOADER);
            esp_task_wdt_reset();
        }
    }

    static void taskWatchdog(void*) {
        esp_task_wdt_add(NULL);
        for (;;) {
            // Wait up to 30s for ALL liveness bits to be set
            EventBits_t bits = xEventGroupWaitBits(
                livenessGroup, LIVENESS_ALL,
                pdTRUE,   // clear on exit
                pdTRUE,   // wait for ALL
                pdMS_TO_TICKS(30000));

            if ((bits & LIVENESS_ALL) != LIVENESS_ALL) {
                evlog::logf("[wdt] liveness lost: bits=0x%x — rebooting\n",
                              (unsigned)(bits & LIVENESS_ALL));
                ESP.restart();
            }
            esp_task_wdt_reset();
        }
    }

    void startAll() {
        qScanCodes      = xQueueCreate(8, 40);
        qButtonGestures = xQueueCreate(8, sizeof(ButtonGesture));
        qScanUploads    = xQueueCreate(16, sizeof(ScanUploadJob));
        livenessGroup   = xEventGroupCreate();

        // Initialize TWDT (30s timeout)
        esp_task_wdt_init(TWDT_TIMEOUT_S, true);

        xTaskCreatePinnedToCore(taskNetwork,  "net",  8192, NULL, 5, NULL, 0);
        xTaskCreatePinnedToCore(taskUploader, "upl",  8192, NULL, 3, NULL, 0);
        xTaskCreatePinnedToCore(taskInput,    "in",   4096, NULL, 4, NULL, 1);
        xTaskCreatePinnedToCore(taskLogic,    "lgc",  8192, NULL, 3, NULL, 1);
        xTaskCreatePinnedToCore(taskDisplay,  "dsp",  4096, NULL, 2, NULL, 1);
        xTaskCreatePinnedToCore(taskWatchdog, "wdt",  2048, NULL, 7, NULL, 1);

        evlog::logln("[tasks] all 6 tasks started");
    }
}
