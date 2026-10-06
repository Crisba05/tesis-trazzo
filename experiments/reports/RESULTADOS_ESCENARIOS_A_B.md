# Resultados — Escenarios A y B (tolerancia a fallos del nodo IoT)

> **Para:** equipo de redacción del artículo.
> **Campaña:** 2026-09-22. **Escenarios C (corte de energía) y D (ráfaga): pendientes.**
> **Origen de los datos:** capturas serial crudas en `tesis/analisis/logs/*.ndjson`, re-derivadas
> automáticamente con `scripts/scenarios_capture/firmware_rules.mjs` (`EpisodeTracker`).
> Todas las cifras de este documento salen de esa derivación automática, no de lectura manual.

---

## 1. Qué responden estos escenarios

Sustentan **H2** (*«el firmware mantiene una tasa de pérdida de registros de 0 % ante una interrupción de red»*), que hasta ahora se apoyaba en **un solo corte fortuito** de la sesión operativa del 22-jul (14 eventos offline). Esa era la debilidad metodológica más señalable del manuscrito (ver `REVISION_MANUSCRITO.md` §Bloque 2-B).

- **Escenario A** — interrupciones de conectividad de distinta duración (objetivos 60 / 300 / 900 s).
- **Escenario B** — reinicio del microcontrolador **estando en modo offline**, para demostrar que la cola persiste en **flash (LittleFS)** y no en RAM. Sin esta prueba, la afirmación «persistencia en LittleFS» del manuscrito no está demostrada: un búfer en RAM daría el mismo resultado en el Escenario A.

---

## 2. Montaje experimental

| Elemento | Valor |
|---|---|
| Nodo | ESP32 DevKit V1 + GM65, `deviceId = IOT-E6B3F57E`, firmware `2.0.0-dev` |
| Tenant | `tesis-demo` (clon anonimizado: 311 estudiantes, **297 activos**) |
| Backend | `api.example.org` — **en la nube**, no la réplica local co-ubicada |
| Red | AP doméstico `WIFI_LAB`; interrupciones provocadas apagando/reiniciando el router |
| Credenciales | documentNumber anonimizados `9990000001`–`9990000063` (aulas 1°A/1°B del clon) |
| Límites del backend | `POST /iot-attendance/scan` **30/min**; `POST /scan/batch` **6/min** |
| Fecha | 2026-09-22, 18:42–21:00 (hora Lima) |

**Dos compilaciones durante la campaña** (el log lo identifica; declararlo en el paper):

| Build | Corridas | Diferencia |
|---|---|---|
| #1 | A-60s rep1, A-compuesta | base |
| #2 | resto de A y todo B | + corrección del bug de caché (§5.1) + línea de log `[scan] lookup …` |

---

## 3. Definición de la métrica (importante)

El corte **no** se mide como `[wifi] disconnected → [wifi] connected`. Al reiniciar el router, la **asociación Wi-Fi** vuelve en 1–3 s, pero **el internet real (DNS/TLS al backend) tarda minutos**. Medir la asociación subestimaría el corte entre 10× y 100×.

La métrica usada es el **episodio degradado**:

```
inicio  = primer fallo de subida ([upl] fail | [upl] no WiFi/creds)
fin     = [sync] batch sync OK — log cleared
perdidos = eventos encolados durante el episodio − eventos en el lote al sincronizar
```

El episodio **atraviesa reinicios** (la cola vive en flash), que es justamente lo que se necesita para el Escenario B. Implementación: `EpisodeTracker` en `firmware_rules.mjs`; validada reproduciendo las reconstrucciones manuales de las 5 primeras corridas.

---

## 4. Resultados

### 4.1 Escenario A — interrupciones de red

| Objetivo | Rep | Episodio degradado (s) | Offline generados | Recuperados | **Perdidos** | Flaps Wi-Fi |
|---|---|---|---|---|---|---|
| 60 s | 1 | 93.7 | 5 | 5 | **0** | 0 |
| 60 s | 2 | 71.7 | 5 | 5 | **0** | 2 |
| 60 s | 3 | 76.3 | 7 | 7 | **0** | 2 |
| — (compuesta) | — | 183.2 | 9 | 9 | **0** | 1 |
| 300 s | 1 | 379.2 | 5 | 5 | **0** | 2 |
| 300 s | 2 (blip previo) | 5.4 | 1 | 1 | **0** | 0 |
| 300 s | 2 | 351.2 | 5 | 5 | **0** | 2 |
| 300 s | 3 | 415.4 | 5 | 5 | **0** | 2 |
| 900 s | 1 | 963.2 | 5 | 5 | **0** | 2 |
| 900 s | 2 | 903.5 | 5 | 5 | **0** | 2 |
| | **Total** | **rango 5.4–963.2 s** | **52** | **52** | **0** | |

### 4.2 Escenario B — reinicio en modo offline

Procedimiento: red cortada → 10 escaneos offline → **reinicio del nodo sin restablecer la red** → arranque → red restablecida → sincronización.

| Rep | Episodio degradado (s) | Offline antes del reinicio | Pendientes tras el arranque | Recuperados | **Perdidos** |
|---|---|---|---|---|---|
| 1 | 152.0 | 10 | **10** | 10 | **0** |
| 2 | 225.0 | 10 | **10** | 10 | **0** |
| 3 | 97.7 | 10 | **10** | 10 | **0** |
| **Total** | | **30** | **30** | **30** | **0** |

Evidencia del reinicio en el log crudo: salida del bootloader ROM (`ets Jul 29 2019`, `rst:0x1 (POWERON_RESET)`) y reinicialización completa del firmware (`[cache:students] loaded 297 records`, `[tasks] all 6 tasks started`), seguida de `[sync] 10 records pending` → `batch sync OK`.

**Los reinicios fueron por corte de alimentación USB** (`POWERON_RESET`), no por el botón EN. Es una prueba *más* exigente que un reset limpio, pero hay que describirla como tal.

### 4.3 Agregado A + B

| Indicador | Valor |
|---|---|
| Episodios degradados | 13 |
| Eventos encolados offline | **82** |
| Eventos recuperados | **82** |
| **Eventos perdidos** | **0 (0.00 %)** |
| Duración del episodio | 5.4 s – 963.2 s |
| Profundidad máxima de cola | 10 |
| Fallos de montaje de LittleFS | 0 |
| `[offline] FS not ready — drop` (pérdida real) | 0 |

**IC de Wilson 95 % para la tasa de recuperación** (82/82): **[95.52 %, 100 %]**. Conviene reportarlo así en vez de «100 %» a secas, por el mismo criterio que ya aplicamos a la precisión óptica.

### 4.4 Comportamiento del backoff (dato de diseño verificado)

27 reintentos de sincronización fallidos observados. El intervalo escaló de **49.0 s a 300.0 s**, alcanzando el **tope de diseño documentado de 5 minutos**. Esto explica por qué la sincronización no ocurre apenas vuelve el internet: el reintento está deliberadamente espaciado. **No es una demora del sistema, es el mecanismo anti-avalancha funcionando** — conviene decirlo explícitamente en la Discusión, porque un revisor podría leer «963 s para recuperar» como lentitud.

---

## 5. ⚠ Hallazgo que afecta a H1 — leer antes de redactar

**Latencia de subida online contra el backend en la nube** (`[upl] OK … in N ms`, round-trip HTTPS completo desde el dispositivo, n = 50):

| | ms |
|---|---|
| mínimo | 1 812 |
| **mediana** | **2 034** |
| máximo | 2 771 |

**La mediana real contra el backend de producción es ~2.03 s, es decir, ligeramente por encima del umbral de 2 s de H1.** El manuscrito sustenta H1 con **30 ms (p95)**, que provienen del banco controlado sobre **réplica local co-ubicada** (`benchmarks.json`) — una condición distinta, que aísla deliberadamente la latencia de red.

Ambas cifras son correctas y no se contradicen, pero **miden cosas distintas** y no pueden mezclarse en la misma tabla. Recomendación (ya anticipada en `REVISION_MANUSCRITO.md` §Bloque 2-A): definir H1 **una sola vez y de forma explícita**, y presentar las dos cifras por separado:

- *Latencia de procesamiento aplicativo* (réplica co-ubicada): p50 22 ms / p95 30 ms → sustenta H1.
- *Round-trip extremo a extremo en despliegue real* (nube, TLS por petición, WAN): mediana 2.03 s → repórtese como caracterización del despliegue, no como la métrica de H1.

Si no se separan, un revisor puede argumentar que H1 no se cumple en producción. Decirlo nosotros primero, con las dos cifras y su justificación, es mucho más sólido que omitirlo.

---

## 6. Otros hallazgos

### 6.1 Bug de caché encontrado y corregido (validado en campo)
`student_cache::pullFromBackend()` vaciaba `_records` al inicio; si una página del roster fallaba a mitad, abortaba dejando `_records` a medias pero `_index` (índice de búsqueda binaria) describiendo el roster anterior. Resultado: estudiantes válidos resolvían como **«NO REGISTRADO»** de forma aparentemente aleatoria. **Corregido** revirtiendo al último snapshot bueno de LittleFS.

Se validó **en condiciones reales** durante A-300s-rep2: el refresco automático falló por DNS caído y el log muestra la ruta corregida funcionando:

```
[cache:students] pull page failed: -1
[cache:students] aborted due to page error — reverting to last known-good cache
[cache:students] loaded 297 records (MsgPack)
```

Nota importante: **el mensaje local «NO REGISTRADO» nunca implicó pérdida de datos.** El escaneo se sube igual y el backend resuelve la identidad por su cuenta (es la fuente autoritativa). Es un buen párrafo para la sección de trazabilidad.

### 6.2 Un reinicio por watchdog (TASK_WDT)
En la corrida compuesta, el nodo se reinició por `TASK_WDT` **después** de completar la sincronización, mientras recargaba el roster completo (3 páginas de ~24 KB + horarios + config). **No se perdió ningún dato** (los 9 eventos ya estaban sincronizados), pero revela un riesgo latente: una recarga pesada de caché inmediatamente después de vaciar una cola grande puede bloquear una tarea más de 30 s. Reportarlo como limitación honesta; es el tipo de hallazgo que da credibilidad.

### 6.3 Estudiantes inactivos
3 lookups devolvieron `found=0` con `cacheSize=297`: corresponden a alumnos **dados de baja** (p. ej. `9990000062`). El roster solo trae activos, así que es el comportamiento correcto, no un fallo de caché. Útil para explicar la política de bajas en Trazabilidad.

### 6.4 Ruido de log inofensivo
`[E][vfs_api.cpp:105] open(): /littlefs/offline/pending.ndjson does not exist` al arrancar: es el driver VFS reportando que la cola está vacía. El código lo maneja (`if (!f) { _count = 0; … }`). Ignorar.

---

## 7. Limitaciones a declarar en el artículo

1. **Un solo nodo, un solo tenant, una sola red.** No es un despliegue multi-nodo.
2. **La duración del corte se midió, no se impuso.** Los objetivos (60/300/900 s) son nominales; la duración real del episodio va de 71.7 s a 963.2 s. Reportar siempre la medida.
3. **El corte se produjo reiniciando el router**, lo que genera un patrón realista (la Wi-Fi vuelve rápido, el internet tarda) pero no un corte de duración exactamente controlada.
4. **El Escenario B se ejecutó por corte de alimentación USB** (`POWERON_RESET`), no con el botón de reset.
5. **Dos compilaciones de firmware** durante la campaña (§2). Los cambios son de corrección e instrumentación, no del mecanismo de tolerancia a fallos.
6. **Huecos de captura.** En A-60s-rep1 y en las 3 repeticiones de B, la captura por PC se interrumpió al reiniciar el dispositivo; los episodios se reconstruyeron uniendo los archivos consecutivos. (Esto es justamente lo que motivó el log persistente en flash del módulo, ya implementado pero **aún sin probar en hardware** — ver `GUIA_ESCENARIOS.md` §3.bis.)
7. **Una corrida fue descartada** (captura `18-57-35`): se produjo el corte pero no se generaron escaneos durante la ventana, por lo que no aporta evidencia de recuperación. Se excluye de las tablas; el archivo crudo se conserva.

---

## 8. Riesgo abierto (no corregido a propósito)

`offline_log::readAll()` arma el lote concatenando las líneas del NDJSON. Si un corte de energía deja **una línea truncada** (sin su `\n`), el JSON del lote quedaría malformado y el backend podría rechazar **todo el lote** de forma repetida.

**No se corrigió deliberadamente**, para no alterar el sistema que el Escenario C va a medir. El firmware ya instrumentado reporta al arrancar `[boot] pending file: … tailBytes=N`; **si `tailBytes > 0` y la sincronización falla, ese es un hallazgo real** que debe documentarse antes de arreglarlo.

---

## 9. Qué falta

| Escenario | Estado | Qué aportaría |
|---|---|---|
| **C** — corte de energía durante la escritura | 1 repetición piloto hecha (5/5 recuperados, sin corrupción de LittleFS); **faltan ≥4** | Caracteriza el peor caso: corrupción de LittleFS / evento truncado |
| **D** — ráfaga 50/100/250 lecturas | No iniciado | Valida el dedup (buffer 20 / ventana 30 s) bajo fila continua |

Para C y D conviene usar ya el **log persistente en el módulo** (`GUIA_ESCENARIOS.md` §3.bis): evita la fragilidad de la captura por PC ante cortes de energía. **Requiere prueba de humo en hardware antes de fiarse de él.**

---

## 10. Archivos

| Qué | Dónde |
|---|---|
| Capturas crudas (evidencia primaria) | `tesis/analisis/logs/scenario_[AB]_serial_2026-09-22-*.ndjson` |
| Resúmenes por corrida | `tesis/analisis/logs/scenario_[AB]_*_2026-09-22_rep*.json` |
| Tabla por episodio (para figuras/tablas) | `tesis/analisis/logs/escenarios_A_B_episodios.csv` |
| Parser y métricas compartidas | `scripts/scenarios_capture/firmware_rules.mjs` |
| Protocolo de ejecución | `report/GUIA_ESCENARIOS.md` |
| Revisión del manuscrito | `report/REVISION_MANUSCRITO.md` |

**Reproducir las tablas de este documento:** correr `EpisodeTracker` sobre los `.ndjson` de cada sesión (ver §10 de `GUIA_ESCENARIOS.md`). Las sesiones que abarcan dos archivos (A-60s-rep1 y las 3 de B) deben unirse ordenando por timestamp.
