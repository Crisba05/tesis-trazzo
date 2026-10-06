/**
 * serial_capture.mjs — captura y parsea el serial del nodo ESP32 durante los escenarios.
 *
 * Sella cada línea con el reloj del host (ms), guarda el crudo en NDJSON y deriva
 * métricas por "corte de red" (outage) con precisión de máquina, a partir de los
 * marcadores que el firmware ya imprime ([wifi], [offline], [sync], reset reason…).
 *
 * Requisito (una sola vez):
 *     cd tesis/analisis/scripts/scenarios_capture && npm init -y && npm i serialport
 *
 * Uso:
 *     node serial_capture.mjs --port COM5 --scn A --baud 115200
 *     (lista de puertos)  node serial_capture.mjs --list
 *
 * Salidas (carpeta ../../logs):
 *     scenario_<scn>_serial_<YYYY-MM-DD>_<HHMMSS>.ndjson   ← crudo sellado
 *     scenario_<scn>_events_<...>.json                     ← eventos parseados + outages
 * Ctrl+C para cerrar y volcar el resumen.
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parse, OutageTracker, EpisodeTracker } from "./firmware_rules.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const LOGS = path.resolve(HERE, "..", "..", "logs");
fs.mkdirSync(LOGS, { recursive: true });

// ---- args ----
const A = process.argv.slice(2);
const arg = (k, d) => { const i = A.indexOf(k); return i >= 0 ? A[i + 1] : d; };
const PORT = arg("--port");
const BAUD = parseInt(arg("--baud", "115200"), 10);
const SCN = (arg("--scn", "X") || "X").toUpperCase();
const LIST = A.includes("--list");

let SerialPortMod;
try { SerialPortMod = await import("serialport"); }
catch { console.error("Falta 'serialport'. Instala:  npm i serialport"); process.exit(1); }
const { SerialPort } = SerialPortMod;

if (LIST) {
  const ports = await SerialPort.list();
  console.log("Puertos disponibles:");
  for (const p of ports) console.log(`  ${p.path}  ${p.manufacturer || ""} ${p.friendlyName || ""}`);
  process.exit(0);
}
if (!PORT) { console.error("Indica el puerto:  --port COM5   (o --list para verlos)"); process.exit(1); }

// ---- files ----
const stamp = new Date().toISOString().replace(/[:T]/g, "-").slice(0, 19);
const rawPath = path.join(LOGS, `scenario_${SCN}_serial_${stamp}.ndjson`);
const evPath  = path.join(LOGS, `scenario_${SCN}_events_${stamp}.json`);
const rawStream = fs.createWriteStream(rawPath, { flags: "a" });
const events = [];

// ---- outage state machine (shared with dump_device_log.mjs) ----
const tracker = new OutageTracker();
const outages = tracker.outages;
const epTracker = new EpisodeTracker();     // first failed upload -> SYNC_OK (survives reboots)
const episodes = epTracker.episodes;
function onEvent(ev, t) {
  const o = tracker.onEvent(ev, t);
  if (o) {
    console.log(`
  ✔ OUTAGE cerrado: corte=${o.outage_s}s  sync=${o.sync_latency_s}s  ` +
      `offline=${o.offline_appended}  colaMax=${o.max_queue_depth}  drops=${o.drops}
`);
  }
  const ep = epTracker.onEvent(ev, t);
  if (ep) {
    console.log(`
  ✔ EPISODIO cerrado: degradado=${ep.degraded_s}s  offline=${ep.offline_appended}  ` +
      `recuperados=${ep.recovered_at_sync}  perdidos(est)=${ep.lost_estimate}  reinicios=${ep.reboots_during}
`);
  }
}

// ---- open port, with auto-reconnect ----
// Escenario C corta la alimentacion a proposito -> el puerto USB desaparece.
// En vez de morir, seguimos vivos: detectamos la caida, la anotamos como
// evento (DEVICE_DISCONNECTED/RECONNECTED) en el MISMO archivo, y reintentamos
// abrir el puerto cada 1s hasta que el dispositivo vuelva.
console.log(`Escuchando ${PORT} @ ${BAUD}  escenario=${SCN}`);
console.log(`Crudo:   ${rawPath}`);
console.log(`Eventos: ${evPath}\n(Ctrl+C para cerrar y volcar resumen — sobrevive cortes de energia/USB)\n`);

let currentPort = null;
let reconnecting = false;
let shuttingDown = false;

function logSynthetic(type, extra) {
  const t = Date.now();
  const rec = { t, iso: new Date(t).toISOString(), type, ...extra };
  events.push(rec);
  rawStream.write(JSON.stringify({ t, iso: rec.iso, line: `<${type}>` }) + "\n");
  return rec;
}

function scheduleReconnect() {
  if (reconnecting || shuttingDown) return;
  reconnecting = true;
  logSynthetic("DEVICE_DISCONNECTED");
  console.log(`\n  ⚠ Puerto perdido (¿corte de energía/USB?) — reintentando cada 1s...\n`);
  const timer = setInterval(async () => {
    if (shuttingDown) { clearInterval(timer); return; }
    try {
      const ports = await SerialPort.list();
      if (!ports.some(p => p.path === PORT)) return; // aun no vuelve
      clearInterval(timer);
      openPort(true);
    } catch { /* sigue reintentando */ }
  }, 1000);
}

function openPort(isReconnect) {
  const p = new SerialPort({ path: PORT, baudRate: BAUD }, e => {
    if (e) {
      if (isReconnect) { reconnecting = false; scheduleReconnect(); }
      else { console.error("No pude abrir el puerto:", e.message); process.exit(1); }
      return;
    }
    if (isReconnect) {
      reconnecting = false;
      logSynthetic("DEVICE_RECONNECTED");
      console.log(`\n  ✅ Puerto recuperado — reanudando captura (mismo archivo)\n`);
    }
  });
  currentPort = p;
  let buf = "";
  p.on("data", chunk => {
    buf += chunk.toString("utf8");
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i).replace(/\r$/, ""); buf = buf.slice(i + 1);
      const t = Date.now();
      rawStream.write(JSON.stringify({ t, iso: new Date(t).toISOString(), line }) + "\n");
      const ev = parse(line);
      if (ev) {
        const rec = { t, iso: new Date(t).toISOString(), ...ev, raw: line }; events.push(rec); onEvent(ev, t);
        if (ev.type === "LOOKUP_NOT_FOUND") {
          console.log(`\n  ⚠ NO ENCONTRADO en caché local: code=${ev.code} cacheSize=${ev.cacheSize}\n`);
        } else if (ev.type !== "HTTP" && ev.type !== "LOOKUP_FOUND") {
          console.log(`  · ${ev.type}${ev.pending!==undefined?` pending=${ev.pending}`:""}${ev.reason?` (${ev.reason})`:""}`);
        }
      }
      else process.stdout.write(".");
    }
  });
  p.on("error", e => { console.error("\nserial error:", e.message); if (!shuttingDown) scheduleReconnect(); });
  p.on("close", () => { if (!shuttingDown) scheduleReconnect(); });
}
openPort(false);

// ---- shutdown: dump events + summary ----
function dump() {
  const out = {
    meta: { tool: "serial_capture", scenario: SCN, port: PORT, baud: BAUD,
      generatedAt: new Date().toISOString(),
      note: "Timestamps = host clock (ms). outages[] derivado de WIFI_DOWN→WIFI_UP→SYNC_OK." },
    summary: {
      total_events: events.length,
      boots: events.filter(e => e.type === "BOOT").length,
      resets: events.filter(e => e.type === "RESET_REASON").map(e => e.reason),
      lfs_failures: events.filter(e => e.type === "LFS_FAIL").length,
      offline_drops: events.filter(e => e.type === "OFFLINE_DROP").length,
      lookups_not_found: events.filter(e => e.type === "LOOKUP_NOT_FOUND")
        .map(e => ({ code: e.code, cacheSize: e.cacheSize, at: e.iso })),
      power_cuts_detected: events.filter(e => e.type === "DEVICE_DISCONNECTED").length,
      outages,
      episodes,
    },
    events,
  };
  fs.writeFileSync(evPath, JSON.stringify(out, null, 2));
  console.log(`\n── resumen ──`);
  console.log(`eventos=${events.length}  outages=${outages.length}  cortes=${out.summary.power_cuts_detected}  ` +
    `drops=${out.summary.offline_drops}  lfs_fail=${out.summary.lfs_failures}  boots=${out.summary.boots}`);
  if (outages.length) console.table(outages);
  console.log(`Guardado: ${evPath}`);
}
process.on("SIGINT", () => {
  shuttingDown = true;
  try { currentPort && currentPort.isOpen && currentPort.close(); } catch { /* noop */ }
  rawStream.end();
  dump();
  process.exit(0);
});
