// src/network/hmac_auth.cpp
// HMAC-SHA256 signing for device authentication.
// Signature = HMAC(secret, METHOD|PATH|timestamp|nonce|SHA256(body))

#include "network/hmac_auth.h"
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <esp_random.h>

namespace hmac_auth {

    static char _deviceId[32] = {0};
    static char _apiSecret[65] = {0};

    void setCredentials(const char* deviceId, const char* apiSecret) {
        strncpy(_deviceId, deviceId ? deviceId : "", sizeof(_deviceId) - 1);
        strncpy(_apiSecret, apiSecret ? apiSecret : "", sizeof(_apiSecret) - 1);
    }

    bool hasCredentials() {
        return _deviceId[0] != '\0' && _apiSecret[0] != '\0';
    }

    const char* getDeviceId()  { return _deviceId; }
    const char* getApiSecret() { return _apiSecret; }

    // SHA-256 hash of arbitrary data, output as 64 hex chars + null.
    static void sha256hex(const uint8_t* data, size_t len, char* out) {
        uint8_t hash[32];
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
        mbedtls_md_starts(&ctx);
        mbedtls_md_update(&ctx, data, len);
        mbedtls_md_finish(&ctx, hash);
        mbedtls_md_free(&ctx);
        for (int i = 0; i < 32; i++) {
            sprintf(out + i * 2, "%02x", hash[i]);
        }
        out[64] = '\0';
    }

    // Generate 16 random bytes as 32 hex chars.
    static void generateNonce(char* out) {
        uint8_t buf[16];
        esp_fill_random(buf, sizeof(buf));
        for (int i = 0; i < 16; i++) {
            sprintf(out + i * 2, "%02x", buf[i]);
        }
        out[32] = '\0';
    }

    // HMAC-SHA256, output as raw bytes (32 bytes).
    static void hmacSha256(const char* key, size_t keyLen,
                           const uint8_t* msg, size_t msgLen,
                           uint8_t* out) {
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, keyLen);
        mbedtls_md_hmac_update(&ctx, msg, msgLen);
        mbedtls_md_hmac_finish(&ctx, out);
        mbedtls_md_free(&ctx);
    }

    bool sign(const char* method, const char* path,
              const uint8_t* body, size_t bodyLen,
              SignedHeaders& out) {

        if (!hasCredentials()) return false;

        // Timestamp
        time_t now;
        time(&now);
        snprintf(out.timestamp, sizeof(out.timestamp), "%ld", (long)now);

        // Nonce
        generateNonce(out.nonce);

        // SHA256(body)
        char bodyHash[65];
        if (body && bodyLen > 0) {
            sha256hex(body, bodyLen, bodyHash);
        } else {
            sha256hex((const uint8_t*)"", 0, bodyHash);
        }

        // Build signing string: METHOD|PATH|timestamp|nonce|bodyHash
        char signingStr[512];
        snprintf(signingStr, sizeof(signingStr), "%s|%s|%s|%s|%s",
                 method, path, out.timestamp, out.nonce, bodyHash);

        // HMAC
        uint8_t hmacRaw[32];
        hmacSha256(_apiSecret, strlen(_apiSecret),
                   (const uint8_t*)signingStr, strlen(signingStr),
                   hmacRaw);

        // Base64 encode the HMAC
        char sigB64[48];
        size_t olen = 0;
        mbedtls_base64_encode((uint8_t*)sigB64, sizeof(sigB64), &olen,
                              hmacRaw, 32);
        sigB64[olen] = '\0';

        // Build Authorization header
        snprintf(out.authorization, sizeof(out.authorization),
                 "TRAZZO-HMAC keyId=%s, ts=%s, nonce=%s, sig=%s",
                 _deviceId, out.timestamp, out.nonce, sigB64);

        return true;
    }
}
