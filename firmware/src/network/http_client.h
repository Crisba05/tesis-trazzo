// src/network/http_client.h
// HTTP client wrapper with HMAC-SHA256 authentication.
// All requests include device auth headers when credentials are available.
#pragma once

#include <Arduino.h>

namespace http_client {

    struct Response {
        int    statusCode;     // HTTP status (200, 401, etc.) or -1 on network error
        String body;           // response body (truncated to 4KB)
        String etag;           // ETag header if present
    };

    // Must be called after wifi_mgr::begin() and credentials are loaded.
    void begin(const char* baseUrl, const char* tenantSubdomain);

    // GET with optional ETag for cache validation (304 Not Modified).
    Response get(const char* path, const char* ifNoneMatch = nullptr);

    // POST with JSON body.
    Response post(const char* path, const char* jsonBody);

    // POST without auth (for public endpoints like provisioning claim).
    Response postPublic(const char* path, const char* jsonBody);

    // GET without auth (for provisioning poll).
    Response getPublic(const char* path);
}
