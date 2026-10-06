# Fault-tolerant IoT attendance node (ESP32 + GM65)

Source code, capture tooling and raw experiment logs for the paper
*"Nodo IoT tolerante a fallos usando ESP32 y GM65 para el registro de asistencia escolar"*
(C. S. Bacilio Vilca, O. I. Burga de la Cruz — Universidad Tecnológica del Perú).

The node reads student credentials with a GM65 scanner, classifies each read
locally, persists it to flash **before** attempting upload (write-ahead), and
synchronises its local queue with the server in bounded batches when
connectivity returns.

## Contents

| Path | What it is |
|---|---|
| `firmware/` | ESP32 firmware (C++17, PlatformIO, Arduino/ESP-IDF, FreeRTOS, LittleFS) |
| `backend/iot-attendance/` | Server-side ingestion module (NestJS): idempotency by `scanId`, per-record batch validation, regression test |
| `experiments/capture/` | Serial capture and device-log tools used during the scenarios |
| `experiments/logs/` | Raw serial captures of scenarios A–D (`.ndjson`, one console line per record, host-timestamped) |
| `experiments/reports/` | Per-scenario results and run guides (Spanish) |
| `docs/` | Node–server sequence diagram |

Key files: `firmware/src/tasks/tasks.cpp` (write-ahead capture),
`firmware/src/logic/offline_log.cpp` (local queue),
`firmware/src/logic/heartbeat.cpp` (chunked sync),
`firmware/src/network/wifi_manager.cpp` (clock persistence),
`backend/iot-attendance/iot-attendance.service.ts`.

## Scope and limitations of this repository

- `firmware/` is the firmware as it stood **after** the corrections derived from
  scenarios C and D. Scenarios A and B were run on an earlier build; each report
  states the build used.
- `backend/iot-attendance/` is an excerpt of a larger multi-tenant platform that
  is not published. It does not build on its own; it is provided for inspection.
- Not every file in `experiments/logs/` is a valid run: pilots and aborted
  captures are kept as recorded. `experiments/logs/INDICE_LOGS_C_D.md` and the
  reports identify the runs behind each reported figure. Three captures of
  scenario D that contain only corrupted bytes (the serial port was opened by
  several processes at once) were left out.
- Network identifiers in logs and source were replaced: the server host
  (`api.example.org`), the Wi-Fi SSID (`WIFI_LAB`) and one MAC address. No other
  log content was altered.
- All credential codes in the logs are synthetic (`999…`) and belong to an
  anonymised demonstration tenant. The repository contains no personal data and
  no device or server secrets; device credentials are provisioned at pairing
  time and stored in NVS.

## Building the firmware

```bash
cd firmware
pio run -e esp32-dev
```

Environment names are defined in `firmware/platformio.ini`. Set
`TRAZZO_DEFAULT_BACKEND_URL` to your own server.

## License

Code: MIT (`LICENSE`). Experiment logs, reports and figures: CC BY 4.0 (`LICENSE-DATA`).
