# Guía de ejecución — Escenario C (corte de energía)

> Runbook de una sola sesión. Complementa `GUIA_ESCENARIOS.md` §2-Escenario C y §3.bis.
> **Regla de oro de este escenario: aquí NO se busca 0 % de pérdida.** El resultado creíble es
> el número medido, sea cual sea. Un "2 de 13 cortes perdieron el evento en vuelo" vale más
> ante un revisor que un 100 % que nadie se cree.

---

## 0. Lo que ya tienes y lo que falta

La rep. piloto del 22-sep (`scenario_C_powercut_2026-09-22_rep1.json`) dio: 5 eventos encolados,
**5 recuperados, 0 perdidos**, LittleFS montó OK en los 4 arranques, y una trama GM65 truncada
(4 de 10 bytes) correctamente rechazada por el validador. Verificado contra la BD.

Esa corrida cortó **antes** de que el evento llegara a escribirse (se truncó en el cable UART, no
en la flash). Es un caso válido, pero no es el peor caso. Falta cubrir los otros dos.

---

## 1. Decisión previa: ¿flasheo el firmware con log interno?

**Recomendación: sí, flashea.** Razón: sin el reporte `[boot] pending file: … tailBytes=N` no
tienes **ningún instrumento** para detectar que un corte partió una línea a medias — la cola
`pending.ndjson` no se puede descargar por el portal, y el USB se cae justo cuando cortas la luz.
Sin ese dato, el Escenario C se queda en "no pasó nada malo, creo".

Lo que cuesta: el firmware de C será una **build distinta** a la de A y B (cambios solo de
instrumentación). Hay que declararlo en Métodos, junto con el *observer effect*: el log vive en la
misma flash que la cola, así que un corte puede caer en una escritura del log en vez de la de la
cola. `offline_log::verify()` solo lee, no repara nada.

```bash
cd E:\Carpetas\Escritorio\TRAZZO\modulo_iot && pio run -e esp32-dev -t upload --upload-port COM4
```

### 1.1 Prueba de humo — OBLIGATORIA antes de la campaña

El log interno está **compilado pero nunca ejecutado en hardware**. No arranques la campaña sin
esto (si falla, corres 13 cortes y te quedas sin evidencia):

```bash
cd E:\Carpetas\Escritorio\TRAZZO\tesis\analisis\scripts\scenarios_capture && node dump_device_log.mjs --port COM4 --scn TEST
```

Debe imprimir la tabla `boots` con tu arranque y la columna `pend_al_arrancar`. Luego escanea
2 credenciales, repite el comando, y confirma que aparecen `[scan]` y `[upl] OK`.
**Si esto no funciona, avísame antes de seguir** — se corre C con captura por serial y se acepta
perder la ventana del corte.

Con la prueba OK, limpia el log para que el archivo contenga solo la campaña:

```bash
cd E:\Carpetas\Escritorio\TRAZZO\tesis\analisis\scripts\scenarios_capture && node dump_device_log.mjs --port COM4 --scn TEST --clear
```

---

## 2. Montaje

- Módulo alimentado por **power bank** con un cable que puedas desconectar de golpe (o un
  interruptor de regleta). Sin PC conectada: el log queda en la flash.
- **Credenciales: 20 distintas**, sin repetir dentro de la misma corrida (el dedup de 30 s te
  contaminaría el conteo). Anota cuáles usas en cada corte — es lo que vas a contrastar con la BD.
- Nodo **offline** antes de cada corte (router apagado o WiFi del AP caído). Si está online el
  evento se sube directo y nunca toca `pending.ndjson`: no mides nada.
- Firmware anotado: `2.0.0-dev` + la línea `[boot] build=<fecha> <hora>` que sale en el log.

---

## 3. Las tres ventanas que hay que cortar

El firmware tiene tres momentos de riesgo distintos, con probabilidades de acierto **muy**
distintas. Cortar "durante la escritura" a mano suena bien en el protocolo pero es casi
inalcanzable; por eso se separan y se reporta cada una por su lado.

| | Qué se ataca | Ventana real | ¿Se puede acertar a mano? |
|---|---|---|---|
| **C1** | El append a `pending.ndjson` | ~10 ms por escaneo | Casi no. Se mide la tasa de acierto. |
| **C2** | Integridad general del FS estando offline | siempre | Sí, siempre |
| **C3** | Entre el POST del lote y el borrado de la cola | ~2 s (mediana del RTT) | **Sí, es la buena** |

**C3 es la ventana valiosa.** El firmware sube el lote y *después* borra la cola
(`modulo_iot/src/logic/heartbeat.cpp:266-268`). Si cortas en medio, el servidor ya guardó pero el
módulo aún tiene la cola → al volver reenvía → el upsert por (tenant, alumno, fecha) del backend
debería absorberlo sin duplicar filas. Eso es exactamente lo que hay que demostrar, y sí se puede
acertar porque el RTT contra la nube es de ~2 s.

---

## 4. Procedimiento por corte

Trece cortes, ~50 min. Usa el logger HTML (`logger_escenarios.html` → Escenario C) o una hoja;
lo importante es que quede escrito **por corte**: nº, clase (C1/C2/C3), offset, códigos en cola,
código en vuelo, y qué dijo el arranque siguiente.

### C1 — corte durante el append (6 cortes)

1. Con 3–5 eventos ya en cola, presenta una credencial nueva.
2. **Corta la alimentación** con el offset que toque. Varía: `0.0, 0.2, 0.5, 0.8, 1.0, 1.5 s`
   después del pitido del escáner. Anota el offset pretendido (aproximado está bien, es una
   distribución, no un punto).
3. Reconecta. Anota del arranque: `reset=POWERON` y sobre todo **`tailBytes`**.
   - `tailBytes=0` → el corte cayó fuera de la ventana. Normal, es el caso esperado.
   - `tailBytes>0` → **acertaste**. Esto es el hallazgo. No repitas ni "arregles" nada: sigue el
     protocolo hasta el sync y **anota qué pasó con el resto de la cola** (ver §6).
4. No restablezcas la red todavía; encadena el siguiente corte.

### C2 — corte con cola cargada (3 cortes)

1. Cola con 5+ eventos, sin escanear nada en ese instante. Corta la alimentación en frío.
2. Reconecta y anota: `reset`, `lines`, `valid`, `tailBytes` del reporte de arranque.
3. Comprueba que `valid` == los eventos que encolaste. Esto es la prueba de persistencia pura.

### C3 — corte durante la subida del lote (4 cortes)

1. Con 5 eventos en cola, **restablece la red** y espera.
2. El disparo es la línea `[sync] N records pending` — pero no la ves sin PC. Dos formas:
   - **Con PC conectada** (recomendado para C3: aquí el corte no mata la captura porque el
     evento de interés ya está en la flash): corre `serial_capture.mjs`, y corta **1 s después**
     de ver `[sync] N records pending`.
   - **Sin PC**: corta a ciegas ~1 s después de que la pantalla indique sincronización.
3. Reconecta, deja que sincronice y luego **verifica en la BD**: lo que importa es si quedaron
   **filas duplicadas** o si el lote se perdió entero.
4. Anota el resultado por corte: `reenviado sin duplicar` / `duplicado` / `perdido`.

---

## 5. Cierre y descarga

```bash
cd E:\Carpetas\Escritorio\TRAZZO\tesis\analisis\scripts\scenarios_capture && node dump_device_log.mjs --port COM4 --scn C
```

Genera en `logs/`: `device_log_C_*.log` (crudo de la flash), `.ndjson` (con tiempos
reconstruidos) y `device_events_C_*.json` (arranques + episodios + resumen).
Si prefieres descargarlo por WiFi: pulsador >5 s → portal → `http://192.168.4.1/log` (y `/log1`)
→ `node dump_device_log.mjs --analyze log.txt log1.txt --scn C`.

⚠ **No hagas reset de fábrica ni despareo antes de descargar**: ambos formatean LittleFS y
borran el log.

Pásame el `.ndjson` + tu hoja de cortes y yo derivo las tablas y verifico contra la BD, igual que
en A y B.

---

## 6. Qué se reporta (y por qué así)

| Métrica | Cómo sale |
|---|---|
| Cortes por clase | tu hoja |
| Aciertos en la ventana de append | nº de arranques con `tailBytes>0` / nº de cortes C1 |
| Montajes de LittleFS OK | nº de arranques sin `LittleFS init failed` |
| Eventos encolados / recuperados / perdidos | log + consulta a la BD |
| Duplicados tras corte en sync | consulta a la BD sobre los cortes C3 |

**Si ningún corte C1 acierta la ventana** (lo más probable), el resultado no es "0 % de pérdida":
es **"la ventana vulnerable no se alcanzó en 6 intentos"**, y se reporta como una cota — la
ventana de exposición es de unos milisegundos por evento frente a un ciclo de segundos. Eso es
una afirmación cuantitativa y defendible; decir "0 % de pérdida ante cortes de energía" a partir
de no haber acertado, no.

**Si algún corte C1 acierta**, hay una consecuencia conocida y esperada que debes mirar sí o sí:
`readAll()` concatena las líneas del archivo, así que una línea partida malforma el JSON del lote
entero y el backend rechazaría **todos** los eventos en cola, no solo el truncado. Ese fallo
**está sin corregir a propósito**, para no alterar el sistema que se está midiendo. Si ocurre, es
un hallazgo real del paper (modo de fallo + magnitud del impacto), y el arreglo va como trabajo
futuro. No lo parchees a mitad de campaña.
