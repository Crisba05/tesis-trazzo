# Escenario C — corte de energía. Resultados

Dos corridas válidas, **antes y después** de una corrección de diseño derivada de la primera. El
escenario dejó de ser una comprobación y pasó a ser un ciclo medir → corregir → volver a medir, que
es su resultado más aprovechable.

| | C-1 (línea base) | C-2 (con *write-ahead*) |
|---|---|---|
| Fecha y hora (Lima) | 27-sep-2026, 03:38–03:51 | 27-sep-2026, 04:20–04:26 |
| Build de firmware | `Sep 26 2026 20:22:14` | `Sep 27 2026` (write-ahead) |
| Log crudo | `scenario_C_serial_2026-09-27-08-38-04.ndjson` | `scenario_C_serial_2026-09-27-09-20-50.ndjson` |

Ambas verificadas contra la base de datos del tenant `tesis-demo` por consulta directa de solo lectura.
Las corridas del 22-sep y del 27-sep 00:42 quedan como **pilotos** (§6).

---

## 1. Resultado

| Métrica | C-1 | C-2 |
|---|---|---|
| Cortes de energía | 10 | 8 |
| Arranques | 9 | 8 |
| Fallos de montaje de LittleFS | 0 | 0 |
| Arranques con la cola truncada (`tailBytes>0`) | 0 de 9 | 0 de 8 |
| Escaneos válidos generados | 14 | 10 |
| Escaneos que llegaron a la flash | 8 | **10** |
| **Escaneos perdidos antes de persistir** | **6 (43 %)** | **0 (0 %)** |
| Registros sincronizados al volver la red | 8 de 8 | 10 de 10 |
| Perdidos una vez en la cola | 0 | 0 |
| Asistencias creadas en base de datos | 6 | 8 |

En C-2, los 10 registros encolados corresponden a 9 alumnos distintos (uno se escaneó dos veces) de
los cuales 8 están activos: **8 asistencias creadas, 1 rechazo correcto** por alumno inactivo y una
deduplicación correcta del escaneo repetido. Cero pérdidas en toda la cadena.

---

## 2. La métrica que explica el cambio: latencia de persistencia

Es el tiempo entre clasificar un escaneo y tenerlo escrito en la flash. Mientras no esté escrito, el
evento solo existe en RAM y un corte lo destruye. Medido sobre los logs crudos:

| | n | mínimo | **mediana** | máximo |
|---|---|---|---|---|
| C-1 (subir primero, guardar si falla) | 8 | 4.25 s | **15.59 s** | 298.83 s |
| C-2 (guardar primero) | 10 | 0.14 s | **0.16 s** | 0.33 s |

**La ventana de exposición se redujo unas 100 veces**, de una mediana de 15.6 s a 0.16 s. Ese es el
resultado central del escenario.

El efecto se ve directamente en el log de C-2: cuatro de los ocho cortes cayeron entre 4 y 8 segundos
después de un escaneo — exactamente la franja en la que C-1 perdía eventos — y los cuatro
sobrevivieron.

```
09:23:18  [scan] code=9990000052
09:23:18  [offline] appended  pending=9      <- persistido en el mismo segundo
09:23:22  <corte de energía>
09:23:26  [boot] pending file: bytes=1883 lines=9 valid=9 tailBytes=0
```

---

## 3. El hallazgo de C-1 y su corrección

C-1 mostró que la pérdida **no ocurría al escribir en flash, sino antes de llegar a ella**. El
firmware intentaba subir cada escaneo y solo lo guardaba en la cola *cuando el intento HTTP fallaba*;
sin red ese fallo tardaba entre 4.1 y 19.4 s, y durante toda esa ventana el evento vivía únicamente
en RAM.

Las dos ventanas, medidas:

| Ventana | Duración | Cortes que la alcanzaron |
|---|---|---|
| Escritura del `append` en flash | ~0.15 s | **0 de 30** (toda la campaña) |
| Espera a que expire el intento HTTP (C-1) | 4.1–19.4 s | **6 de 10** |

La corrección es de orden, no de mecanismo: **persistir primero, subir después, marcar como
entregado al confirmar** (*write-ahead*). El registro se escribe en la cola nada más clasificarlo; la
subida en vivo se mantiene para la respuesta inmediata en pantalla, y al aceptarla el backend se anota
el `scanId` en un archivo de marcas para que el lote no lo reenvíe.

Si un corte cae entre la subida exitosa y su marca, el registro se reenvía en el siguiente lote. **No
genera duplicados**: la ingesta es idempotente por `scanId` y la asistencia se hace *upsert* por
(tenant, alumno, fecha). Esa propiedad es la que hace viable el diseño y conviene declararla.

---

## 4. Integridad del almacenamiento

Sumando las cuatro corridas, **30 cortes de energía sin una sola escritura truncada ni corrupción de
LittleFS**: en todos los arranques el reporte de integridad dio `líneas == válidas` y `tailBytes == 0`.

La ventana de escritura del `append` no se alcanzó ni una vez. El enunciado correcto no es "0 % de
pérdida en escritura", sino que esa ventana es demasiado estrecha para alcanzarla incluso forzándola
deliberadamente. Con el write-ahead esa ventana angosta pasa a ser **la única** ventana de pérdida del
sistema.

También se comportó correctamente el rechazo de basura: en C-2 una trama del escáner llegó corrompida
por el corte a mitad de transmisión UART y fue descartada localmente sin escribirse.

---

## 5. Las tres correcciones previas, verificadas

| Capa | Qué hace | Estado |
|---|---|---|
| 1 — reloj persistente en NVS | Restaura la hora al arrancar sin red | **Verificada** (C-1 y C-2) |
| 2 — validación por registro en backend | Un registro malo no invalida el lote | **Verificada** (C-1 y C-2) |
| 3 — cuarentena ante 4xx | Un rechazo permanente no bloquea la cola | **No ejercitada** |

La capa 1 actuó en todos los arranques (`[clock] restored from NVS … (approximate — pending NTP)`) y
eliminó los timestamps vacíos que habían bloqueado la cola en el piloto. La capa 3 no llegó a
dispararse porque no hubo ningún 4xx: **sigue sin probarse en hardware y debe declararse así**.

### Limitación del reloj restaurado

El reloj restaurado arrastra el tiempo apagado y el error **se acumula** entre cortes seguidos. En C-2,
tras ocho cortes en seis minutos el desfase llegó a unos **40 s** (un escaneo de las 04:22:45 quedó
grabado como 04:22:08); en C-1, con diez cortes en doce minutos, alcanzó **2 min 13 s**.

Es el compromiso buscado —una hora aproximada es preferible a ninguna, que el backend rechaza— y los
registros viajan marcados con `ntpUncertain`. Pero para asistencia escolar, donde la frontera entre
PRESENTE y TARDANZA son minutos, hay que declararlo como limitación.

---

## 6. Defectos que la campaña destapó

Los tres se encontraron **por inyección de fallos, no por revisión de código**, y los tres causaban
pérdida silenciosa. Justifican el método y merecen ir en Discusión.

1. **Timestamp vacío tras corte de energía.** Sin RTC con batería, un escaneo posterior al reinicio y
   anterior a la resincronización NTP se encolaba sin hora. El backend lo rechazaba y, como el lote
   viaja íntegro, **arrastraba a los registros válidos**. Bloqueó la cola de forma permanente: 13
   reintentos, 2 reinicios por watchdog, 6 eventos irrecuperables.
2. **Un registro inválido invalidaba el lote entero.** `@ValidateNested` hacía que la validación
   respondiera 400 a toda la petición antes de llegar al servicio, que sí adjudicaba registro por
   registro. El dispositivo reintentaba los mismos bytes indefinidamente.
3. **Pérdida antes de persistir** (§3), corregida con write-ahead y medida en §2.

Hay además una lección metodológica: la primera versión de la corrección 2 respondía 200 y la cola se
vaciaba —parecía correcta— mientras rechazaba los seis registros por un fallo de conversión de tipos.
**Solo se detectó al contrastar la base de datos contra el log del dispositivo.** El criterio "la cola
se vació" habría dado por buena una corrección que borraba datos. Cubierto ahora por
`iot-batch-envelope.spec.ts`.

---

## 7. Qué falta

- **Escenario D** (ráfaga 50/100/250): sin iniciar.
- **Capa 3 (cuarentena)**: sin ejercitar en hardware; requiere provocar un 4xx deliberado.
- **Cortes espaciados**: las dos corridas usan temporización adversaria (8–10 cortes en pocos
  minutos). Sirve para caracterizar el peor caso, no para estimar una tasa de campo (§8).

---

## 8. Nota sobre cómo enunciar estos números

El 43 % de C-1 procede de un régimen **deliberadamente hostil**: diez cortes en doce minutos,
provocados justo después de escanear. No es una tasa esperable en un colegio, donde un apagón ocurre
una vez. Presentado como *tasa de fallo en campo* es indefendible; presentado como *caracterización
del peor caso*, es sólido — y hace que el 0 % de C-2 signifique algo, porque se obtuvo bajo el mismo
régimen.

Conviene además acotar H2 con precisión. Los 82 eventos con 0 pérdidas de los Escenarios A y B miden
**eventos que ya estaban en la cola**; C demuestra que la captura previa a la persistencia era un
punto débil distinto, ahora medido y corregido. Enunciadas así, las dos mitades encajan en lugar de
parecer contradictorias.
