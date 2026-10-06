// src/logic/offline_log.h
// Append-only offline scan log in LittleFS (NDJSON).
// Each line: {"code":"...","scannedAt":"...","scheduleId":"...","decision":N}
#pragma once

#include <Arduino.h>
#include "types.h"
#include <vector>

namespace offline_log {

    // Create the queue mutex. Call once from setup(), before tasks start.
    // taskLogic appends every scan (write-ahead) while taskUploader marks
    // records delivered and may compact the files, so the two must not
    // interleave: a compaction racing an append would drop a live scan.
    void begin();

    // Persist a scan record. This is WRITE-AHEAD: it runs before the upload is
    // attempted, not as a fallback when the upload fails.
    //
    // The old order (try to upload, persist on failure) left every scan living
    // only in RAM for as long as the HTTP attempt took to give up — measured at
    // 4.1 to 19.4 s with no connectivity. A power cut in that window destroyed
    // the event outright: in the Scenario C run of 2026-09-27, 6 of 14 scans
    // were lost that way, while the flash write itself survived all 22 cuts of
    // the campaign. Persisting first shrinks the exposure to this one append.
    //
    // scanId is a UUIDv7 string for end-to-end idempotency.
    // ntpUncertain marks a record whose timestamp is approximate (clock restored
    // from NVS after a power cut, or absent altogether); it travels with the
    // record so the backend can tell an approximate time from an exact one.
    // An empty scannedAtIso is written with SCAN_EPOCH_PLACEHOLDER and forces
    // ntpUncertain — never dropped, since losing the scan is the worse outcome.
    bool append(const char* scanId, const char* code, const char* scannedAtIso,
                const char* scheduleId, AttendanceDecision decision,
                bool ntpUncertain = false);

    // Note that the backend already accepted this scanId through the live path,
    // so the next batch sync skips it. Appending a marker keeps this O(1) and
    // power-cut safe; rewriting the queue on every scan would not be.
    //
    // If a cut lands between the successful upload and this marker, the record
    // is simply re-sent in the next batch. That is harmless: ingestion is
    // idempotent by scanId, and attendance upserts by (tenant, student, date).
    bool markSent(const char* scanId);

    // Move every pending record to a quarantine file and empty the queue.
    // Used when the backend answers a permanent error (4xx) for the batch:
    // retrying it would block the queue forever, so the records are parked
    // where they remain available as evidence instead of being lost silently.
    // Returns how many records were parked.
    uint32_t quarantineAll();

    // Up to `maxRecords` records not yet acknowledged, returned as the COMPLETE
    // request body `{"scans":[...]}`; `ids` receives the scanIds included so the
    // caller can mark them once the backend accepts them. The caller must not
    // wrap it again — building the wrapper separately doubles peak memory and
    // starves the TLS handshake.
    //
    // Chunked on purpose: the whole queue in one request cannot be built on
    // this device past ~100 records (payload String + batch copy + HMAC + TLS
    // buffers against ~83 KB of heap), and the failure mode is a permanent -1.
    // See SYNC_BATCH_MAX_RECORDS.
    String readBatch(uint16_t maxRecords, std::vector<String>& ids);

    // All pending records in one string. Kept for callers that need the whole
    // queue; prefer readBatch() for anything that goes over the network.
    String readAll();

    // Clear the queue and its sent markers after a successful sync.
    void clear();

    // Number of records still waiting to be sent (persisted minus acknowledged).
    uint32_t count();

    // Read-only integrity report of the pending file — used by evlog at boot
    // to record what survived a reset / power cut. NEVER modifies the file
    // (measuring the system must not change it).
    struct VerifyResult {
        uint32_t bytes;        // file size
        uint32_t lines;        // newline-terminated lines
        uint32_t valid;        // lines that parse as a JSON object with a scanId
        uint32_t tailBytes;    // bytes after the last '\n' (>0 => truncated last write)
    };
    VerifyResult verify();
}
