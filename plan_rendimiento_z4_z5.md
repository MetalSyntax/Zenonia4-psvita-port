# Plan: exprimir el rendimiento de Zenonia 4 y preparar Zenonia 5

Creado 2026-11-06, tras la release **v1.2.0** (`port_progress.md` Fase 134). Las pruebas empiezan
el 2026-11-07. Cada paso termina en una medicion en consola, no en una impresion.

## Punto de partida (medido, `log_0005.log`)

| Modo | Antes de los 3 nucleos | v1.2.0 |
| :--- | :--- | :--- |
| AUTO (23 ticks/s) | 10-12 frames mostrados/s | **15-20**, caidas a ~12 |
| OFF | 11-14 | **~17** |

- **Costo de un frame dibujado:** ~70ms (antes de los 3 nucleos); el frame saltado cuesta ~14ms.
- **Upload y swap:** upload 1.6ms y swap 0.1ms. Del lado del loader ya no queda nada grande.
- **Donde se va el tiempo:** dentro de `NativeRender`, es decir el motor dibujando por software.
- **Sospecha sobre las caidas a 12, a confirmar en T1:** vendrian de lo que todavia corre en un
  solo nucleo, que son los draw-ops de mezcla, los sprites recortados y los efectos de pantalla
  completa.

## Protocolo de medicion (igual para todas las pruebas)

1. **Build:** con logs (`--pace` o la variante de la prueba). Nunca `--pace-nologs` para medir.
2. **Escena fija** para comparar: la misma zona de combate con muchos enemigos, y una zona tranquila.
3. **Configuracion fija:** botones en pantalla en el mismo estado (anotarlo), FPS 23 y frameskip AUTO.
4. **Duracion:** al menos 5 lineas `[Pace]` seguidas en cada escena (~30s).
5. **Datos que se anotan:** `shown_fps` (minimo, maximo y promedio), `ticks_fps` (debe seguir en ~23),
   y los contadores propios de la prueba.
6. **Revision visual:** cortes en sprites, lineas en las uniones de franjas, capas desordenadas,
   audio cortado.
7. **Criterio de aceptacion:** mejora repetible de **+1 fps o mas** en el promedio de combate, sin
   regresion visual. Si no se cumple, se descarta y se documenta igual en `port_progress.md`.

Logs: `ux0:data/zenonia-4/logs/log_NNNN.log` (`manage_vita.py` opcion 2). `build/eboot.bin` es
siempre la ultima variante compilada.

---

## Bloque A — Pendientes de v1.2.0 (primer dia, sin codigo)

**A1. Sesion larga de 20-30 min con v1.2.0.**
- Que el audio no se corte; el worker del nucleo 1 tiene prioridad 0x10000110, menor que el audio.
- Bateria y temperatura.
- Que no haya cuelgues en los cambios de escenario.

**A2. Probar en consola el menu START+SELECT con la fila nueva de Frame skip.**
- Que se guarde, y que R+SELECT y el menu queden sincronizados.

---

## Bloque B — Normal (poco riesgo)

### T1. Medir que draw-ops dominan las escenas lentas

*Idea 4: es el prerequisito de B/C.*

- **Que hacer:** contador por `op` en `PutCompressImg_hook` (`loader/dynlib.c`, ya recibe `op`),
  y aparte cuantos COPY grandes llegan con recorte. Volcarlo en la linea de 120 frames, igual
  que `par_split`.
  - Solo enteros incrementados, sin logs por llamada: ver la memoria de "no perf impact from logging".
- **Salida:** ranking de ops en combate pesado vs. escena tranquila. Decide el orden de T4.

### T2. Ajustar `PARALLEL_SPRITES`

*Idea 3.*

- **Que hacer:** en `loader/sprite_parallel.c`, `MIN_ROWS` 24 -> 16 -> 12 (y `MIN_AREA` a la
  par). Una variante por valor.
- **Medir:** `shown_fps`, `par_split`, `par_alone`.
- **Esperado:** pequeno. Si sube `par_split` pero no `shown_fps`, la sincronizacion ya cuesta lo
  que ahorra: volver atras.

### T3. Espera de los workers con `WFE`/`SEV` en vez de `yield`

*Idea 3.*

- **Que hacer:** el worker hace `wfe` en lugar de girar; el principal hace `sev` despues de
  publicar el trabajo (`__atomic_store` + `dsb` + `sev`).
- **Medir:** temperatura/bateria (A1) y que `shown_fps` no baje.
- **Riesgo:** confirmar que `wfe`/`sev` estan permitidos en modo usuario en la Vita. Si la app se
  cuelga, volver a `yield`.

---

## Bloque C — Intermedio (riesgo moderado, la mejor relacion esfuerzo/ganancia)

### T4. Repartir en franjas mas draw-ops

*Idea 4.*

- **Mismo metodo que COPY:** franjas por filas, el hilo principal espera, pixeles identicos.
- **Requisito antes de consola:** cada formato se verifica en el host contra el pseudo-C de
  `out_ghidra.c`, como el test de 20000 sprites de la Fase 132.
- **Orden segun T1. Candidatos y sus lineas en `out_ghidra.c`:**
  - `DrawOP_COPY_ClippingCompress_16` (~227200): sprites que tocan el borde. El recorte por fila
    complica el salto inicial.
  - `DrawOP_COPY_Compress_16_Ex` / `_Alpha` / `_Ex_Alpha` (198818 / 199468 / 199071): los
    headers [-7,-3] que hoy van al original.
  - `DrawOP_ADD_Compress_16` (218990), `DrawOP_SUB_Compress_16` (223884),
    `DrawOP_FX_Compress_16` (222094): hechizos y efectos.
  - `LIGHTEN` / `DARKEN` / `GRAY` / `FLIP` (222533 / 219586 / 222244 / 220387).
- **Ojo:** las variantes que leen el destino (mezclas) siguen siendo seguras por franjas, porque
  cada pixel lo toca una sola franja.
- **Riesgo:** si el formato tiene estado entre filas (contadores, alpha acumulado), no se puede
  partir sin replicar ese estado.

### T5. Efectos de pantalla completa en franjas

*Idea 5.*

- **Candidatos:**
  - `FastClearNonClipping`: ya hookeado en `dynlib.c` (`CGsGraphics_FastClearNonClipping_neon_hook`).
  - `Screen_RGBBLEND16_Real` (241507) y `Screen_RGBBLEND16_16` (226752).
  - `CGsScreenEffMgr::DrawUpdate` (32020), `DrawApplyFilter` (32538), `DrawZoom`,
    `DrawRotationZoom`, `ScreenHaze`, y `CMvScreenEffMgr::ScreenZoomBlur` (calculo radial por
    pixel, ~46690).
- **Que hacer:** usar la misma pool de workers de `sprite_parallel.c`, generalizada a "trabajo
  sobre un rango de filas".
- **Riesgo:** los efectos con zoom o rotacion leen de una copia de respaldo
  (`CopyBackupScreen`). Partir por filas de DESTINO es seguro solo si la fuente es otra memoria.
  Confirmarlo funcion por funcion.

### T6. Atajo para sprites 100% opacos

*Idea 6.*

- **Que hacer:**
  - Al primer dibujo de un sprite, detectar si no tiene ningun token transparente.
  - Si es opaco, guardar las filas ya traducidas por la paleta y despues copiarlas con `memcpy`/NEON.
- **Leccion de las Fases 95-100:**
  - La clave del cache debe ser **puntero + paleta + hash del contenido**.
  - Invalidar en los cambios de escenario (`status==19`) y no cachear con `g_ui_status == -1`.
  - Verificar en el host antes de ir a consola.
- **Medir:** tasa de aciertos + `shown_fps` + corrupcion visual.

---

## Bloque D — Ambicioso (riesgo alto)

### T7. Capa de fondo (tiles) cacheada

*Idea 7.*

- **Paso 1:** encontrar en Ghidra la funcion que dibuja la capa de tiles del mapa (buscar a partir
  de `CMvMap`/`CMvLayerData`/`Draw*Layer`) y medir cuanto cuesta, con timestamps solo alrededor
  de esa llamada.
- **Paso 2:** si es grande, guardar el fondo en un buffer propio.
  - Si la camara no se movio, se copia el buffer.
  - Si se movio N pixeles, `memmove` + dibujar solo la franja nueva.
- **Riesgo:** tiles animados (agua, fuego) congelados; efectos que modifican el fondo.

### T8. Hibrido: solo el fondo en la GPU

*Idea 8.*

- **Requisito:** necesita T7 (saber exactamente que es "fondo").
- **Que hacer:** el fondo como una textura grande en vitaGL y los sprites por CPU encima, sobre un
  FB transparente (color-key).
- **Por que es mas seguro que GPU_QUADS:** usa pocas texturas.
- **Riesgo:** el orden de capas si el motor dibuja algo bajo el fondo.

---

## Bloque E — Extremo (proyecto aparte, rama propia)

### T9. Revivir GPU_QUADS con `FB_TEX_RING`

*Idea 9.*

- **Hipotesis nueva:**
  - Los 6 crashes `PC=0x0` (Fases 72-91) fueron dentro de `_glTexSubImage2D` de vitaGL, durante
    la subida del FB.
  - `FB_TEX_RING` (Fase 126) **ya no llama a `glTexSubImage2D`**: escribe directo con
    `vglGetTexDataPointer`.
  - Puede que la ruta del crash ya no exista. No esta probado.
- **Bloqueo hoy:** `FB_TEX_RING` tiene `#error` con `GPU_QUADS`, porque
  `gpu_quads_restore_engine_state` bindea la textura del motor directo. Hay que adaptar ese bind
  al anillo.
- **Reglas duras:**
  - Nunca `DISABLE_VSYNC` + `GPU_QUADS`: colgo la consola entera dos veces. El guard de `build.sh`
    no se toca.
  - Rama aparte. Primero, prueba corta en el menu antes de combate.
  - Si la consola se cuelga, se corta la prueba y se documenta.
- **Bugs conocidos que siguen abiertos:** texturas borradas despues de cambiar de escenario
  (Fase 95) y caminos de dibujo directo no hookeados (capas desordenadas).

### T10. GPU con texturas de indices + paleta en shader

*Idea 10.*

- **Diseno:**
  - Cada sprite decodificado una vez a una textura de 8 bits (indices); la paleta, 256 colores,
    como textura 256x1.
  - Un shader traduce los colores. El mismo sprite con otra paleta reusa la textura, que era el
    bug estructural del cache.
  - ADD/SUB/alpha -> modos de mezcla de la GPU.
- **Punto de intercepcion:** mas arriba que `PutCompressImg`, en `CGxPZxAni::Draw` /
  `CGxPZxBitmap::Blt`, para tener el orden real de dibujo.
- **Esfuerzo:** semanas; es reescribir el backend grafico. Es la unica via a 30+ estables en un
  juego mas pesado.
- **Cuando:** solo si B+C+D no alcanzan para Zenonia 5.

---

## Zenonia 5 — Pista paralela

**Z1. Identificar el motor antes de escribir codigo** (skill `psvita-port-init`):
- `nm -D libgameDSO.so` (o como se llame) buscando `Java_com_gamevil_nexus2_Natives_*`,
  `PutCompressImg`, `SetZeroBlendFunc`, `glDrawFrame`, `setFrameSpeed`, `CZnTouchKeypad`.
- jadx: buscar `SetFPS`/`SetSpeed` para saber a que FPS apunta el juego original.
- **Si dibuja sprites con OpenGL** (no FB por software), el problema es otro: lo que aplica es
  vitaGL y shaders, no los nucleos.

**Z2. Si es el mismo motor, traer desde el dia 1 (de `Zenonia4-vita/loader/`):**
- **Velocidad y frames:** `g_engine_fps` + ritmo fijo + frameskip AUTO (`main.c`, `java.c`).
- **Subida de imagen:** `FB_TEX_RING` (`dynlib.c`).
- **Dibujo:** `sprite_parallel.c` y los draw-ops extra de T4/T5 que ya esten probados.
- **Menu y botones:** `config.c` / `vitamenu.c`, con los botones en pantalla via parche de 2
  bytes; re-verificar la instruccion de entrada con objdump.
- **Logs:** numerados `.log`, la linea `[Pace]` y `eboot.bin` en `build/`.
- **Build:** la receta vitaGL vendorizada de la Fase 124 (`SOFTFP_ABI=1`). Sin ella, pantalla negra.

**Z3. Primera medicion de Zenonia 5:**
- Igual que el protocolo de arriba.
- Comparar el costo del frame dibujado contra los ~70ms de Zenonia 4 para saber cuanto margen hace
  falta y que bloque (C, D o E) es necesario.

---

## Orden sugerido

| Dia | Trabajo |
| :--- | :--- |
| 1 | A1 + A2 + **T1** (medir draw-ops) + T2 |
| 2-3 | **T4** en el orden que diga T1 (primero el op mas caro), con test de host cada uno |
| 4 | **T5** (efectos de pantalla) + T3 |
| 5 | T6 (opacos) — release v1.3.0 si B+C suman |
| Despues | Z1/Z2 con Zenonia 5; T7/T8 si Z5 lo necesita; T9/T10 como rama experimental |

Cada prueba se registra como fase nueva en `port_progress.md`, con sus numeros de `[Pace]`.
