# Índice de logs — Escenarios C y D

Todos en `tesis/analisis/logs/`. Los `.ndjson` son la **evidencia primaria**: cada línea es
`{t, iso, line}` con la línea de consola del módulo sellada con el reloj del PC. Los `.json`
que acompañan a algunos son el resumen que genera `serial_capture.mjs` al cerrar.

---

## Escenario C

| Archivo | Corrida | Estado |
|---|---|---|
| `scenario_C_serial_2026-09-22-*.ndjson` (6 archivos) | 22-sep, protocolo antiguo | **Piloto** |
| `scenario_C_serial_2026-09-27-00-42-50.ndjson` | 27-sep 00:42 UTC | **Piloto** — invalidada por los defectos que destapó |
| `scenario_C_serial_2026-09-27-06-15-27.ndjson` | 27-sep 06:15 UTC | Drenaje de la cola bloqueada (evidencia del defecto de validación) |
| **`scenario_C_serial_2026-09-27-08-38-04.ndjson`** | 27-sep 03:38 Lima | ✅ **C-1, línea base** — 10 cortes, 6 de 14 perdidas |
| **`scenario_C_serial_2026-09-27-09-20-50.ndjson`** | 27-sep 04:20 Lima | ✅ **C-2, con write-ahead** — 8 cortes, 0 perdidas |

**Los dos que van al paper son C-1 y C-2** (`08-38-04` y `09-20-50`). De ahí salen todas las cifras
de `report/RESULTADOS_ESCENARIO_C.md`, incluida la latencia de persistencia (15.59 s → 0.16 s).

Ficheros vacíos, ignorar: `06-15-11`, `06-15-19` (0 bytes).

---

## Escenario D

| Archivo | Tamaño | Estado |
|---|---|---|
| `scenario_D_serial_2026-09-27-09-34-57.ndjson` | 4.8 MB | ❌ **Descartar** — conflicto de puerto |
| `scenario_D_serial_2026-09-27-09-35-02.ndjson` | 727 KB | ❌ **Descartar** — conflicto de puerto |
| `scenario_D_serial_2026-09-27-09-35-33.ndjson` | 1.2 MB | ❌ **Descartar** — conflicto de puerto |
| **`scenario_D_serial_2026-09-27-09-35-46.ndjson`** | 289 KB | ✅ **La corrida** — 394 lecturas, 22 min |
| `scenario_D_serial_2026-09-27-10-06-10.ndjson` | 8 KB | ✅ Evidencia del fallo de memoria TLS (`-32512`) |
| **`scenario_D_serial_2026-09-27-10-12-49.ndjson`** | 7 KB | ✅ **Drenaje de los 146** en seis tandas |

Los tres descartados pesan mucho y contienen basura: varios procesos abrieron COM4 a la vez y
capturaron bytes corruptos. Su `.json` de resumen tiene `total_events: 0`, que es cómo se reconocen.
**Regla:** solo un programa puede tener el puerto abierto a la vez.

---

## Cómo releer cualquiera de ellos

```bash
cd E:\Carpetas\Escritorio\TRAZZO\tesis\analisis\logs && node -e "require('fs').readFileSync(process.argv[1],'utf8').trim().split('\n').forEach(l=>{try{const r=JSON.parse(l);console.log(r.iso.slice(11,19),r.line)}catch(e){}})" scenario_D_serial_2026-09-27-09-35-46.ndjson
```

Cambia el nombre del archivo al final. Para filtrar, añade `| grep "\[sync\]"` o el prefijo que
busques (`[scan]`, `[offline]`, `[upl]`, `[boot]`).

---

## Otros datos de la campaña

- `escenarios_A_B_episodios.csv` — los 13 episodios de A y B, ya derivados.
- `scenario_A_*` y `scenario_B_*` — corridas del 22-sep con sus JSON consolidados por repetición.
- Informes en `tesis/analisis/report/`: `RESULTADOS_ESCENARIOS_A_B.md`,
  `RESULTADOS_ESCENARIO_C.md`, `RESULTADOS_ESCENARIO_D.md` y los borradores de texto.
