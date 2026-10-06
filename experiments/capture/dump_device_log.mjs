/**
 * dump_device_log.mjs — download + analyse the module's OWN persistent log.
 *
 * The firmware (evlog) keeps a timeline in its flash that survives resets and
 * POWER CUTS, so you no longer need a PC attached while running a scenario
 * (run it on a power bank, cut power however you like) and just download the
 * log once at the end.
 *
 * USB (module plugged in, nothing else holding the COM port):
 *     node dump_device_log.mjs --port COM4 --scn C
 *     node dump_device_log.mjs --port COM4 --scn C --clear      # also wipe the log after a COMPLETE dump
 *
 * WiFi (hold the button >5 s -> portal opens -> join the module's AP ->
 *       browse http://192.168.4.1/log  [and /log1 for the older rotated part]):
 *     node dump_device_log.mjs --analyze log.txt [log1.txt] --scn C
 *
 * Outputs (../../logs):
 *     device_log_<scn>_<stamp>.log      raw log exactly as stored on the module (evidence)
 *     device_log_<scn>_<stamp>.ndjson   one record per line with reconstructed timestamps
 *     device_events_<scn>_<stamp>.json  boots[], episodes[], outages[], summary
 *
 * Record format on the module:   B<boot> M<millis-since-boot> E<epoch|0> | <message>
 *   E=0 means the clock wasn't NTP-synced yet at that moment (typical right
 *   after a power cut with the network down). Absolute time is then rebuilt
 *   from the first synced line of the same boot (offset = E*1000 - M); if a
 *   boot never synced, its time is chained after the previous boot and flagged.
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parse, OutageTracker, EpisodeTracker } from "./firmware_rules.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));

// ---- args ----
const A = process.argv.slice(2);
const flag = k => A.includes(k);
const arg = (k, d) => { const i = A.indexOf(k); return i >= 0 && A[i + 1] && !A[i + 1].startsWith("--") ? A[i + 1] : d; };
const LOGS = path.resolve(arg("--outdir", path.join(HERE, "..", "..", "logs")));
fs.mkdirSync(LOGS, { recursive: true });
const SCN = (arg("--scn", "X") || "X").toUpperCase();
const stamp = new Date().toISOString().replace(/[:T]/g, "-").slice(0, 19);

// ============================ acquisition ============================

async function downloadFromSerial({ port, baud, timeoutS, clear }) {
  let SerialPort;
  try { ({ SerialPort } = await import("serialport")); }
  catch { throw new Error("Falta 'serialport'. Instala:  npm i serialport"); }

  return new Promise((resolve, reject) => {
    const meta = { begin: null, end: null, err: null, files: [] };
    const lines = [];
    let state = "wait";               // wait -> dumping -> done
    let buf = "", lastRx = Date.now(), cleared = false;
    const deadline = Date.now() + timeoutS * 1000;

    const p = new SerialPort({ path: port, baudRate: baud }, e => { if (e) reject(new Error(`No pude abrir ${port}: ${e.message}`)); });

    const done = (complete) => {
      clearInterval(tick);
      if (state === "done") return;
      state = "done";
      setTimeout(() => p.isOpen ? p.close(() => resolve({ lines, complete, meta, cleared })) : resolve({ lines, complete, meta, cleared }), 200);
    };

    p.on("data", chunk => {
      buf += chunk.toString("utf8");
      let i;
      while ((i = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, i).replace(/\r$/, ""); buf = buf.slice(i + 1);
        if (line.startsWith("LOGDUMP_BEGIN")) { state = "dumping"; meta.begin = line; lastRx = Date.now(); process.stdout.write("\nvolcado en curso"); }
        else if (line.startsWith("LOGDUMP_FILE")) { meta.files.push(line); }
        else if (line.startsWith("LOGDUMP_ERR")) { meta.err = line; done(false); }
        else if (line.startsWith("LOGDUMP_END")) {
          meta.end = line;
          if (clear && lines.length) { p.write("LOG CLEAR\n"); state = "clearing"; lastRx = Date.now(); }
          else done(true);
        }
        else if (line.startsWith("LOGCLEAR_OK")) { cleared = true; done(true); }
        else if (state === "dumping" && line.startsWith("LD|")) {
          lines.push(line.slice(3)); lastRx = Date.now();
          if (lines.length % 50 === 0) process.stdout.write(".");
        }
      }
    });
    p.on("error", e => { console.error("\nserial error:", e.message); done(false); });

    // The module only answers once it's running. Re-send until the dump starts
    // (also covers the board resetting when the port is opened).
    setTimeout(() => { if (state === "wait") p.write("LOG DUMP\n"); }, 1500);
    const tick = setInterval(() => {
      if (state === "wait" && (Date.now() - lastRx) > 2500) { p.write("LOG DUMP\n"); lastRx = Date.now(); process.stdout.write("."); }
      if (Date.now() > deadline) { console.error("\nTimeout — resultado parcial."); done(false); }
      if (state === "dumping" && Date.now() - lastRx > 15000) { console.error("\nSin datos por 15 s — resultado parcial."); done(false); }
      if (state === "clearing" && Date.now() - lastRx > 5000) done(true);
    }, 500);
  });
}

function loadFromFiles(files) {
  const lines = [];
  for (const f of files) {
    for (let l of fs.readFileSync(f, "utf8").split(/\r?\n/)) {
      if (l.startsWith("LD|")) l = l.slice(3);
      if (l) lines.push(l);
    }
  }
  return lines;
}

// ============================ analysis ============================

const REC = /^B(\d+) M(\d+) E(\d+) \| (.*)$/;
const median = a => { const s = [...a].sort((x, y) => x - y); const m = s.length >> 1; return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2; };

function analyze(rawLines) {
  const records = []; let malformed = 0;
  for (const l of rawLines) {
    const m = REC.exec(l);
    if (!m) { malformed++; continue; }
    records.push({ boot: +m[1], ms: +m[2], epoch: +m[3], text: m[4] });
  }
  // Stable chronological order (files may be given in any order).
  records.sort((a, b) => a.boot - b.boot || a.ms - b.ms);

  // --- reconstruct absolute time per boot ---
  const bootIds = [...new Set(records.map(r => r.boot))];
  const offsetOf = new Map(), estimated = new Map();
  let prevEnd = null;
  for (const b of bootIds) {
    const rs = records.filter(r => r.boot === b);
    const offs = rs.filter(r => r.epoch > 0).map(r => r.epoch * 1000 - r.ms);
    let off, est = false;
    if (offs.length) off = median(offs);
    else { est = true; off = (prevEnd !== null ? prevEnd + 1000 : 0) - rs[0].ms; }   // chain after previous boot
    offsetOf.set(b, off); estimated.set(b, est);
    prevEnd = Math.max(...rs.map(r => off + r.ms));
  }
  const timeline = records.map(r => {
    const t = offsetOf.get(r.boot) + r.ms;
    const est = estimated.get(r.boot);
    return { boot: r.boot, ms: r.ms, epoch: r.epoch, t, iso: est && offsetOf.get(r.boot) < 1e12 ? null : new Date(t).toISOString(),
             iso_estimated: est || r.epoch === 0, line: r.text };
  });

  // --- events + trackers ---
  const outageTr = new OutageTracker(), epTr = new EpisodeTracker();
  const boots = new Map();
  const B = b => { if (!boots.has(b)) boots.set(b, { boot: b, first_iso: null, last_iso: null, time_reliable: !estimated.get(b),
      fw: null, build: null, reset_reason: null, reset_code: null, pending_at_boot: null, fs: null,
      scans: 0, upload_ok: 0, upload_fail: 0, offline_appends: 0, lookups_not_found: 0, invalid_frames: 0,
      cache_aborted: 0, sync_ok: 0, _t0: null, _t1: null }); return boots.get(b); };
  const events = [];
  for (const rec of timeline) {
    const ev = parse(rec.line);
    const bs = B(rec.boot);
    if (bs._t0 === null) bs._t0 = rec.t; bs._t1 = rec.t;
    if (!ev) continue;
    events.push({ boot: rec.boot, t: rec.t, iso: rec.iso, ...ev });
    outageTr.onEvent(ev, rec.t); epTr.onEvent(ev, rec.t);
    switch (ev.type) {
      case "BOOT": bs.fw = ev.fw; break;
      case "BOOT_INFO": bs.build = ev.build; break;
      case "RESET_REASON": bs.reset_reason = ev.reason; bs.reset_code = ev.code; break;
      case "BOOT_PENDING": bs.pending_at_boot = { bytes: ev.bytes, lines: ev.lines, valid: ev.valid, tailBytes: ev.tailBytes }; break;
      case "BOOT_FS": bs.fs = { used: ev.used, total: ev.total }; break;
      case "SCAN": bs.scans++; break;
      case "UPLOAD_OK": bs.upload_ok++; break;
      case "UPLOAD_FAIL": case "UPLOAD_NOWIFI": bs.upload_fail++; break;
      case "OFFLINE_APPEND": bs.offline_appends++; break;
      case "LOOKUP_NOT_FOUND": bs.lookups_not_found++; break;
      case "SCAN_INVALID": bs.invalid_frames++; break;
      case "CACHE_PULL_ABORTED": bs.cache_aborted++; break;
      case "SYNC_OK": bs.sync_ok++; break;
    }
  }
  const bootList = [...boots.values()].sort((a, b) => a.boot - b.boot);
  let prev = null;
  for (const b of bootList) {
    b.first_iso = b._t0 !== null && b._t0 > 1e12 ? new Date(b._t0).toISOString() : null;
    b.last_iso  = b._t1 !== null && b._t1 > 1e12 ? new Date(b._t1).toISOString() : null;
    // Time from the previous boot's LAST logged line to this boot's first one:
    // an upper bound on how long the module was down / resetting.
    b.gap_since_prev_boot_s = prev && prev.t1 !== null && b._t0 !== null ? +((b._t0 - prev.t1) / 1000).toFixed(1) : null;
    prev = { t1: b._t1 };            // keep the end instant BEFORE dropping the scratch fields
    delete b._t0; delete b._t1;
  }

  const summary = {
    records: records.length, malformed_lines: malformed,
    boots: bootList.length,
    power_on_boots: bootList.filter(b => (b.reset_code === 1)).length,
    other_resets: bootList.filter(b => b.reset_code !== null && b.reset_code !== 1).map(b => ({ boot: b.boot, reason: b.reset_reason })),
    truncated_tail_events: bootList.filter(b => b.pending_at_boot && b.pending_at_boot.tailBytes > 0).map(b => ({ boot: b.boot, tailBytes: b.pending_at_boot.tailBytes })),
    invalid_pending_lines: bootList.filter(b => b.pending_at_boot && b.pending_at_boot.lines > b.pending_at_boot.valid)
                                   .map(b => ({ boot: b.boot, lines: b.pending_at_boot.lines, valid: b.pending_at_boot.valid })),
    offline_appended_total: bootList.reduce((s, b) => s + b.offline_appends, 0),
    episodes: epTr.episodes.length,
    lost_estimate_total: epTr.episodes.reduce((s, e) => s + e.lost_estimate, 0),
    boots_with_estimated_time: bootList.filter(b => !b.time_reliable).map(b => b.boot),
  };
  return { timeline, events, boots: bootList, episodes: epTr.episodes, outages: outageTr.outages, summary };
}

// ============================ main ============================

(async () => {
  let rawLines, source, complete = true, meta = null, cleared = false;

  if (flag("--analyze")) {
    const i = A.indexOf("--analyze");
    const files = [];
    for (let k = i + 1; k < A.length && !A[k].startsWith("--"); k++) files.push(A[k]);   // stop at the next flag
    if (!files.length) { console.error("Uso: --analyze <archivo.log> [archivo2.log]"); process.exit(1); }
    rawLines = loadFromFiles(files); source = { mode: "file", files };
  } else {
    const port = arg("--port");
    if (!port) { console.error("Indica el puerto:  --port COM4   (o --analyze <archivo> para un log ya descargado)"); process.exit(1); }
    console.log(`Pidiendo el log al módulo en ${port} …  (cierra cualquier otro monitor serie)`);
    const r = await downloadFromSerial({ port, baud: parseInt(arg("--baud", "115200"), 10),
      timeoutS: parseInt(arg("--timeout", "120"), 10), clear: flag("--clear") });
    rawLines = r.lines; complete = r.complete; meta = r.meta; cleared = r.cleared;
    source = { mode: "serial", port };
    console.log(`\nrecibidas ${rawLines.length} líneas — ${complete ? "volcado COMPLETO" : "volcado PARCIAL"}${cleared ? " — log del módulo borrado" : ""}`);
    if (meta.err) console.error("El módulo respondió:", meta.err);
  }
  if (!rawLines.length) { console.error("No hay líneas de log. ¿Firmware con evlog flasheado? ¿Puerto correcto?"); process.exit(1); }

  const base = path.join(LOGS, `device_log_${SCN}_${stamp}`);
  fs.writeFileSync(base + ".log", rawLines.join("\n") + "\n");

  const res = analyze(rawLines);
  fs.writeFileSync(base + ".ndjson", res.timeline.map(r => JSON.stringify(r)).join("\n") + "\n");
  const evPath = path.join(LOGS, `device_events_${SCN}_${stamp}.json`);
  fs.writeFileSync(evPath, JSON.stringify({
    meta: { tool: "dump_device_log", scenario: SCN, generatedAt: new Date().toISOString(), source, complete, dump_meta: meta,
            note: "Timestamps rebuilt from E (NTP epoch) per boot; boots listed in summary.boots_with_estimated_time were chained after the previous boot." },
    summary: res.summary, boots: res.boots, episodes: res.episodes, outages: res.outages, events: res.events,
  }, null, 2));

  console.log(`\n── boots (${res.boots.length}) ──`);
  console.table(res.boots.map(b => ({
    boot: b.boot, inicio: b.first_iso ? b.first_iso.slice(11, 19) : "(sin reloj)", reset: b.reset_reason, gap_s: b.gap_since_prev_boot_s,
    pend_al_arrancar: b.pending_at_boot ? `${b.pending_at_boot.valid}/${b.pending_at_boot.lines}` : "-",
    cola_truncada: b.pending_at_boot ? b.pending_at_boot.tailBytes : "-",
    scans: b.scans, off: b.offline_appends, ok: b.upload_ok, fail: b.upload_fail, sync: b.sync_ok, no_enc: b.lookups_not_found,
  })));
  if (res.episodes.length) { console.log("── episodios (primer fallo de subida → SYNC_OK) ──"); console.table(res.episodes); }
  console.log("── resumen ──"); console.log(res.summary);
  console.log(`\nGuardado:\n  ${base}.log\n  ${base}.ndjson\n  ${evPath}`);
})().catch(e => { console.error("ERROR:", e.message); process.exit(1); });
