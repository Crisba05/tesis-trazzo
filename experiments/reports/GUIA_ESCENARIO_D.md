# Guía de ejecución — Escenario D (ráfaga de captura)

> Runbook de una sesión. Complementa `GUIA_ESCENARIOS.md` §2-Escenario D.
> **Qué mide D:** estabilidad y comportamiento del sistema bajo captura sostenida — deduplicación,
> latencia bajo carga, profundidad de colas, memoria y watchdog. **No es una prueba de integridad de
> datos**: para eso están A, B y C. Léete §1 antes de escanear nada, porque hay tres límites de
> diseño que vas a alcanzar sí o sí y conviene alcanzarlos a propósito, no por sorpresa.

---

## 0. El firmware ya no es el de A y B

Desde el Escenario C el módulo usa *write-ahead*: **cada escaneo se escribe en la flash antes de
intentar subirse**. Para D eso cambia dónde hay que medir.

Antes, un escaneo que no se subía "se guardaba offline". Ahora todos se guardan siempre, y lo que
varía es si además se entregan en vivo o se quedan para el lote. Los puntos de medida son tres y hay
que contarlos por separado:

1. **Emitidos** — líneas `[scan] code=…` (lo que el lector capturó)
2. **Persistidos** — líneas `[offline] appended … pending=N`
3. **Aceptados** — respuestas del backend: `[upl] OK` en vivo, o `[sync] batch sync OK — N accepted`

En condiciones normales, emitidos == persistidos. Que se separen es el hallazgo.

---

## 1. Los tres límites que vas a alcanzar

Ninguno es un fallo: son parámetros de diseño. Pero si no los conoces, vas a leer sus efectos como
errores del sistema.

| Límite | Valor | Dónde | Qué verás al alcanzarlo |
|---|---|---|---|
| Rate limit de escaneo en vivo | **30/min** | `iot-attendance.controller.ts:29` | HTTP 429; el registro se queda en cola para el lote |
| Rate limit de sincronización | **6/min** | `iot-attendance.controller.ts:37` | 429 en el `[sync]`, reintento con backoff |
| Tamaño máximo del lote | **200 registros** | `IotBatchScanEnvelopeDto` | ⚠ ver §2 |
| Ventana de deduplicación | **10 s**, anillo de 20 códigos | `config.h:56`, `dedup.cpp:7` | `[scan] duplicate — ignored` |
| Cola de códigos del lector | 8 | `tasks.cpp:687` | escaneos descartados si el lector va más rápido que la lógica |
| Cola de subida | 16 | `tasks.cpp:689` | `upload queue FULL — persisted; batch sync will pick it up` |

**El rate limit manda sobre el ritmo.** 30 escaneos por minuto es un escaneo cada 2 s. Si escaneas más
rápido —que es justo lo que pide una ráfaga— la mitad va a devolver 429. Eso **no pierde datos** (el
registro ya está en flash y lo recoge el lote), pero significa que una tanda de 250 en vivo es
imposible por diseño: tardaría más de 8 minutos aunque escanearas a velocidad de máquina.

Por eso la tanda de 250 se hace **offline y se sincroniza al final** (§3).

---

## 2. ⚠ Predicción: la tanda de 250 debería fallar

Esto lo predigo leyendo el código, no lo he medido, y por eso vale la pena medirlo.

`readAll()` vuelca **toda** la cola en un solo lote, sin trocear. El DTO acepta como máximo 200
registros. Una cola de 250 produce por tanto un lote de 250 → validación lo rechaza → **HTTP 400** →
y como un 4xx es un error permanente, la capa de cuarentena aparta los 250 registros a
`/offline/rejected.ndjson` y vacía la cola.

Resultado esperado: **los 250 salen de la cola sin llegar a registrarse**. No se pierden del disco
—quedan en el archivo de cuarentena, que para eso existe— pero no se convierten en asistencia.

Tienes dos caminos y los dos son legítimos:

- **Medirlo.** Corres la tanda de 250, confirmas la predicción y la reportas como límite operativo
  documentado. De paso **ejercitas la capa 3, que es lo único de la campaña que nunca se ha
  disparado en hardware**. Es el único escenario que la activa de forma natural.
- **Arreglarlo antes.** Trocear `readAll()` en lotes de 200 son unas pocas líneas, pero cambia el
  firmware otra vez y te obliga a declarar otra build.

Mi recomendación: **mídelo primero**. Un límite predicho por lectura de código y confirmado
experimentalmente vale más en el paper que un límite que nunca existió, y de regalo cierras la capa 3.
Luego lo arreglas y, si quieres, repites solo esa tanda.

---

## 3. Montaje

- **Credenciales: 40 distintas.** No hacen falta 250: con 40 y la ventana de dedup en 10 s puedes
  ciclarlas. A un escaneo cada 2 s, volver a la primera tarda 80 s, muy por encima de los 10 s.
  Numéralas del 1 al 40 y escanea siempre en el mismo orden: si luego falta una, sabes cuál.
- **Alumnos activos.** Verifica antes que las 40 correspondan a alumnos activos; un inactivo
  responde `student_not_found` correctamente y te ensucia el conteo (nos pasó con 9990000058 y
  9990000062).
- **Módulo por USB con captura serial**, que aquí sí puedes mantener: no hay cortes de energía.
  ```bash
  cd E:\Carpetas\Escritorio\TRAZZO\tesis\analisis\scripts\scenarios_capture && node serial_capture.mjs --port COM4 --scn D
  ```
- **Un cronómetro** para marcar el inicio y el fin de cada tanda.

**Ojo con el recuento en base de datos.** La asistencia se hace *upsert* por (tenant, alumno, fecha):
reescanear la misma credencial actualiza la misma fila, no crea otra. Con 40 credenciales y 250
escaneos vas a tener **40 filas, no 250**. Eso es correcto. El conteo de D se hace sobre el log del
dispositivo y las respuestas del backend, no sobre filas.

---

## 4. Procedimiento

### Tanda 1 — 50 escaneos en vivo (con red)

Mide latencia y comportamiento del rate limit.

1. Red estable, cola vacía (confirma `pending=0` en pantalla o reinicia tras un sync).
2. Marca la hora e inicia. Escanea las 40 credenciales en orden, luego repite desde la 1 hasta
   completar 50. **Ritmo constante de un escaneo cada 2 s.**
3. Marca la hora de fin.
4. Deja el módulo conectado 5 minutos más para que el lote recoja lo que haya quedado por 429.

### Tanda 2 — 100 escaneos en vivo

Igual que la anterior pero a **ritmo libre, lo más rápido que puedas leer las credenciales**. Aquí sí
quieres saturar: es donde aparecen las colas llenas y los 429.

### Tanda 3 — 250 escaneos offline

La que prueba el límite del lote (§2).

1. **Corta la red** (apaga el WiFi del router). Confirma en el log que el módulo está offline.
2. Escanea 250 veces ciclando las 40 credenciales, a ritmo cómodo. Verifica de reojo que el contador
   `pending=N` del log sube sin saltos.
3. **Restablece la red** y observa el `[sync]`. Aquí se resuelve la predicción de §2.
4. Si aparece la cuarentena, **no borres nada**: el archivo `rejected.ndjson` es la evidencia.

---

## 5. Qué registrar por tanda

| Dato | De dónde sale |
|---|---|
| Emitidos / persistidos / aceptados | log del dispositivo (§0) |
| Duplicados legítimos (dentro de 10 s) | `[scan] duplicate — ignored` |
| **Duplicados espurios** | códigos distintos marcados como duplicados — sería un fallo real |
| Escaneos no leídos por el lector | tus 250 marcas menos las líneas `[scan]` |
| 429 recibidos | `[upl] fail 429` y `[sync] batch failed: 429` |
| Latencia en vivo | campo `in NNNNms` de `[upl] OK` → p50 y p95 |
| Profundidad máxima de cola | mayor `pending=N` del log |
| Colas llenas | `upload queue FULL` |
| Reinicios por watchdog | `reset reason: TASK_WDT` |
| Memoria | `heap=` en las líneas de arranque y KPIs |

Al terminar, pásame los `.ndjson` de las tres tandas y derivo las tablas, como en A, B y C.

---

## 6. Cómo interpretar lo que salga

**Un 429 no es un fallo.** Es el rate limit haciendo su trabajo, y con write-ahead no cuesta datos:
el registro está en flash y el lote lo recoge. Lo que hay que reportar es **cuántos hubo y cuánto
tardó el sistema en absorberlos**, porque eso sí describe el comportamiento en una fila real de
entrada al colegio.

**Un duplicado dentro de 10 s tampoco es un fallo**, es el dedup. El fallo sería lo contrario: dos
códigos distintos y uno marcado como duplicado, o el mismo código aceptado dos veces con menos de
10 s. Ninguno se ha visto hasta ahora.

**Lo que sí sería un hallazgo:** escaneos emitidos que no llegan a persistirse (con write-ahead no
debería pasar ya), un `TASK_WDT` durante la ráfaga, caída sostenida del heap, o el lector perdiendo
tramas por la cola de 8 códigos.

**El dato más útil para el paper** es la latencia en vivo bajo carga frente a la que ya tienes
medida en reposo (mediana 2.03 s contra el backend en la nube). Si la mediana se mantiene y la p95 no
se dispara, es evidencia directa de que el nodo no se degrada con la fila de entrada, que es el
escenario operativo real.
