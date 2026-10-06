/**
 * firmware_rules.mjs — shared parser for the ESP32 firmware's log lines.
 *
 * Used by BOTH:
 *   - serial_capture.mjs   (live USB-serial capture)
 *   - dump_device_log.mjs  (log downloaded from the module's own flash)
 * so the two sources are analysed with exactly the same rules.
 *
 * `parse(line)` takes the message text (without any timestamp prefix) and
 * returns a structured event or null.
 */

export const RULES = [
  [/\[TRAZZO\] booting fw=(\S+) heap=(\d+)/,         m => ({ type: "BOOT", fw: m[1], heap: +m[2] })],
  [/\[TRAZZO\] reset reason: (.+?) \((\d+)\)/,        m => ({ type: "RESET_REASON", reason: m[1], code: +m[2] })],
  // Written by the on-device log at every boot (evlog::begin):
  [/\[boot\] seq=(\d+) build=(.+)/,                   m => ({ type: "BOOT_INFO", seq: +m[1], build: m[2].trim() })],
  [/\[boot\] pending file: bytes=(\d+) lines=(\d+) valid=(\d+) tailBytes=(\d+)/,
                                                       m => ({ type: "BOOT_PENDING", bytes: +m[1], lines: +m[2], valid: +m[3], tailBytes: +m[4] })],
  [/\[boot\] fs used=(\d+) total=(\d+)/,               m => ({ type: "BOOT_FS", used: +m[1], total: +m[2] })],
  [/LittleFS init failed/,                            () => ({ type: "LFS_FAIL" })],
  [/\[factory\] LittleFS formatted/,                  () => ({ type: "LFS_FORMATTED" })],
  [/\[offline\] appended scanId=(\S+) pending=(\d+)/,  m => ({ type: "OFFLINE_APPEND", scanId: m[1], pending: +m[2] })],
  [/\[offline\] FS not ready — drop/,                 () => ({ type: "OFFLINE_DROP" })],   // <-- real loss
  [/\[offline\] file open failed/,                    () => ({ type: "OFFLINE_OPEN_FAIL" })],
  [/\[offline\] log cleared/,                         () => ({ type: "OFFLINE_CLEARED" })],
  [/\[sync\] (\d+) records pending/,                  m => ({ type: "SYNC_PENDING", pending: +m[1] })],
  [/\[sync\] batch sync OK — log cleared/,            () => ({ type: "SYNC_OK" })],
  [/\[sync\] batch failed: (-?\d+) — backoff (\d+)ms/, m => ({ type: "SYNC_FAIL", status: +m[1], backoff_ms: +m[2] })],
  [/\[wifi\] connected — IP=(\S+) RSSI=(-?\d+)/,      m => ({ type: "WIFI_UP", ip: m[1], rssi: +m[2] })],
  [/\[wifi\] disconnected/,                           () => ({ type: "WIFI_DOWN" })],
  [/\[ntp\] synced/,                                  () => ({ type: "NTP_SYNC" })],
  [/\[http\] (\S+) (\S+) -> (-?\d+) \((\d+) bytes\)/,  m => ({ type: "HTTP", method: m[1], url: m[2], status: +m[3], bytes: +m[4] })],
  [/\[scan\] lookup code=(\S+) found=(\d) cacheSize=(\d+)/,
                                                       m => ({ type: m[2] === "0" ? "LOOKUP_NOT_FOUND" : "LOOKUP_FOUND",
                                                               code: m[1], cacheSize: +m[3] })],
  [/\[scan\] code=(\S+)\s*$/,                          m => ({ type: "SCAN", code: m[1] })],
  [/\[scan\] INVALID doc number '(.*)' — dropped locally/, m => ({ type: "SCAN_INVALID", raw: m[1] })],
  [/\[scan\] duplicate — ignored/,                    () => ({ type: "SCAN_DUPLICATE" })],
  [/\[upl\] OK (\S+) in (\d+)ms/,                      m => ({ type: "UPLOAD_OK", scanId: m[1], ms: +m[2] })],
  [/\[upl\] fail (-?\d+) dur=(\d+)ms — saving offline/, m => ({ type: "UPLOAD_FAIL", status: +m[1], ms: +m[2] })],
  [/\[upl\] no WiFi\/creds — offline (\S+)/,           m => ({ type: "UPLOAD_NOWIFI", scanId: m[1] })],
  [/\[cache:students\] aborted due to page error/,    () => ({ type: "CACHE_PULL_ABORTED" })],
  [/\[cache:students\] loaded (\d+) records/,          m => ({ type: "CACHE_LOADED", records: +m[1] })],
];

export function parse(line) {
  for (const [re, fn] of RULES) { const m = line.match(re); if (m) return fn(m); }
  return null;
}

/**
 * Degraded-connectivity EPISODES — the metric we were reconstructing by hand.
 *
 * WIFI_DOWN/WIFI_UP only flag the Wi-Fi *association*; a router reboot flaps that
 * within seconds while the real internet stays down for minutes. What matters
 * for "no data lost" is the span from the FIRST failed upload (event queued
 * offline) to the SYNC_OK that drains the queue. An episode survives reboots /
 * power cuts in between (the queue is in flash), which is exactly Scenarios B/C.
 *
 *   lost_estimate = offline events appended during the episode that were NOT
 *                   in the batch at SYNC_OK  (>=0; 0 = nothing lost)
 */
export class EpisodeTracker {
  constructor() { this.episodes = []; this._reset(); }
  _reset() { this.start = null; this.appended = 0; this.upFails = 0; this.lastPending = null; this.wifiDowns = 0; this.reboots = 0; }
  onEvent(ev, t) {
    switch (ev.type) {
      case "UPLOAD_FAIL": case "UPLOAD_NOWIFI":
        if (this.start === null) this.start = t;
        this.upFails++; return null;
      case "OFFLINE_APPEND":
        if (this.start === null) this.start = t;
        this.appended++; return null;
      case "WIFI_DOWN": if (this.start !== null) this.wifiDowns++; return null;
      case "BOOT":      if (this.start !== null) this.reboots++;   return null;
      case "SYNC_PENDING": this.lastPending = ev.pending; return null;
      case "SYNC_OK": {
        if (this.start === null) return null;
        const recovered = this.lastPending ?? 0;
        const ep = {
          start: new Date(this.start).toISOString(),
          end: new Date(t).toISOString(),
          degraded_s: +((t - this.start) / 1000).toFixed(1),
          offline_appended: this.appended,
          recovered_at_sync: recovered,
          lost_estimate: Math.max(0, this.appended - recovered),
          upload_failures: this.upFails,
          wifi_flaps: this.wifiDowns,
          reboots_during: this.reboots,
        };
        this.episodes.push(ep);
        this._reset();
        return ep;
      }
      default: return null;
    }
  }
}

/**
 * Derives per-outage metrics from the event stream: WIFI_DOWN -> WIFI_UP -> SYNC_OK.
 * `t` are epoch milliseconds. Feed every event via onEvent(); it returns the
 * closed outage object when a SYNC_OK completes one, otherwise null.
 *
 * NOTE: plain numbers can't carry extra properties, so down/up instants live in
 * separate fields (an earlier version hung `._up` off a number and crashed).
 */
export class OutageTracker {
  constructor() { this.outages = []; this._reset(); }
  _reset() { this.start = null; this.up = null; this.appended = 0; this.maxPending = 0; this.drops = 0; }
  onEvent(ev, t) {
    if (ev.type === "WIFI_DOWN") { this._reset(); this.start = t; }
    else if (ev.type === "OFFLINE_APPEND" && this.start) { this.appended++; this.maxPending = Math.max(this.maxPending, ev.pending); }
    else if (ev.type === "OFFLINE_DROP") { this.drops++; }
    else if (ev.type === "WIFI_UP" && this.start) { this.up = t; }
    else if (ev.type === "SYNC_OK" && this.start && this.up) {
      const o = {
        outage_start: new Date(this.start).toISOString(),
        outage_s: +((this.up - this.start) / 1000).toFixed(1),
        sync_latency_s: +((t - this.up) / 1000).toFixed(2),
        offline_appended: this.appended, max_queue_depth: this.maxPending, drops: this.drops,
      };
      this.outages.push(o);
      this._reset();
      return o;
    }
    return null;
  }
}
