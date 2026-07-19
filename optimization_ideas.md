# Análisis de Optimización: Zenonia 4 (PS Vita)

Basado en el análisis del archivo `port_progress.md` (hasta la Fase 33), el port actualmente alcanza unos **~9 a 11 FPS** usando la subida directa del framebuffer en formato RGB565 y con overclock de la CPU/GPU. Ya se han descartado varias optimizaciones fallidas (bajar resolución interna global, interceptar `glViewport`, etc.).

## Lo que NO se ha hecho (Pendiente según el log actual)

### 1. Parchear el "Hot Path" real de dibujo de sprites (`PutCompressImg`)
En las Fases 31 y 32, el desarrollador encontró el verdadero camino crítico por el que pasan las animaciones y sprites comprimidos (`CGxPZxAni::Draw` -> `CGxPZxZero::Blt` -> `PutCompressImg`). Se instrumentó un build de prueba para contar estas llamadas.
- **Pendiente:** Ejecutar el build `--hotpath-probe` en consola. Si se confirman docenas/cientos de llamadas por frame, el paso definitivo sería **escribir un parche de escalado directamente sobre `PutCompressImg`** (el cual ya tiene un modo `ENLARGE` nativo). Esto permitiría bajar la cantidad real de píxeles renderizados por CPU sin romper la cámara.

> **CONFIRMADO con números reales (Fase 38, `port_progress.md`).** `avg_cgxpzxzero_blt_per_frame`/
> `avg_putcompressimg_per_frame` treparon de 14.7 a **445-547 llamadas por frame** en una sesión de
> combate real (`--all-perf-test`, log completo hasta frame 361+) — muy por encima de los datos
> parciales de la Fase 31/32. Este es ahora el candidato con más potencial real de toda esta lista, listo
> para pasar de "hipótesis" a "escribir el parche de escala". De paso: `avg_cgxpzxani_draw_per_frame`
> (35.5) es ~15x menor que `avg_cgxpzxzero_blt_per_frame` (546.6) — confirma que hay OTROS callers de
> `Blt`/`PutCompressImg` además de `CGxPZxAni::Draw` (probablemente fondo/tiles), todavía sin identificar.

### 2. Implementar renderizado Multihilo (Threading)
Se mencionó como una vía de optimización de alto riesgo. Al ser el cuello de botella actual el renderizado por software (CPU), delegar el cálculo del renderizado del framebuffer (o de ciertas primitivas de dibujo) a otros núcleos de la PS Vita podría suponer un aumento sustancial del framerate, pero requeriría reescritura pesada.

### 3. Evaluar el pipeline general de la clase `CGsGraphics::`
El pipeline de renderizado está repartido en unas ~20 primitivas independientes de la clase `CGsGraphics::` (`DrawLine`, `DrawRect`, `PutPixel`, etc.). Si el parcheo de `PutCompressImg` no es suficiente, habría que atacar estas primitivas una por una para optimizar su código ensamblador (por ejemplo, vectorizándolas más con NEON).

> **Instrumentación implementada y compilada (Fase 36, `port_progress.md`), conteos todavía sin medir.**
> 13 de las ~20 primitivas (las que no reciben structs por valor, ver Fase 36 para el detalle de cuáles se
> dejaron afuera y por qué) confirmadas como símbolos exportados directamente hookeables con `nm -D` —
> a diferencia de `DrawNative`, no hay despacho por vtable de por medio. Contador agregado
> `avg_cgsgraphics_per_frame` detrás de `INSTRUMENT_CGSGRAPHICS_CALLS` (default OFF). Build de prueba:
> `./build.sh --cgsgraphics-probe`.

### 4. Resolver la causa exacta del bug del Wordwrap
Se identificó que el juego calculaba el ajuste de línea letra por letra (`GFA_GetWordwrapPositionEx`), tardando minutos en renderizar cuadros de texto. Se hizo una mitigación ("piso mínimo" de avance), pero la causa real sigue sin resolverse con precisión. Arreglar esto correctamente podría acelerar diálogos largos sin desacomodar el texto.

> **Investigado (Fase 36), sin cambios.** Se revisó `NexusFont.java` decompilado para descartar un mismatch
> de escala EM entre `Paint.setTextSize` y `stbtt_ScaleForMappingEmToPixels` — ya resuelto correctamente en
> una fase anterior (ver comentario en `loader/font.c`). Sin un log real con la línea `[GFA][diag] wordwrap
> sospechoso: ...` (el diagnóstico existente dispara como máximo una vez por sesión), no hay datos nuevos
> para derivar la causa real con confianza — se deja como está hasta conseguir ese log.

---

## 10 Métodos Nuevos para Mejorar la Velocidad (No mapeados/intentados)

Dado que el mayor problema es que el motor original usa renderizado por software en la CPU, aquí tienes 10 ideas avanzadas para eludir este cuello de botella:

### 1. Aceleración 100% por Hardware (GPU Hooking)
En lugar de permitir que la CPU procese los píxeles y dibuje los sprites en el framebuffer de software, intercepta directamente las llamadas del hot-path (`PutCompressImg` o similar). Cada vez que el motor pida dibujar un sprite, el *loader* captura las coordenadas, el puntero de la textura y manda a dibujar un **quad texturizado usando vitaGL directamente en la GPU**. Esto elimina casi todo el estrés de la CPU.

### 2. Actualización de Framebuffer por "Dirty Rectangles"
Actualmente el motor sube el framebuffer entero (~1MB) cada frame. Puedes implementar un sistema donde interceptes las llamadas de dibujo (`SetClipRect`, `PutCompressImg`) para calcular el *bounding box* de las zonas que realmente cambiaron este frame. Luego, usa `glTexSubImage2D` para subir solo esas pequeñas áreas a la GPU, reduciendo dramáticamente el ancho de banda.

> **Medido en consola real (Fase 38): retornos decrecientes justo cuando más importa.** No hacía falta
> interceptar `SetClipRect`/`PutCompressImg` como sugiere la idea original: el wrapper de
> `glTexSubImage2D` (`loader/dynlib.c`) ya recibe el puntero de píxeles completo, así que el diffing
> fila-por-fila contra el frame anterior es autocontenido. `avg_dirtyrect_pct_uploaded` subió de **27.2%
> a 98.0%** en una sesión real (`--all-perf-test`) a medida que la acción de combate se intensificaba — el
> motor termina redibujando casi toda la pantalla en combate sostenido, dejando poco margen de ahorro
> justo cuando más se necesitaría. La idea sigue siendo correcta y segura, pero su beneficio real parece
> marginal en el peor caso. Build de prueba: `./build.sh --dirtyrect-test`.

### 3. Subidas Asíncronas con DMA (Pixel Buffer Objects - PBOs)
Si estás forzado a subir el framebuffer completo cada frame, hazlo sin bloquear la CPU. Usando PBOs de OpenGL, puedes indicarle a la PS Vita que inicie una transferencia DMA de memoria principal a VRAM en segundo plano, permitiendo que la CPU comience inmediatamente a calcular la lógica del *siguiente* frame en paralelo.

> **Descartada por ahora (Fase 36).** vitaGL expone la API de PBO (`glBindBuffer`/`glBufferData`/
> `glMapBuffer`), pero sin su código fuente a mano (no vendorizado en este árbol) no se pudo confirmar que
> `glTexSubImage2D` realmente honre un `GL_PIXEL_UNPACK_BUFFER` en vez de ignorarlo silenciosamente —
> implementar esto arriesgaba un flag que compila y "funciona" sin dar ningún beneficio real, imposible de
> distinguir de una prueba genuina sin el código fuente de vitaGL.

### 4. Reemplazo de funciones `libc` (memcpy/memset) por versiones NEON ultrarrápidas
Los motores antiguos por software dependen masivamente de `memcpy` y `memset` para limpiar el lienzo y blitear píxeles. Si el `.so` utiliza los imports estándar de `libc`, puedes interceptarlos en `dynlib.c` (Global Offset Table) y redirigirlos a implementaciones fuertemente vectorizadas con NEON específicas para la memoria de PS Vita.

> **Medido en consola real (Fase 35, `port_progress.md`): sin mejora de FPS detectable.** Sesión completa
> (8 checkpoints hasta frame 961, misma duración que la referencia de la Fase 33) dio 9.54 FPS promedio
> (rango 7.8-11.4) contra ~9.6 FPS (7.7-11.8) del build normal — dentro del ruido, no una mejora real. El
> usuario sintió la sesión "más estable", lo cual es compatible con este resultado (el log solo mide FPS
> promedio, no varianza de frame-time) pero no está confirmado con datos. Build de prueba:
> `./build.sh --neon-libc-test` / combinado con #9: `./build.sh --neon-novsync-test`.

### 5. Pre-descompresión de Assets en Memoria (Caché decodificado)
Actualmente el *loader* cachea los bytes crudos (Fase 9). Sin embargo, los sprites `.pzx` usan color-key y compresión ("Zero"). Si deconstruyes ese formato, podrías pre-descomprimir los assets a bitmaps crudos (`RGBA8888`) una sola vez en el momento de carga (`readAssets`), para que las rutinas de dibujo solo tengan que hacer copias de memoria en lugar de decodificar píxel por píxel en pleno gameplay.

### 6. Afinidad de Hilos y desbloqueo del 4to Núcleo
La PS Vita tiene 4 núcleos ARM, de los cuales 3 están disponibles para juegos. Asegúrate de forzar la afinidad del hilo principal del juego al núcleo más libre usando `sceKernelChangeThreadCpuAffinityMask`. Asimismo, puedes aislar toda la lógica del *loader* (audio, llamadas al sistema) a otro núcleo distinto para que no interrumpan el bucle principal de renderizado.

> **Implementado y compilado (Fase 36), activo en el build normal (sin flag).** Hilo principal fijado a
> `SCE_KERNEL_CPU_MASK_USER_0` en `gl_init()` (`loader/main.c`), hilo mezclador de audio fijado a
> `SCE_KERNEL_CPU_MASK_USER_1` en `audio_init()` (`loader/audio.c`) — mismo criterio de "optimización
> estándar, sin gate" que el boost de reloj de CPU/GPU/bus ya existente. Confirmado en el log con
> `[PERF] Afinidad de CPU: ...` y `[AUDIO] mezclador iniciado ..., afinidad->nucleo 1 (res=...)`; el valor
> de `res` en consola real confirma si la llamada tuvo éxito (`0`) o falló.

### 7. Traducción Dinámica JIT (Dynarec de rutinas gráficas)
Si logras identificar el código en lenguaje ensamblador de las primitivas más costosas (las de `CGsGraphics::`), podrías usar un mini-traductor JIT en el *loader* que reemplace esos bloques de código ARM estándar del motor Android por instrucciones NEON altamente optimizadas y reescritas en tiempo de ejecución.

### 8. Offloading completo del Audio a un hilo esclavo
Aunque el audio parezca ligero, la decodificación de `.ogg` y su mezclado pueden generar pausas. Escribe un hilo dedicado en `audio.c` que corra en un núcleo distinto, recibiendo comandos a través de un búfer sin bloqueos (lock-free ring buffer). Así, `OnSoundPlay` solo enviará un puntero a la cola y retornará en microsegundos, sin que el motor se detenga.

> **Ya estaba hecho, confirmado al revisar `loader/audio.c` (Fase 34).** `audio_init()` ya lanza
> `audio_thread()` en un thread Vita dedicado que decodifica Tremor y mezcla; `audio_play()` (llamado desde
> `OnSoundPlay`) sólo toma un mutex, copia campos de la voz y retorna — la decodificación real nunca corre
> en el thread principal. No se tocó nada acá, sería trabajo redundante.

### 9. Desactivar el V-Sync para desatascar la lógica
Si vitaGL está forzando un VSync (por defecto `vglSwapBuffers` a veces lo espera), el motor podría estar perdiendo tiempo bloqueado si falla el margen de los 16.6ms o 33.3ms, forzándolo a esperar al siguiente refresco de pantalla. Probar un modo con el VSync totalmente desactivado (permitiendo Tearing temporalmente) te dirá si el límite de FPS actual es artificial por bloqueos de la ventana temporal.

> **Medido en consola real (Fase 35): resultado negativo, y es información real.** 10.00 FPS promedio
> (rango 5.9-12.2) pero en una sesión más corta que la referencia (5 checkpoints, no llegó a los 961 frames
> de la Fase 33) — muestra insuficiente para afirmar mejora. Consistente con lo esperado: a ~90-110ms de
> frametime, `sceDisplayWaitVblankStart()` ya retornaba casi inmediato, así que no había mucho margen para
> ganar saltándose la espera — esto DESCARTA el VSync como techo artificial del FPS, confirma que el cuello
> de botella real es el render por software en CPU. Build de prueba: `./build.sh --no-vsync-test` /
> combinado con #4: `./build.sh --neon-novsync-test`.

### 10. Downsampling temporal entrelazado (Renderizado Checkerboard en CPU)
Si el parcheo de resolución o cámara falla (como vimos en las fases descartadas), puedes hackear el bucle de dibujo de software del `.so` para que, en un frame dibuje solo las filas pares de los sprites, y en el siguiente frame dibuje las filas impares (entrelazado). Luego unes ambos con vitaGL usando un shader básico o texturas. Visualmente reduce la resolución a la mitad en movimiento, pero duplica el framerate de CPU inmediatamente sin romper la matemática espacial del motor.
