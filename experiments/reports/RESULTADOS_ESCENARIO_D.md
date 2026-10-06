# Escenario D — ráfaga de captura sostenida. Resultados

**Corrida: 27-sep-2026, 04:36–04:58 hora Lima (22 min).** Firmware `2.0.0-dev`, build
`Sep 27 2026 04:11:32` (write-ahead). Fuente: `logs/scenario_D_serial_2026-09-27-09-35-46.ndjson`
(2 442 líneas). Drenaje final verificado en `scenario_D_serial_2026-09-27-10-12-49.ndjson`.
Todo contrastado contra la base de datos de `tesis-demo` por consulta de solo lectura.

> Tres capturas previas del mismo día (`09-34-57`, `09-35-02`, `09-35-33`) quedaron **vacías por
> conflicto de puerto**: varios procesos abrieron COM4 a la vez. Se descartan; solo un programa puede
> tener el puerto.

---

## 1. Balance de escaneos — cuadra al registro

| Concepto | Cantidad |
|---|---|
| Escaneos emitidos por el lector | **394** |
| Duplicados ignorados por el dedup (ventana de 10 s) | 91 |
| Tramas inválidas descartadas localmente | 4 |
| **Escaneos persistidos en flash** | **298** |
| Perdidos | **1** |

394 = 91 + 4 + 298 + 1. El único escaneo perdido ocurrió a las **09:48:22, un segundo antes del
reinicio por PANIC** de §4: cayó entre la clasificación y la escritura. Es una pérdida atribuible al
crash, no al diseño de la cola.

### Entrega de los 298 persistidos

| Vía | Registros |
|---|---|
| Subida en vivo (`[upl] OK`) | 91 |
| Lotes durante la corrida | 61 |
| Drenaje final tras la corrección (§5) | 146 |
| **Total entregado** | **298 (100 %)** |

En base de datos quedaron **280 filas de asistencia IOT del 27-sep, todas PRESENT** (86 en línea,
194 offline), sobre 280 alumnos distintos. Son menos que 298 porque la asistencia hace *upsert* por
(tenant, alumno, fecha): reescanear a un alumno ya registrado actualiza su fila en vez de crear otra.

---

## 2. Rendimiento bajo carga

| Métrica | Valor |
|---|---|
| Latencia de subida en vivo, p50 | **2 278 ms** |
| Latencia de subida en vivo, p95 | 2 675 ms |
| Latencia máxima | 2 958 ms |
| Muestras | 91 |
| Respuestas 429 por límite de tasa | **0** |
| Cola de subida llena (backpressure) | 138 veces |
| Profundidad máxima de la cola offline | 151 registros |

Frente a los **2 034 ms** de mediana medidos en reposo contra el mismo backend en la nube, la
degradación bajo ráfaga es de **~12 %**, sin cola larga en la p95. El nodo no se degrada
apreciablemente con una fila de entrada.

**Cero 429, y es un hallazgo en sí mismo.** El límite del backend es de 30 escaneos/minuto, pero
nunca se alcanzó: el subidor serializa las peticiones y cada una tarda ~2.3 s, de modo que el
dispositivo **se autolimita a ~26/min**, justo por debajo del umbral. El cuello de botella es el
propio nodo, no la política de tasa del servidor.

Las 138 activaciones de backpressure (cola de subida en vivo llena, profundidad 16) **no costaron
ningún dato**: con *write-ahead* el registro ya está en flash y lo recoge el lote. Bajo el diseño
anterior cada una habría sido una pérdida potencial.

---

## 3. Deduplicación

91 duplicados ignorados, **0 espurios**: ningún código distinto fue marcado como repetido y ningún
código se aceptó dos veces dentro de la ventana de 10 s. En 394 lecturas consecutivas el mecanismo
—anillo de 20 entradas, ventana de 10 s— se comportó exactamente como está especificado.

Las 4 tramas inválidas fueron rechazadas por el validador de formato sin llegar a la cola.

---

## 4. Defecto encontrado (1): crash por colisión de descriptores

A las 09:48:23 el módulo se reinició por **PANIC**, no por watchdog:

```
assert failed: lfs_file_close lfs.c:6080 (lfs_mlist_isopen(lfs->mlist, ...))
```

Backtrace decodificado contra el ELF de esa misma compilación:

```
taskUploader → http_client::post → doRequest
  → ~WiFiClientSecure → stop() → esp_vfs_close → vfs_littlefs_close → lfs_file_close → assert
```

**Causa: el orden de declaración de variables locales.** `HTTPClient` se declaraba antes que
`WiFiClientSecure`; como los locales se destruyen en orden inverso, el cliente TLS moría primero y
el destructor de `HTTPClient` llamaba `end()` sobre un objeto ya destruido, cerrando un descriptor ya
cerrado. Para entonces el VFS había reasignado ese número a un archivo de LittleFS, y el cierre cayó
sobre el archivo.

Es un defecto **latente desde el diseño original**, no introducido por esta campaña. Solo se
manifiesta bajo carga, cuando hay suficientes conexiones TLS fallidas y escrituras a flash para que
los números de descriptor colisionen — exactamente lo que este escenario provoca.

**Corregido:** clientes declarados antes que `HTTPClient`, y `http.end()` explícito antes de salir.

*(Hubo además un segundo reinicio, por `TASK_WDT` con la tarea de red bloqueada durante una recarga
del padrón con el heap en ~83 KB. Es el mismo watchdog ya observado en la campaña A/B y sigue sin
corregirse.)*

---

## 5. Defecto encontrado (2): la cola tenía un techo y por encima no drenaba nunca

Al final de la corrida quedaron **146 registros pendientes que no sincronizaban**. La red estaba
perfectamente bien: en los mismos segundos, los heartbeats respondían 200 y las descargas de padrón
de 25 KB respondían 200. **Solo fallaba el lote**, y siempre con `-1`.

El patrón lo delató: **60 registros sincronizaron sin problema; 126 y 146 fallaban invariablemente.**
El log del segundo intento dio la causa exacta:

```
SSL - Memory allocation failed (-32512)   en start_ssl_client()
```

El fallo ocurría **en el handshake TLS, antes de enviar nada**. La cola se volcaba entera en una sola
petición: `readAll()` mantenía el cuerpo completo como String (~31 KB con 146 registros) y el
envoltorio `{"scans":…}` creaba **una segunda copia íntegra**. Con ambas vivas, mbedTLS no encontraba
bloque para su handshake. Por eso una descarga de 25 KB sí funcionaba: es una sola copia de bajada.

**Consecuencia operativa:** por encima de ~100 registros la cola **no drenaba nunca**, y el error
`-1` es indistinguible de un fallo de red. Una mañana escolar con 150 alumnos escaneados sin
conectividad no se habría sincronizado jamás, sin ninguna señal clara de por qué.

**Corregido en dos frentes:**
1. `readBatch()` construye directamente el cuerpo final con `reserve()` — una sola asignación, sin
   copia de envoltorio — y el búfer se libera en cuanto vuelve la respuesta.
2. Envío troceado en tandas de 25 registros (`SYNC_BATCH_MAX_RECORDS`), muy por debajo del tope de
   200 del backend. Si quedan más, el siguiente envío se adelanta a ~12 s.

### Validación sobre la cola real atascada

Los 146 registros seguían en la flash. Tras aplicar la corrección drenaron solos en seis tandas:

```
[sync] sending 25 of 146 record(s), body=5386 B, heap=127268
[http] POST /api/iot-attendance/scan/batch -> 200
[sync] batch sync OK — 25 accepted
[sync] 121 record(s) still queued — next chunk shortly
… (×6) …
[sync] batch sync OK — 21 accepted
[offline] log cleared
```

**146 de 146 recuperados**, cuerpo de ~5 KB por petición y heap estable en ~125 KB durante todo el
proceso. La corrección quedó demostrada sobre datos reales previamente bloqueados, no sobre un caso
de prueba.

---

## 6. Lo que no se llegó a probar

- **El tope de 200 registros por lote** del backend: con el troceado a 25 ya no es alcanzable desde
  el firmware. Queda como límite del contrato, no como riesgo operativo.
- **La capa de cuarentena ante 4xx**: no se disparó, porque todos los fallos fueron `-1` y nunca un
  rechazo permanente. **Sigue sin probarse en hardware** y debe declararse así.

---

## 7. Lectura para el manuscrito

D no era un escenario de integridad de datos —para eso están A, B y C— sino de estabilidad bajo
carga. Aun así aportó los dos defectos más graves de toda la campaña, y ambos comparten una
característica: **solo aparecen bajo carga sostenida y ninguno habría salido en una revisión de
código.** Es el argumento más fuerte a favor del método de inyección de fallos.

Los tres resultados aprovechables:

1. **Rendimiento:** la latencia se degrada solo un 12 % bajo ráfaga y el dedup es exacto en 394
   lecturas. El nodo aguanta una fila de entrada real.
2. **El write-ahead demostró su valor fuera del escenario para el que se diseñó:** las 138
   activaciones de backpressure, que en el diseño anterior eran pérdidas potenciales, no costaron un
   solo registro.
3. **Un modo de fallo silencioso y crítico** (§5): pérdida total de la cola por encima de ~100
   registros, con un síntoma que aparentaba ser un problema de red. Encontrado, explicado,
   corregido y revalidado sobre la cola real.
