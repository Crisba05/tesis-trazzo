// src/drivers/scanner.cpp
// GM65 scanner on Serial2 (9600 baud) or Serial mock for Wokwi.
//
// GM65 factory-default output framing varies by firmware batch:
//   * CR+LF, CR only, LF only, or NO terminator at all.
// This parser accepts all four:
//   - closes buffer on '\n' OR '\r'
//   - closes buffer on inter-byte silence >= FRAME_GAP_MS  (timeout framing)
// A raw-byte debug dump lets us see what the scanner actually sends.

#include "drivers/scanner.h"
#include "utils/evlog.h"
#include "config.h"

namespace scanner {

    static char     _buf[64];
    static uint8_t  _pos = 0;
    static uint32_t _lastByteMs = 0;
    static bool     _debugRaw = false;     // scanner confirmed working — silenced
    static constexpr uint32_t FRAME_GAP_MS = 80;  // consider frame closed after this silence

#if defined(TRAZZO_SCANNER_MOCK) && TRAZZO_SCANNER_MOCK
    static HardwareSerial& _serial = Serial;
#else
    static HardwareSerial& _serial = Serial2;
#endif

    void begin() {
#if !defined(TRAZZO_SCANNER_MOCK) || !TRAZZO_SCANNER_MOCK
        Serial2.begin(9600, SERIAL_8N1, pins::SCANNER_RX, pins::SCANNER_TX);
        evlog::logln("[scanner] GM65 on Serial2 @ 9600");
#else
        evlog::logln("[scanner] mock mode — type codes in Serial Monitor");
#endif
        _pos = 0;
        _lastByteMs = 0;
    }

    static bool emitFrame(char* buf, size_t bufLen) {
        // Trim leading/trailing whitespace-ish bytes (some scanners prepend NUL or ETX).
        int start = 0;
        while (start < _pos && (_buf[start] <= 0x20)) start++;
        int end = _pos;
        while (end > start && (_buf[end - 1] <= 0x20)) end--;
        int len = end - start;
        _pos = 0;
        if (len <= 0) return false;

        size_t copyLen = ((size_t)len < bufLen - 1) ? (size_t)len : bufLen - 1;
        memcpy(buf, _buf + start, copyLen);
        buf[copyLen] = '\0';
        evlog::logf("[scanner] frame: '%s' (%d bytes)\n", buf, (int)copyLen);
        return true;
    }

    bool poll(char* buf, size_t bufLen) {
        while (_serial.available()) {
            char ch = (char)_serial.read();
            _lastByteMs = millis();

            if (_debugRaw) {
                // Show every incoming byte as hex + char for diagnostics.
                evlog::logf("[scanner][raw] 0x%02X '%c'\n",
                              (uint8_t)ch, (ch >= 0x20 && ch < 0x7F) ? ch : '.');
            }

            // Explicit terminator: CR or LF closes the frame.
            if (ch == '\n' || ch == '\r') {
                if (_pos > 0) return emitFrame(buf, bufLen);
                continue;
            }
            if (_pos < sizeof(_buf) - 1) {
                _buf[_pos++] = ch;
            }
        }

        // Timeout-based framing: if we have data buffered and no new byte
        // arrived for FRAME_GAP_MS, close the frame (scanner sent no
        // terminator, but paused between scans).
        if (_pos > 0 && (millis() - _lastByteMs) > FRAME_GAP_MS) {
            return emitFrame(buf, bufLen);
        }
        return false;
    }
}
