// src/events.cpp
#include "events.h"

namespace events {
    EventGroupHandle_t systemEvents = nullptr;

    void init() {
        if (systemEvents == nullptr) {
            systemEvents = xEventGroupCreate();
        }
    }
}
