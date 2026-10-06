// src/network/hmac_auth.h
// HMAC-SHA256 request signing per PLAN_V2 §8.
// Produces the Authorization header:
//   TRAZZO-HMAC keyId=<deviceId>, ts=<unix>, nonce=<16B hex>, sig=<base64>
#pragma once

#include <Arduino.h>

namespace hmac_auth {

    // Set the device credentials (loaded from NVS after pairing).
    void setCredentials(const char* deviceId, const char* apiSecret);

    // Returns true if credentials are loaded.
    bool hasCredentials();

    // Accessors needed by http_client for X-Device-Id / X-Device-Key headers
    const char* getDeviceId();
    const char* getApiSecret();

    // Build the HMAC signature and populate the output headers.
    // method: "POST" or "GET"
    // path:   "/api/iot-attendance/scan"
    // body:   request body (nullptr for GET)
    // bodyLen: length of body
    //
    // Out params (caller provides buffers):
    //   authHeader:  full "TRAZZO-HMAC keyId=..., ts=..., nonce=..., sig=..." string
    //   timestamp:   unix epoch string
    //   nonce:       32 hex chars
    struct SignedHeaders {
        char authorization[256];
        char timestamp[16];
        char nonce[36];
    };

    bool sign(const char* method, const char* path,
              const uint8_t* body, size_t bodyLen,
              SignedHeaders& out);
}
