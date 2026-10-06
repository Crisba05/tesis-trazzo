// src/network/captive_portal.cpp
// Captive portal — AP mode with async web server for WiFi provisioning.
// Saves SSID, password, backend URL and tenant subdomain to NVS.

#include "network/captive_portal.h"
#include "network/wifi_manager.h"
#include "config.h"
#include "drivers/display_oled.h"
#include "drivers/display_tft.h"
#include "drivers/display_lcd.h"
#include "tasks/tasks.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <nvs.h>

namespace captive_portal {

    static AsyncWebServer* server = nullptr;
    static DNSServer*      dns    = nullptr;
    static bool            active = false;
    static bool            saved  = false;

    static const char* AP_PASS = "trazzo2026";

    // Minimal HTML form — no external resources, works offline.
    // Backend URL is fixed (never changes) so it's a hidden field; the WiFi
    // password field has a toggle button to show/hide the value.
    static const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TRAZZO IoT Setup</title>
<style>
body{font-family:sans-serif;max-width:440px;margin:32px auto;padding:0 16px;background:#f5f5f5;color:#111}
h1{color:#1a73e8;font-size:1.4em;margin-bottom:4px}
.sub{color:#555;font-size:0.9em;margin-top:0}
label{display:block;margin-top:14px;font-weight:600;font-size:0.9em}
input{width:100%;padding:10px;margin-top:4px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box;font-size:1em;background:#fff}
.pw{position:relative}
.pw input{padding-right:44px}
.pw button.eye{position:absolute;right:6px;top:6px;width:36px;height:36px;padding:0;background:transparent;border:none;color:#555;font-size:1.2em;cursor:pointer;border-radius:6px}
.pw button.eye:hover{background:#eee}
.chip{display:inline-block;background:#eef2ff;color:#3730a3;font-family:monospace;font-size:0.85em;padding:6px 10px;border-radius:6px;margin-top:4px;word-break:break-all}
button.submit{margin-top:22px;width:100%;padding:12px;background:#1a73e8;color:#fff;border:none;border-radius:6px;font-size:1.05em;cursor:pointer;font-weight:500}
button.submit:hover{background:#1557b0}
button.submit:active{background:#0d4894}
.info{margin-top:16px;padding:10px 12px;background:#e8f5e9;border-radius:6px;font-size:0.85em;color:#1b5e20}
.wifiHead{display:flex;align-items:center;justify-content:space-between;margin-top:14px}
.wifiHead .rescan{background:#eef2ff;color:#3730a3;border:none;border-radius:6px;padding:6px 10px;font-size:0.85em;cursor:pointer}
.wifiHead .rescan:hover{background:#e0e7ff}
.wifiHead .rescan:disabled{opacity:0.5;cursor:not-allowed}
.nets{margin-top:6px;border:1px solid #ccc;border-radius:8px;background:#fff;max-height:260px;overflow-y:auto}
.nets .empty{padding:16px;text-align:center;color:#777;font-size:0.9em}
.netRow{display:flex;align-items:center;gap:10px;padding:10px 12px;border-bottom:1px solid #eee;cursor:pointer;transition:background 0.1s}
.netRow:last-child{border-bottom:none}
.netRow:hover{background:#f8faff}
.netRow.sel{background:#e3f0ff}
.netRow .ssid{flex:1;font-weight:500;font-size:0.95em;word-break:break-all}
.netRow .lock{color:#666;font-size:1em;width:16px;text-align:center}
.netRow .lock.open{color:#a0a0a0}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px;width:16px}
.bars span{width:3px;background:#ccc;border-radius:1px}
.bars span.on{background:#1a73e8}
.bars span:nth-child(1){height:4px}
.bars span:nth-child(2){height:7px}
.bars span:nth-child(3){height:10px}
.bars span:nth-child(4){height:14px}
.hint{font-size:0.8em;color:#555;margin-top:4px;min-height:1.1em}
</style>
</head>
<body>
<h1>TRAZZO IoT</h1>
<p class="sub">Configura la conexion WiFi del modulo de asistencia.</p>
<form method="POST" action="/save" id="form">
  <div class="wifiHead">
    <label style="margin:0">Red WiFi</label>
    <button type="button" class="rescan" id="rescan" onclick="scan()">Refrescar</button>
  </div>
  <div class="nets" id="nets"><div class="empty" id="empty">Buscando redes...</div></div>
  <div class="hint" id="scanmsg"></div>
  <input type="hidden" id="ssid" name="ssid" required>
  <div id="manualBox" style="display:none;margin-top:8px">
    <input id="ssidManual" placeholder="Nombre de la red (manual)" maxlength="32" autocomplete="off">
  </div>
  <div style="margin-top:6px">
    <a href="#" onclick="toggleManual(event)" style="font-size:0.85em;color:#1a73e8">
      &raquo; <span id="manualLbl">Escribir SSID manualmente</span>
    </a>
  </div>

  <label>Contrasena WiFi</label>
  <div class="pw">
    <input id="pass" name="pass" type="password" maxlength="64" placeholder="Contrasena" autocomplete="off">
    <button type="button" class="eye" aria-label="Mostrar/ocultar" onclick="tp()">&#128065;</button>
  </div>

  <label>Servidor TRAZZO</label>
  <div class="chip">%BACKEND_URL%</div>
  <input type="hidden" name="url" value="%BACKEND_URL%">

  <button type="submit" class="submit">Guardar y Conectar</button>
</form>
<div class="info">
  El colegio se asigna automaticamente cuando un administrador pareee este modulo
  desde el panel TRAZZO usando el codigo mostrado en la pantalla.
</div>
<div class="info" style="background:#fff3e0;color:#7a4a00">
  Al guardar, el modulo se reiniciara y se conectara a la red indicada.
  Si la conexion falla, presione el boton mas de 5 segundos para reconfigurar.
</div>
<script>
function tp(){var p=document.getElementById('pass');p.type=p.type==='password'?'text':'password';}
function bars(rssi){
  var lvl = rssi>=-55?4 : rssi>=-65?3 : rssi>=-75?2 : rssi>=-85?1 : 0;
  var out='<span class="bars">';
  for(var i=1;i<=4;i++) out+='<span class="'+(i<=lvl?'on':'')+'"></span>';
  return out+'</span>';
}
function pickSsid(ssid){
  document.getElementById('ssid').value=ssid;
  var rows=document.querySelectorAll('.netRow');
  rows.forEach(function(r){r.classList.toggle('sel', r.dataset.ssid===ssid);});
  document.getElementById('pass').focus();
}
function renderNets(list){
  var box=document.getElementById('nets');
  if(!list.length){ box.innerHTML='<div class="empty">Sin redes visibles. Toca refrescar.</div>'; return; }
  list.sort(function(a,b){return b.rssi-a.rssi;});
  // dedupe by ssid (WiFi puede reportar duplicados)
  var seen={}, dedup=[];
  list.forEach(function(n){ if(!n.ssid||seen[n.ssid]) return; seen[n.ssid]=1; dedup.push(n); });
  box.innerHTML=dedup.map(function(net){
    var lock=net.open?'<span class="lock open">&#x1F513;</span>':'<span class="lock">&#x1F512;</span>';
    var safe=(net.ssid||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;');
    return '<div class="netRow" data-ssid="'+safe+'" onclick="pickSsid(this.dataset.ssid)">'
         +   bars(net.rssi)
         +   '<div class="ssid">'+safe+'</div>'
         +   '<span style="color:#888;font-size:0.8em">'+net.rssi+'dBm</span>'
         +   lock
         + '</div>';
  }).join('');
}
function scan(){
  var m=document.getElementById('scanmsg');
  var b=document.getElementById('rescan');
  b.disabled=true; m.textContent='Buscando redes...';
  fetch('/scan').then(function(r){return r.json();}).then(function(list){
    renderNets(list);
    m.textContent=list.length+' red(es) encontrada(s). Toca una para seleccionarla.';
  }).catch(function(){
    document.getElementById('nets').innerHTML='<div class="empty">Error al escanear.</div>';
    m.textContent='';
  }).finally(function(){ b.disabled=false; });
}
function toggleManual(e){
  e.preventDefault();
  var box=document.getElementById('manualBox');
  var lbl=document.getElementById('manualLbl');
  var m=document.getElementById('ssidManual');
  var hidden=document.getElementById('ssid');
  if(box.style.display==='none'){
    box.style.display='block';
    lbl.textContent='Elegir de la lista';
    m.oninput=function(){ hidden.value=m.value; };
  } else {
    box.style.display='none';
    lbl.textContent='Escribir SSID manualmente';
    m.value=''; hidden.value='';
  }
}
window.addEventListener('load',scan);
</script>
</body>
</html>
)rawliteral";

    static String buildApSsid() {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        char ssid[20];
        snprintf(ssid, sizeof(ssid), "TRAZZO-%02X%02X%02X",
                 mac[3], mac[4], mac[5]);
        return String(ssid);
    }

    static void saveToNvs(const char* ssid, const char* pass, const char* url) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_CONFIG, NVS_READWRITE, &h) != ESP_OK) {
            Serial.println("[portal] NVS open failed");
            return;
        }
        nvs_set_str(h, NVS_KEY_WIFI_SSID, ssid);
        nvs_set_str(h, NVS_KEY_WIFI_PASS, pass);
        nvs_set_str(h, NVS_KEY_BACKEND_URL, url);
        // Tenant subdomain is no longer collected in the portal — the backend
        // derives it from the admin's JWT during /pair and hands it back in the
        // poll response. See provisioning.cpp: PAIRED branch persists TENANT.
        nvs_commit(h);
        nvs_close(h);
        Serial.printf("[portal] saved — SSID=%s url=%s\n", ssid, url);
    }

    static String loadNvsStr(const char* key, const char* fallback) {
        nvs_handle_t h;
        char buf[128] = {0};
        if (nvs_open(NVS_NS_CONFIG, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(buf);
            if (nvs_get_str(h, key, buf, &len) == ESP_OK && len > 1) {
                nvs_close(h);
                return String(buf);
            }
            nvs_close(h);
        }
        return String(fallback);
    }

    bool start() {
        if (active) return false;
        saved = false;

        // Freeze the STA side so task_network doesn't fight the AP. Without
        // this, wifi_mgr::poll() calls tryConnect() every 500ms and switches
        // the radio back to STA — the phone drops mid-form.
        wifi_mgr::pause();
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(true, true);   // true, true = disconnect + erase creds cache
        delay(150);

        String apSsid = buildApSsid();
        Serial.printf("[portal] starting AP: %s  pass: %s\n", apSsid.c_str(), AP_PASS);

        WiFi.mode(WIFI_AP);
        WiFi.softAP(apSsid.c_str(), AP_PASS);
        delay(200);
        Serial.printf("[portal] AP IP: %s\n", WiFi.softAPIP().toString().c_str());

        dns = new DNSServer();
        dns->start(53, "*", WiFi.softAPIP());

        server = new AsyncWebServer(80);

        server->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
            String html = String(HTML_PAGE);
            html.replace("%BACKEND_URL%", loadNvsStr(NVS_KEY_BACKEND_URL, TRAZZO_DEFAULT_BACKEND_URL));
            req->send(200, "text/html", html);
        });

        // WiFi scan endpoint — returns JSON array of nearby networks.
        // Called by the portal HTML JS to populate the SSID datalist.
        server->on("/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
            int n = WiFi.scanNetworks(false, true, false, 300);
            String json = "[";
            for (int i = 0; i < n; i++) {
                if (i) json += ",";
                String ssid = WiFi.SSID(i);
                ssid.replace("\\", "\\\\");
                ssid.replace("\"", "\\\"");
                json += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i))
                     +  ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
            }
            json += "]";
            WiFi.scanDelete();
            req->send(200, "application/json", json);
        });

        server->on("/save", HTTP_POST, [](AsyncWebServerRequest* req) {
            String ssid   = req->hasParam("ssid", true)   ? req->getParam("ssid", true)->value()   : "";
            String pass   = req->hasParam("pass", true)    ? req->getParam("pass", true)->value()    : "";
            String url    = req->hasParam("url", true)     ? req->getParam("url", true)->value()     : TRAZZO_DEFAULT_BACKEND_URL;

            if (ssid.length() == 0) {
                req->send(400, "text/plain", "SSID es obligatorio");
                return;
            }

            saveToNvs(ssid.c_str(), pass.c_str(), url.c_str());
            saved = true;

            req->send(200, "text/html",
                "<html><body style='font-family:sans-serif;text-align:center;padding:40px'>"
                "<h2>Guardado!</h2>"
                "<p>El modulo se reiniciara en 3 segundos...</p>"
                "</body></html>");
        });

        // Persistent event log download (evlog). With the portal open, join the
        // module's WiFi and browse to http://192.168.4.1/log (current log) or
        // /log1 (previous, rotated log). Read-only; the log is never modified here.
        server->on("/log", HTTP_GET, [](AsyncWebServerRequest* req) {
            if (LittleFS.exists(FS_PATH_SYSLOG)) req->send(LittleFS, FS_PATH_SYSLOG, "text/plain", true);
            else req->send(404, "text/plain", "no log yet");
        });
        server->on("/log1", HTTP_GET, [](AsyncWebServerRequest* req) {
            if (LittleFS.exists(FS_PATH_SYSLOG_OLD)) req->send(LittleFS, FS_PATH_SYSLOG_OLD, "text/plain", true);
            else req->send(404, "text/plain", "no rotated log");
        });

        // Redirect all unknown URLs to portal (captive portal behavior)
        server->onNotFound([](AsyncWebServerRequest* req) {
            req->redirect("http://" + WiFi.softAPIP().toString());
        });

        server->begin();

        // ESPAsyncWebServer spawns an internal "async_tcp" task that idles
        // waiting for events. It gets auto-registered to the TWDT and, having
        // no work during the portal wait, triggers a panic every ~30s. Remove
        // it from the watchdog so the portal can wait indefinitely.
        {
            TaskHandle_t asyncTcp = xTaskGetHandle("async_tcp");
            if (asyncTcp) {
                esp_err_t r = esp_task_wdt_delete(asyncTcp);
                Serial.printf("[portal] removed async_tcp from TWDT (r=%d)\n", (int)r);
            }
        }
        active = true;

        // Build WIFI QR payload — phones auto-connect when scanned
        char qrPayload[120];
        snprintf(qrPayload, sizeof(qrPayload),
                 "WIFI:T:WPA;S:%s;P:%s;;",
                 apSsid.c_str(), AP_PASS);

        Serial.println("[portal] >>> showing WIFI_SETUP on TFT <<<");
        // Both drivers get the message. On hardware the OLED calls are stubs
        // (TFT owns the display); on Wokwi it's the reverse.
        display_oled::setMode(display_oled::Mode::WIFI_SETUP);
        display_oled::setWifiQr(qrPayload, apSsid.c_str(), AP_PASS);
        display_oled::renderTick();
        display_tft::setMode(display_tft::Mode::WIFI_SETUP);
        display_tft::setWifiQr(qrPayload, apSsid.c_str(), AP_PASS);

        // LCD with text instructions for those without a smartphone camera
        char lcdL1[17], lcdL2[17];
        snprintf(lcdL1, sizeof(lcdL1), "WiFi: TRAZZO");
        snprintf(lcdL2, sizeof(lcdL2), "Clave: %s", AP_PASS);
        display_lcd::setErrorMessage(lcdL1, lcdL2);

        Serial.println("[portal] waiting for user to submit form...");
        Serial.printf("[portal] SSID='%s'  PASS='%s'\n", apSsid.c_str(), AP_PASS);
        Serial.printf("[portal] QR payload: %s\n", qrPayload);

        uint32_t startMs = millis();
        const uint32_t TIMEOUT_MS   = 5 * 60 * 1000;   // 5 minutes
        // Grace window: any gesture received in the first 3s is ignored.
        // The very-long-press that OPENED the portal produces a release
        // event (SHORT) when the user lifts the button, which would
        // otherwise self-cancel the portal immediately.
        const uint32_t GRACE_MS     = 3000;
        uint32_t lastDisplayTick = 0;
        bool cancelled = false;

        while (!saved && !cancelled && (millis() - startMs < TIMEOUT_MS)) {
            dns->processNextRequest();

            // Feed the task watchdog + liveness bit for the logic task
            // (which is blocked here waiting for the portal to close).
            // Without this, task_watchdog reboots after 15s of blocking.
            esp_task_wdt_reset();
            if (tasks::livenessGroup) {
                xEventGroupSetBits(tasks::livenessGroup, tasks::LIVENESS_LOGIC);
            }

            // ── Exit gestures while portal is open ──────────────────────
            //   SHORT_PRESS  → cancel and go back to dashboard.
            //   LONG_PRESS   → same (any deliberate press exits).
            //   VERY_LONG    → also exits (avoids re-entering the portal in
            //                  a loop if the user holds again by mistake).
            //   TRIPLE_HOLD  → factory reset (handled by tasks after we exit).
            if (tasks::qButtonGestures) {
                ButtonGesture g;
                if (xQueueReceive(tasks::qButtonGestures, &g, 0) == pdTRUE) {
                    // Grace window: discard everything for the first 3 s so
                    // the button release from the >5s press that opened the
                    // portal doesn't immediately close it.
                    if (millis() - startMs < GRACE_MS) {
                        Serial.printf("[portal] grace: ignoring gesture %d\n", (int)g);
                    } else if (g == ButtonGesture::SHORT_PRESS ||
                        g == ButtonGesture::LONG_PRESS ||
                        g == ButtonGesture::VERY_LONG_PRESS) {
                        Serial.println("[portal] cancelled by button — returning to dashboard");
                        cancelled = true;
                    } else if (g == ButtonGesture::TRIPLE_AND_HOLD) {
                        // Re-queue so the logic task handles it after we return.
                        xQueueSend(tasks::qButtonGestures, &g, 0);
                        cancelled = true;
                    }
                }
            }

            // Refresh both displays occasionally in case a static mode
            // (WIFI_SETUP) got dirtied but hasn't rendered yet.
            uint32_t now = millis();
            if (now - lastDisplayTick > 500) {
                display_oled::renderTick();
                display_lcd::renderTick();
                lastDisplayTick = now;
            }
            delay(10);
        }

        stop();

        if (saved) {
            Serial.println("[portal] credentials saved — rebooting in 2s");
            delay(2000);
            ESP.restart();
        } else if (cancelled) {
            Serial.println("[portal] user cancelled — resuming previous WiFi");
            wifi_mgr::resume();       // volver a intentar la red configurada
        } else {
            Serial.println("[portal] timeout — no credentials submitted");
            wifi_mgr::resume();
        }

        return saved;
    }

    void stop() {
        if (!active) return;
        if (server) { server->end(); delete server; server = nullptr; }
        if (dns)    { dns->stop();   delete dns;    dns = nullptr; }
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        active = false;
        Serial.println("[portal] stopped");
    }

    bool isActive() { return active; }
}
