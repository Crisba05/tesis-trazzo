// src/drivers/button.h
// Debounced GPIO button classified into ButtonGesture events.
// Designed to be polled from a low-priority FreeRTOS task. ISR-free for clarity.
#pragma once

#include <Arduino.h>
#include "types.h"

namespace button {
    void begin();

    // Returns true and fills `gesture` if a gesture was just classified.
    // Call this from a polling task every 10–20ms.
    bool poll(ButtonGesture& gesture);
}
