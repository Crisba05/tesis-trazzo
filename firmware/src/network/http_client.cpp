// src/network/http_client.cpp
// HTTP client with HMAC-SHA256 device authentication.

#include "network/http_client.h"
#include "utils/evlog.h"
#include "network/hmac_auth.h"
#include "network/wifi_manager.h"
#include "network/tls_pinning.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// TLS strict mode is OFF by default for dev. Set TRAZZO_TLS_STRICT=1 in
// platformio.ini (or environment build_flags) to enforce cert validation
// against the bundled CA. Production builds MUST set this flag.
#ifndef TRAZZO_TLS_STRICT
#define TRAZZO_TLS_STRICT 0
#endif

namespace http_client {

    static char _baseUrl[128] = {0};
    static char _tenant[64] = {0};

    void begin(const char* baseUrl, const char* tenantSubdomain) {
        strncpy(_baseUrl, baseUrl ? baseUrl : "", sizeof(_baseUrl) - 1);
        strncpy(_tenant, tenantSubdomain ? tenantSubdomain : "", sizeof(_tenant) - 1);
        // Strip trailing slash
        size_t len = strlen(_baseUrl);
        if (len > 0 && _baseUrl[len - 1] == '/') _baseUrl[len - 1] = '\0';
        evlog::logf("[http] base=%s tenant=%s\n", _baseUrl, _tenant);
    }

    static String buildUrl(const char* path) {
        String url = String(_baseUrl);
        if (path[0] != '/') url += '/';
        url += path;
        return url;
    }

    static void addCommonHeaders(HTTPClient& http) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Tenant-Subdomain", _tenant);
    }

    static void addAuthHeaders(HTTPClient& http, const char* method,
                               const char* path, const uint8_t* body, size_t bodyLen) {
        if (!hmac_auth::hasCredentials()) return;

        hmac_auth::SignedHeaders headers;
        if (hmac_auth::sign(method, path, body, bodyLen, headers)) {
            // Backend DeviceAuthGuard expects deviceId + apiSecret in dedicated headers
            // (verifies sha256(X-Device-Key) == apiKeyHash). HTTPS protects them in transit.
            // X-Device-Nonce/Timestamp still provide replay protection.
            http.addHeader("X-Device-Id",        hmac_auth::getDeviceId());
            http.addHeader("X-Device-Key",       hmac_auth::getApiSecret());
            http.addHeader("X-Device-Timestamp", headers.timestamp);
            http.addHeader("X-Device-Nonce",     headers.nonce);
            // Keep HMAC Authorization for future hardening (Fase F)
            http.addHeader("Authorization",      headers.authorization);
        }
    }

    static Response doRequest(const char* method, const char* path,
                              const char* jsonBody, const char* ifNoneMatch,
                              bool withAuth) {
        Response resp = {-1, "", ""};

        if (!wifi_mgr::isConnected()) {
            evlog::logln("[http] no WiFi — skipping request");
            return resp;
        }

        String url = buildUrl(path);

        // DECLARATION ORDER IS LOAD-BEARING. Locals are destroyed in reverse
        // order, and ~HTTPClient() calls end() on the client it was handed. If
        // the client is declared after `http` it dies first, so that end() runs
        // against a destroyed object and closes an already-closed descriptor.
        // The VFS may by then have handed that number to a LittleFS file, and
        // the close lands on the file instead:
        //   assert failed: lfs_file_close lfs.c:6080 (lfs_mlist_isopen(...))
        // That is the crash seen in Scenario D (2026-09-27 09:48:23), where a
        // burst produced enough failed TLS connects and file writes for the
        // descriptor numbers to collide. Clients first, `http` last.
        WiFiClient client;
        WiFiClientSecure secClient;
        HTTPClient http;

        bool isHttps = url.startsWith("https");
        if (isHttps) {
#if TRAZZO_TLS_STRICT
            // Production: validate against the pinned CA bundle.
            secClient.setCACert(TRAZZO_CA_BUNDLE_PEM);
#else
            // Dev: still uses TLS but skips cert validation. Safe for local
            // testing only — never deploy to prod with this on.
            secClient.setInsecure();
#endif
            http.begin(secClient, url);
        } else {
            http.begin(client, url);
        }

        // Cap por request para no bloquear la task uploader más de 8 s si el
        // backend cuelga la conexión. HTTPClient default es 5000 ms socket +
        // sin límite total — con backend flaky ese sin-límite explotaba TWDT.
        http.setTimeout(8000);
        http.setConnectTimeout(4000);

        addCommonHeaders(http);

        if (ifNoneMatch && ifNoneMatch[0]) {
            http.addHeader("If-None-Match", ifNoneMatch);
        }

        if (withAuth) {
            const uint8_t* bodyBytes = jsonBody ? (const uint8_t*)jsonBody : nullptr;
            size_t bodyLen = jsonBody ? strlen(jsonBody) : 0;
            addAuthHeaders(http, method, path, bodyBytes, bodyLen);
        }

        // Collect specific headers from response
        const char* headerKeys[] = {"ETag", "etag"};
        http.collectHeaders(headerKeys, 2);

        int code;
        if (strcmp(method, "POST") == 0) {
            code = http.POST(jsonBody ? jsonBody : "");
        } else {
            code = http.GET();
        }

        resp.statusCode = code;

        if (code > 0) {
            // Read the FULL body — do NOT truncate. Roster pages are ~25KB
            // (100 students × ~250 bytes). The backend already paginates so
            // no single response should exceed ~30KB. Truncation here was
            // silently corrupting the JSON, breaking student sync.
            resp.body = http.getString();

            // Sanity limit: refuse anything huge (memory safety).
            static constexpr size_t MAX_BODY = 64 * 1024;
            if (resp.body.length() > MAX_BODY) {
                evlog::logf("[http][WARN] body %d > %d bytes — clearing\n",
                              (int)resp.body.length(), (int)MAX_BODY);
                resp.body = "";
                resp.statusCode = -1;   // signal read failure to caller
            }

            if (http.hasHeader("ETag")) {
                resp.etag = http.header("ETag");
            } else if (http.hasHeader("etag")) {
                resp.etag = http.header("etag");
            }
        }

        // Release the connection while the client objects are still alive,
        // instead of leaving it to the destructors (see the note above).
        http.end();

        evlog::logf("[http] %s %s -> %d (%d bytes)\n",
                      method, path, code, resp.body.length());

        http.end();
        return resp;
    }

    Response get(const char* path, const char* ifNoneMatch) {
        return doRequest("GET", path, nullptr, ifNoneMatch, true);
    }

    Response post(const char* path, const char* jsonBody) {
        return doRequest("POST", path, jsonBody, nullptr, true);
    }

    Response postPublic(const char* path, const char* jsonBody) {
        return doRequest("POST", path, jsonBody, nullptr, false);
    }

    Response getPublic(const char* path) {
        return doRequest("GET", path, nullptr, nullptr, false);
    }
}
