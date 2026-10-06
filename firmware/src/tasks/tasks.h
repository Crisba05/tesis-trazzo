// src/tasks/tasks.h
// FreeRTOS task orchestration. Replaces the cooperative loop with 5 tasks
// pinned across both ESP32 cores. Each task feeds the watchdog via the
// liveness event group; if any stops feeding for >15s task_watchdog reboots.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include "types.h"

namespace tasks {

    // Liveness bits — each task ORs its bit into livenessGroup periodically.
    constexpr EventBits_t LIVENESS_NETWORK  = (1 << 0);
    constexpr EventBits_t LIVENESS_INPUT    = (1 << 1);
    constexpr EventBits_t LIVENESS_LOGIC    = (1 << 2);
    constexpr EventBits_t LIVENESS_DISPLAY  = (1 << 3);
    constexpr EventBits_t LIVENESS_UPLOADER = (1 << 4);
    constexpr EventBits_t LIVENESS_ALL =
        LIVENESS_NETWORK | LIVENESS_INPUT | LIVENESS_LOGIC | LIVENESS_DISPLAY | LIVENESS_UPLOADER;

    // Cross-task queues
    extern QueueHandle_t    qScanCodes;       // char[40] — scanned codes from input -> logic
    extern QueueHandle_t    qButtonGestures;  // ButtonGesture — input -> logic
    extern QueueHandle_t    qScanUploads;     // ScanUploadJob — logic -> uploader
    extern EventGroupHandle_t livenessGroup;

    // Payload enviado de taskLogic → taskUploader. Se copia por valor a la
    // cola FreeRTOS; sin heap allocations. Suficiente para el body JSON.
    struct ScanUploadJob {
        char     scanId[40];
        char     documentNumber[24];
        char     scannedAtIso[32];
        char     scheduleId[40];
        char     studentNameShort[32];   // para actualizar last-scan del TFT tras respuesta
        char     gradeSection[8];
        char     timeStr[8];
        uint8_t  decision;               // AttendanceDecision as uint8
        uint8_t  localToastCode;         // el toast que ya mostró taskLogic
        bool     ntpUncertain;
    };

    // Shared atomic state (writers documented below)
    extern volatile uint32_t scansToday;       // logic writes (total)
    extern volatile uint32_t scansMorning;     // logic writes (before 13:00)
    extern volatile uint32_t scansAfternoon;   // logic writes (13:00+)
    extern volatile bool     isProvisioned;    // logic writes

    // Start all tasks. Call after all subsystems are initialized in setup().
    void startAll();

    // Wipe every device-scoped store (all NVS namespaces + LittleFS format)
    // and reboot. Callable from button gesture OR remote `factory_reset`
    // command. Blocks ~2.5s to give the user visual confirmation.
    void factoryResetTotal();

    // Lighter reset used when the backend deprovisions the device (deleted).
    // Wipes ONLY device credentials (NVS_NS_DEVICE) + LittleFS caches, but
    // KEEPS WiFi config (NVS_NS_CONFIG) so the module can be re-paired over
    // BLE without reconfiguring the network. Blocks ~2.5s then reboots.
    void unpairSelf();
}
