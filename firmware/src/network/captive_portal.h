// src/network/captive_portal.h
// Captive portal for initial WiFi provisioning.
// ESP32 becomes an AP ("TRAZZO-XXXXXX"), serves a config form,
// saves credentials to NVS, and reboots into station mode.
#pragma once

#include <Arduino.h>

namespace captive_portal {

    // Start the AP + web server. Blocks until the user submits the form
    // or timeout (5 minutes). Returns true if credentials were saved.
    // Call from main loop when button long-press triggers provisioning,
    // or on first boot when NVS has no WiFi credentials.
    bool start();

    // Stop AP and web server, free resources.
    void stop();

    // True while the portal is active.
    bool isActive();
}
