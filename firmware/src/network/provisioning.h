// src/network/provisioning.h
// Device provisioning: claim registration + poll for pairing completion.
// See PLAN_V2 §3 for the full flow.
#pragma once

#include <Arduino.h>

namespace provisioning {

    enum class State : uint8_t {
        NOT_PROVISIONED,   // no credentials in NVS
        CLAIMING,          // sent claim, waiting for admin to pair
        PROVISIONED,       // paired, credentials in NVS
    };

    // Check NVS for existing credentials. Returns current state.
    State checkState();

    // Generate pairing code, register with backend, then poll until
    // admin pairs the device or timeout (5 min). Returns true if paired.
    // Shows pairing code on OLED during the process.
    bool runClaimAndPoll();

    // Load deviceId + apiSecret from NVS into hmac_auth.
    // Call after checkState() returns PROVISIONED.
    bool loadCredentials();

    // Get the current pairing code (valid during CLAIMING).
    const char* getPairingCode();

    // Get the loaded backend URL from NVS.
    const char* getBackendUrl();

    // Get the loaded tenant subdomain from NVS.
    const char* getTenantSubdomain();

    // SECURITY: compares current tenant in NVS_NS_CONFIG against the one captured
    // at pair time (NVS_KEY_TENANT_AT_PAIR). If they differ, wipes device credentials
    // and reboots — prevents cross-tenant cache contamination if a device is re-flashed
    // or reassigned without a factory reset. Call once during setup() after
    // loadCredentials(). Returns true if a tenant change was detected (won't return —
    // device reboots).
    bool checkTenantConsistency();
}
