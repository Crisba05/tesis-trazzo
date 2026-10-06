// include/events.h
// FreeRTOS event group bits for cross-task signalling.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

namespace events {
    // System-wide event group bits (single EventGroupHandle_t shared by all tasks)
    constexpr EventBits_t WIFI_CONNECTED     = (1 << 0);
    constexpr EventBits_t WIFI_DISCONNECTED  = (1 << 1);
    constexpr EventBits_t NTP_SYNCED         = (1 << 2);
    constexpr EventBits_t PROVISIONED        = (1 << 3);
    constexpr EventBits_t CONFIG_UPDATED     = (1 << 4);
    constexpr EventBits_t SYNC_REQUESTED     = (1 << 5);
    constexpr EventBits_t OTA_PENDING        = (1 << 6);
    constexpr EventBits_t FACTORY_RESET_REQ  = (1 << 7);
    constexpr EventBits_t BUTTON_SHORT       = (1 << 8);
    constexpr EventBits_t BUTTON_LONG        = (1 << 9);
    constexpr EventBits_t BUTTON_DOUBLE      = (1 << 10);
    constexpr EventBits_t SAFE_MODE          = (1 << 11);

    extern EventGroupHandle_t systemEvents;
    void init();
}
