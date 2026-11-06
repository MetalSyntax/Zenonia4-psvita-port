#!/bin/bash
set -e

# Uso: ./build.sh                  -> build normal, build/zenonia_4.vpk
#                                     (RGB565_DIRECT_UPLOAD=ON por default desde
#                                     2026-07-17 -- confirmado en consola real,
#                                     ver port_progress.md Fase 25/26)
#      ./build.sh --neon-fallback   -> build de respaldo con la conversion NEON
#                                     de la Fase 22 en vez de RGB565 directo
#                                     (RGB565_DIRECT_UPLOAD=OFF),
#                                     build/zenonia_4_neonfallback.vpk, mismo
#                                     build tree, no pisa el build normal --
#                                     usar solo si RGB565 directo muestra algun
#                                     problema de color no visto todavia (ver
#                                     CMakeLists.txt RGB565_DIRECT_UPLOAD)
#      ./build.sh --drawnative-probe -> build de diagnostico (Fase 29) con un
#                                     hook de solo-conteo sobre DrawNative()
#                                     (INSTRUMENT_DRAWNATIVE_CALLS=ON),
#                                     build/zenonia_4_drawnative_probe.vpk --
#                                     hipotesis ya descartada (Fase 30), se
#                                     mantiene por si se retoma
#      ./build.sh --hotpath-probe    -> build de diagnostico (Fase 31) con
#                                     hooks de solo-conteo simultaneos sobre
#                                     CGxPZxAni::Draw / CGxPZxZero::Blt /
#                                     PutCompressImg (INSTRUMENT_HOTPATH_CALLS=ON),
#                                     build/zenonia_4_hotpath_probe.vpk
#                                     -- el FPS medido con estos builds de
#                                     diagnostico NO es comparable al normal
#                                     (los hooks agregan overhead real por
#                                     llamada), solo importan los contadores
#                                     avg_*_calls_per_frame del log (ver
#                                     CMakeLists.txt INSTRUMENT_DRAWNATIVE_CALLS
#                                     / INSTRUMENT_HOTPATH_CALLS)
#      ./build.sh --neon-libc-test   -> build experimental (optimization_ideas.md
#                                     #4) con memcpy/memset redirigidos a NEON
#                                     (NEON_LIBC_INTERCEPT=ON),
#                                     build/zenonia_4_neonlibc_test.vpk --
#                                     TODAVIA SIN MEDIR en consola real, comparar
#                                     FPS contra zenonia_4.vpk antes de considerar
#                                     promoverlo a default (ver CMakeLists.txt
#                                     NEON_LIBC_INTERCEPT)
#      ./build.sh --no-vsync-test    -> build de diagnostico (optimization_ideas.md
#                                     #9) que deshabilita la espera de vblank de
#                                     vglSwapBuffers (DISABLE_VSYNC=ON),
#                                     build/zenonia_4_novsync_test.vpk -- causa
#                                     tearing, solo para descartar si el FPS
#                                     actual esta atado artificialmente al
#                                     refresco de pantalla; requiere que la
#                                     vitaGL vendorizada tenga vglWaitVblankStart
#                                     (si el link falla, esta opcion no es
#                                     usable con esa version de vitaGL)
#      ./build.sh --neon-novsync-test -> combina los dos flags de arriba
#                                     (NEON_LIBC_INTERCEPT=ON + DISABLE_VSYNC=ON),
#                                     build/zenonia_4_neonnovsync_test.vpk --
#                                     dos rutas de codigo independientes (una
#                                     redirige memcpy/memset, la otra salta la
#                                     espera de vblank), sin motivo para que
#                                     interactuen entre si; ver port_progress.md
#                                     Fase 35 para la comparacion de FPS de cada
#                                     flag por separado antes de combinarlos
#      ./build.sh --dirtyrect-test    -> build experimental (optimization_ideas.md
#                                     #2, Fase 36) que compara el framebuffer
#                                     RGB565 contra el frame anterior y sube
#                                     solo el rango de filas que cambio
#                                     (DIRTY_RECT_UPLOAD=ON, requiere
#                                     RGB565_DIRECT_UPLOAD=ON), build/
#                                     zenonia_4_dirtyrect_test.vpk -- ver
#                                     avg_dirtyrect_pct_uploaded del log; si
#                                     sale cerca de 100% en combate, el motor
#                                     redibuja casi toda la pantalla siempre y
#                                     esto no ahorra nada (el memcmp/memcpy de
#                                     diffing pasaria a ser costo puro)
#      ./build.sh --cgsgraphics-probe -> build de diagnostico (optimization_ideas.md,
#                                     "Lo que NO se ha hecho" #3, Fase 36) con
#                                     hooks de solo-conteo sobre ~13 primitivas
#                                     de CGsGraphics:: (DrawLine, DrawFillRect,
#                                     DrawRoundRect, DrawEllipse, ClearScreen,
#                                     PutPixel, etc. -- ver dynlib.c)
#                                     (INSTRUMENT_CGSGRAPHICS_CALLS=ON),
#                                     build/zenonia_4_cgsgraphics_probe.vpk --
#                                     mismo caveat que los otros *_probe: FPS
#                                     no comparable al build normal, solo
#                                     importa avg_cgsgraphics_per_frame del log
#      ./build.sh --all-perf-test    -> (Fase 37) combina 4 flags a la vez a
#                                     pedido del usuario: NEON_LIBC_INTERCEPT +
#                                     DISABLE_VSYNC + DIRTY_RECT_UPLOAD (los 3
#                                     candidatos reales de perf) +
#                                     INSTRUMENT_HOTPATH_CALLS (para seguir
#                                     juntando datos de CGxPZxAni::Draw/
#                                     CGxPZxZero::Blt/PutCompressImg mientras
#                                     tanto), build/zenonia_4_allperf_test.vpk.
#                                     OJO: el FPS que se mida con este build NO
#                                     es comparable al normal -- los hooks de
#                                     conteo de INSTRUMENT_HOTPATH_CALLS
#                                     agregan overhead real por llamada (mismo
#                                     caveat que --hotpath-probe solo). Lo que
#                                     SI es valido de este build: los 3
#                                     contadores avg_cgxpzxani_draw_per_frame/
#                                     avg_cgxpzxzero_blt_per_frame/
#                                     avg_putcompressimg_per_frame (call counts
#                                     reales, no dependen del FPS) y una
#                                     confirmacion cualitativa de que los 3
#                                     flags de perf conviven sin crashear.
#                                     a la vez. Ahora tambien incluye INSTRUMENT_CGSGRAPHICS_CALLS=ON.
#      ./build.sh --fastclear-neon-test -> (Fase 41, Tabla A #3) reemplaza
#                                     CGsGraphics::FastClearNonClipping por una
#                                     reimplementacion fiel (verificada contra
#                                     su pseudo-C real en out_ghidra.c) que
#                                     colapsa el loop de memset por fila en uno
#                                     solo cuando el rectangulo es de ancho
#                                     completo (FASTCLEAR_NEON_OPTIMIZE=ON),
#                                     build/zenonia_4_fastclear_neon_test.vpk
#                                     -- a diferencia de los demas hooks de
#                                     este proyecto, ESTE reemplaza la funcion
#                                     por completo (no hay unhook/llamar
#                                     original/rehook), asi que el FPS medido
#                                     con este build SI es comparable al
#                                     normal. TODAVIA SIN MEDIR en consola real.
#      ./build.sh --interlaced-test   -> (Fase 41, optimization_ideas.md #10,
#                                     reinterpretado) sube solo filas
#                                     pares/impares alternadas del framebuffer
#                                     RGB565 por frame en vez del buffer
#                                     completo (INTERLACED_UPLOAD=ON, requiere
#                                     RGB565_DIRECT_UPLOAD=ON, mutuamente
#                                     excluyente con DIRTY_RECT_UPLOAD),
#                                     build/zenonia_4_interlaced_test.vpk --
#                                     ahorra la mitad de los BYTES subidos por
#                                     diseno (a diferencia de dirty-rect, no
#                                     depende de cuanto cambio realmente), pero
#                                     a costa de height/2 llamadas a
#                                     glTexSubImage2D en vez de 1 sola -- ver
#                                     avg_interlaced_pct_uploaded del log (debe
#                                     rondar 50%) y comparar el FPS resultante
#                                     contra el build normal antes de sacar
#                                     conclusiones (el overhead por-llamada de
#                                     esta vitaGL podria dominar sobre el
#                                     ahorro de bytes).
#      NOTA (Fase 41): la idea #3 de optimization_ideas.md (PBO/DMA async) NO
#      tiene build de prueba -- se descarto con una inspeccion directa de
#      vitaGL.h (~/vitasdk/arm-vita-eabi/include/vitaGL.h): GL_PIXEL_UNPACK_BUFFER
#      no esta definido, solo GL_ARRAY_BUFFER/GL_ELEMENT_ARRAY_BUFFER -- sin ese
#      target, glTexSubImage2D no tiene forma de leer de un buffer mapeado
#      (pasar pixels=NULL seria una dereferencia de NULL garantizada, no un
#      experimento real). Ver port_progress.md Fase 41 para el detalle.
# Bug real encontrado en la Fase 32 (port_progress.md): como los flags de
# diagnostico son opciones de CACHE de CMake, invocar este script con un modo
# y despues con otro EN EL MISMO BUILD_DIR (siempre /tmp/zenonia4-build) hacia
# que el flag no mencionado en la segunda invocacion quedara pegado del valor
# cacheado de la primera -- asi fue como el build que crasheo en la Fase 32
# termino con INSTRUMENT_DRAWNATIVE_CALLS=ON e INSTRUMENT_HOTPATH_CALLS=ON
# simultaneos sin que nadie lo pidiera. Fix: fijar SIEMPRE todos los flags de
# forma explicita (ON u OFF), nunca dejar que ninguno quede sin mencionar --
# extendido a NEON_LIBC_INTERCEPT/DISABLE_VSYNC/DIRTY_RECT_UPLOAD/
# INSTRUMENT_CGSGRAPHICS_CALLS al agregarlos.
VPK_OUTPUT_NAME="zenonia_4"
CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF)
if [ "$1" = "--turbo-nologs" ]; then
    # Variante Turbo SIN LOGS: Maxima velocidad pura en CPU (cero I/O a SD, cero format strings).
    VPK_OUTPUT_NAME="zenonia_4_turbo_nologs"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF -DENABLE_LOGGING=OFF)
elif [ "$1" = "--turbo-opt" ]; then
    # Variante Turbo: FrameSkip multi-nivel (1.0x-3.0x), bypass de FastClear redundante, NEON memcpy/memset y subida directa RGB565.
    VPK_OUTPUT_NAME="zenonia_4_turbo"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF -DENABLE_LOGGING=ON)
elif [ "$1" = "--turbo-ring-test" ]; then
    # Fase 126: Turbo identico + FB_TEX_RING (3 texturas rotando, escritura directa,
    # sin el copy-on-write de vitaGL). build/zenonia_4_turbo_ring_test.vpk -- FPS
    # comparable al turbo; UPLOAD_PHASES on (timestamps) para ver la [UploadPhases] contra los ~17ms de Fase 125.
    VPK_OUTPUT_NAME="zenonia_4_turbo_ring_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF -DENABLE_LOGGING=ON -DFB_TEX_RING=ON -DINSTRUMENT_UPLOAD_PHASES=ON)
elif [ "$1" = "--pace" ]; then
    # Fase 134: PARALLEL_SPRITES promovido (confirmado en consola, Fase 133: ~11 -> ~18 FPS).
    # Fase 128: Turbo + FB_TEX_RING (Fase 126, confirmado en consola) + logs. Un solo VPK:
    # R+SELECT cicla frameskip (AUTO/OFF/1.5x..3x), L+SELECT alterna FPS 23 (velocidad
    # original del juego via SetSpeed) / 30 (forzado estilo Zenonia 2/3). build/zenonia_4_pace.vpk
    VPK_OUTPUT_NAME="zenonia_4_pace"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF -DENABLE_LOGGING=ON -DFB_TEX_RING=ON -DINSTRUMENT_UPLOAD_PHASES=OFF -DPARALLEL_SPRITES=ON)
elif [ "$1" = "--parallel-test" ]; then
    # Fase 132: EXPERIMENTO -- --pace + PARALLEL_SPRITES (sprites grandes en 3 nucleos). build/zenonia_4_parallel_test.vpk
    # Fase 128: Turbo + FB_TEX_RING (Fase 126, confirmado en consola) + logs. Un solo VPK:
    # R+SELECT cicla frameskip (AUTO/OFF/1.5x..3x), L+SELECT alterna FPS 23 (velocidad
    # original del juego via SetSpeed) / 30 (forzado estilo Zenonia 2/3). build/zenonia_4_pace.vpk
    VPK_OUTPUT_NAME="zenonia_4_parallel_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF -DENABLE_LOGGING=ON -DFB_TEX_RING=ON -DINSTRUMENT_UPLOAD_PHASES=OFF -DPARALLEL_SPRITES=ON)
elif [ "$1" = "--pace-nologs" ]; then
    # Fase 131: RELEASE -- identico a --pace sin logs (cero I/O a la SD). build/zenonia_4_pace_nologs.vpk
    # Fase 128: Turbo + FB_TEX_RING (Fase 126, confirmado en consola) + logs. Un solo VPK:
    # R+SELECT cicla frameskip (AUTO/OFF/1.5x..3x), L+SELECT alterna FPS 23 (velocidad
    # original del juego via SetSpeed) / 30 (forzado estilo Zenonia 2/3). build/zenonia_4_pace.vpk
    VPK_OUTPUT_NAME="zenonia_4_pace_nologs"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF -DENABLE_LOGGING=OFF -DFB_TEX_RING=ON -DINSTRUMENT_UPLOAD_PHASES=OFF -DPARALLEL_SPRITES=ON)
elif [ "$1" = "--turbo-hacks-test" ]; then
    # Fase 125: Turbo identico en loader + set SEGURO de hacks vitaGL (los que
    # no tocan upload compliance ni submission: MATH/CIRCULAR_POOL mueven CPU,
    # WVP_ON_GPU/NO_TILE_CLIPPER mueven trabajo a la GPU ociosa, PRIMITIVES
    # acelera draws de triangulos, NO_TEX_COMBINER es seguro porque el motor
    # solo usa TexEnv REPLACE -- verificado en logs). build/
    # zenonia_4_turbo_hacks_test.vpk -- FPS directamente comparable al turbo.
    VPK_OUTPUT_NAME="zenonia_4_turbo_hacks_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF -DENABLE_LOGGING=ON -DVITAGL_EXTRA_FLAGS="MATH_SPEEDHACK=1 CIRCULAR_POOL_SPEEDHACK=1 HAVE_WVP_ON_GPU=1 NO_TILE_CLIPPER=1 PRIMITIVES_SPEEDHACK=1 NO_TEX_COMBINER=1")
elif [ "$1" = "--cpu-max-opt" ] || [ "$1" = "--frameskip-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_frameskip"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF)
elif [ "$1" = "--preload" ]; then
    VPK_OUTPUT_NAME="zenonia_4_preload"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=ON)
elif [ "$1" = "--neon-fallback" ]; then
    VPK_OUTPUT_NAME="zenonia_4_neonfallback"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=OFF -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--drawnative-probe" ]; then
    VPK_OUTPUT_NAME="zenonia_4_drawnative_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=ON -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--hotpath-probe" ]; then
    VPK_OUTPUT_NAME="zenonia_4_hotpath_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=ON -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--neon-libc-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_neonlibc_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--no-vsync-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_novsync_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=ON -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--neon-novsync-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_neonnovsync_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=ON -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--dirtyrect-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_dirtyrect_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=ON -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--cgsgraphics-probe" ]; then
    VPK_OUTPUT_NAME="zenonia_4_cgsgraphics_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=ON -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--text-calls-probe" ]; then
    # Fase 86 (port_progress.md, 2026-07-25): --frame-phases-probe (Fase 85)
    # mostro que la pantalla de menu (sin sprites de gameplay) ya costaba 87ms
    # de NativeRender -- candidato directo: CGxFACharCache::DrawToFrameBuffer
    # (texto), nunca medido por su costo. Mismo caveat que los demas *_probe:
    # FPS no comparable al normal, solo importa avg_drawtoframebuffer_per_frame
    # del log.
    VPK_OUTPUT_NAME="zenonia_4_text_calls_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=ON -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--frame-phases-probe" ]; then
    # Fase 85 (port_progress.md, 2026-07-24): metodo NUEVO, no mapeado en
    # optimization_ideas.md -- mide donde se va el tiempo DENTRO del frame
    # (handleCletEvent / NativeRender / el resto) via sceKernelGetProcessTimeWide,
    # una vez por frame (ver CMakeLists.txt INSTRUMENT_FRAME_PHASES). A
    # diferencia de los otros *_probe, el overhead es insignificante -- SI es
    # valido comparar el FPS de este build contra el normal.
    VPK_OUTPUT_NAME="zenonia_4_frame_phases_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=ON -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--upload-probe" ]; then
    # Fase 125 (port_progress.md): config del build DEFAULT (misma receta de
    # perf: RGB565 directo + NEON_LIBC + FASTCLEAR + FRAMESKIP) mas las dos
    # mediciones de fases (FRAME_PHASES + UPLOAD_PHASES). Una sola corrida da
    # el desglose completo: clet / NativeRender / upload / swap / resto.
    # Overhead insignificante (timestamps, no hooks por llamada): el FPS SI es
    # comparable al default. build/zenonia_4_upload_probe.vpk.
    VPK_OUTPUT_NAME="zenonia_4_upload_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=ON -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DINSTRUMENT_UPLOAD_PHASES=ON -DENABLE_FRAMESKIP=ON -DENABLE_ASSET_PRELOAD=OFF)
elif [ "$1" = "--all-perf-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_allperf_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=ON -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=ON -DDIRTY_RECT_UPLOAD=ON -DINSTRUMENT_CGSGRAPHICS_CALLS=ON -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--fastclear-neon-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_fastclear_neon_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--interlaced-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_interlaced_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=ON -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--sprite-decode-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_sprite_decode_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=ON -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--sprite-decode-exalpha-test" ]; then
    # Fase 83 (port_progress.md, 2026-07-24): --sprite-decode-test + la
    # variante de header -6 (DrawOP_COPY_Compress_16_Ex_Alpha, alpha-blend
    # nativo) -- MENOS verificada que el resto (ver loader/sprite_decode.c),
    # build SEPARADO a proposito para poder aislar si algo sale mal sin
    # perder el fix ya confirmado de --sprite-decode-test.
    VPK_OUTPUT_NAME="zenonia_4_sprite_decode_exalpha_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=ON -DSPRITE_DECODE_EX_ALPHA=ON -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--sprite-decode-cache-test" ]; then
    # Fase 83 (port_progress.md, 2026-07-24): candidato 4 (threading acotado)
    # reencuadrado como cache de sprites COPY decodificados en CPU, sin
    # GPU/vitaGL/hilos (ver loader/sprite_decode.c) -- --sprite-decode-test +
    # SPRITE_DECODE_CACHE=ON, SIN la variante Ex_Alpha (build separado a
    # proposito, un cambio a la vez).
    VPK_OUTPUT_NAME="zenonia_4_sprite_decode_cache_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=ON -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=ON -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF)
elif [ "$1" = "--text-native-optimize-test" ]; then
    # Fase 89 (port_progress.md, 2026-07-25): TEXT_NATIVE_OPTIMIZE (Fase 87)
    # aislado en su propio build -- otra sesion lo habia dejado con ON por
    # DEFECTO en TODOS los builds sin ningun flag ni guard (choca con
    # INSTRUMENT_TEXT_CALLS y con GPU_QUADS, los 3 hookean
    # CGxFACharCache::DrawToFrameBuffer) y sin medir en consola real todavia.
    # Vuelve a OFF por defecto (mismo criterio que cualquier otra
    # optimizacion experimental de este proyecto) -- este build lo aisla para
    # medirlo solo, igual que --sprite-decode-exalpha-test/-cache-test.
    VPK_OUTPUT_NAME="zenonia_4_text_native_optimize_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=ON -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=ON)
elif [ "$1" = "--skip-blit-test" ]; then
    # Variante 1: techo de FPS (PutCompressImg no-op). Ver CMakeLists SKIP_SPRITE_BLIT.
    VPK_OUTPUT_NAME="zenonia_4_skip_blit_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DSKIP_SPRITE_BLIT=ON)
elif [ "$1" = "--gpu-atlas-test" ]; then
    # Texture Atlas GPU Quads: Zero-allocation, mega-texturas fijas 1024x1024, batching masivo.
    # Subida directa RGB565 (sin overhead NEON en CPU) con re-bindeo seguro de g_engine_bound_tex.
    VPK_OUTPUT_NAME="zenonia_4_gpu_atlas"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DGPU_QUADS=ON -DNEON_LIBC_INTERCEPT=ON -DFASTCLEAR_NEON_OPTIMIZE=ON -DSPRITE_DECODE_OPTIMIZE=ON)
elif [ "$1" = "--gpu-quads-test" ]; then
    # Variante 2: sprites COPY -> quads GPU. Ver CMakeLists GPU_QUADS / loader/gpu_quads.c.
    VPK_OUTPUT_NAME="zenonia_4_gpu_quads_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DGPU_QUADS=ON)
elif [ "$1" = "--gpu-quads-cpuconv-test" ]; then
    # Fase 78 (port_progress.md, 2026-07-24): diagnostico para el crash
    # determinista de las Fases 72-77 (6 `.psp2dmp` identicos, PC=0x0 dentro
    # de la propia `_glTexSubImage2D` de vitaGL, siempre en la subida RGB565
    # del framebuffer del motor -- ni el batching (Fase 75) ni el re-bind de
    # textura (Fase 76) ni descartar agotamiento de recurso por volumen
    # (Fase 77, solo ~1700 texturas totales) lo explicaron). Misma
    # combinacion que --gpu-quads-test pero con RGB565_DIRECT_UPLOAD=OFF --
    # la subida del FB pasa por `convert_rgb565_to_rgba8888` (CPU) y sube
    # como GL_RGBA/GL_UNSIGNED_BYTE, el MISMO formato que ya usan las
    # texturas de gpu_quads.c, evitando por completo el camino
    # GL_RGB/GL_UNSIGNED_SHORT_5_6_5 donde crashea siempre. Si el crash
    # desaparece con este build (mas lento, es solo diagnostico), confirma
    # que el bug es especifico del formato RGB565 en el despacho interno de
    # `_glTexSubImage2D` de vitaGL, no de gpu_quads.c en si.
    VPK_OUTPUT_NAME="zenonia_4_gpu_quads_cpuconv_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=OFF -DGPU_QUADS=ON)
elif [ "$1" = "--gpu-quads-pure-test" ]; then
    # Build puro de GPU QUADS, sin ninguna optimizacion de rendimiento, sin NEON.
    # Fase 95 (2026-07-25): este build ORIGINALMENTE tambien traia
    # DISABLE_VSYNC=ON -- la MISMA combinacion (DISABLE_VSYNC+GPU_QUADS) que la
    # Fase 81 confirmo que puede colgar la consola ENTERA (no solo la app, sin
    # `.psp2dmp`, requiere reiniciar hardware) por desborde de la cola de
    # comandos de la GPU real. El comentario de --gpu-quads-novsync-test (mas
    # abajo) YA advertia esto explicitamente por nombre ("NO USAR ... ni
    # --gpu-quads-pure-test") pero el flag seguia construyendo la combinacion
    # peligrosa igual -- una advertencia en un comentario no impide que otra
    # sesion futura (o el usuario probando builds ya compilados en build/) lo
    # siga usando sin verla. Una sesion de prueba real con este build NO
    # colgo la consola (solo corrupcion visual -- ver Fase 95), pero el
    # comentario de la Fase 81 ya preveia exactamente esto: "no crasheo" no es
    # evidencia de nada arreglado, solo que ESA corrida en particular no llego
    # a desbordar la cola. Corregido: DISABLE_VSYNC=OFF, este build ya NO
    # combina con GPU_QUADS -- si hace falta reproducir el cuelgue a
    # proposito, usar --gpu-quads-novsync-test (que ahora requiere la
    # confirmacion explicita de mas abajo).
    VPK_OUTPUT_NAME="zenonia_4_gpu_quads_pure_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=OFF -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DSPRITE_DECODE_EX_ALPHA=OFF -DSPRITE_DECODE_CACHE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF -DINSTRUMENT_FRAME_PHASES=OFF -DINSTRUMENT_TEXT_CALLS=OFF -DTEXT_NATIVE_OPTIMIZE=OFF -DGPU_QUADS=ON -DSKIP_SPRITE_BLIT=OFF -DDOWNSAMPLE_RENDER=OFF)
elif [ "$1" = "--gpu-quads-novsync-test" ]; then
    # Fase 80/81 (port_progress.md, 2026-07-24): PELIGRO CONFIRMADO EN
    # CONSOLA REAL -- este build (DISABLE_VSYNC=ON + GPU_QUADS=ON) COLGO LA
    # CONSOLA ENTERA (no solo el juego -- sin `.psp2dmp`, sin poder cerrar la
    # app, hubo que reiniciar hardware) durante un frame "pesado" (~16000
    # quads encolados, el mismo patron de siempre en areas densas). Sin el
    # freno de `sceDisplayWaitVblankStart()`, GPU_QUADS puede emitir mas
    # comandos de dibujo por frame de los que la cola de comandos de la GPU
    # puede absorber, y sin nada limitando la tasa de envio, la cola se
    # desborda -- un cuelgue de sistema real en hardware, no un crash de app
    # (que al menos genera un `.psp2dmp` simbolizable). NO USAR este build
    # (ni --gpu-quads-pure-test, que tambien tiene DISABLE_VSYNC=ON) para mas
    # pruebas de este bug -- el resultado "no crasheo" que parecia dar
    # --gpu-quads-pure-test antes NO era evidencia de nada arreglado, era
    # simplemente que esa corrida en particular no llego a desbordar la cola
    # de comandos. Se mantiene el flag solo por si hace falta reproducir el
    # cuelgue a proposito con el contexto completo de por que existe.
    VPK_OUTPUT_NAME="zenonia_4_gpu_quads_novsync_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DDISABLE_VSYNC=ON -DGPU_QUADS=ON)
elif [ "$1" = "--downsample-test" ]; then
    # Variante 3: resolucion interna reducida (2/3 por defecto) + upscale GPU.
    # OJO: zoomea la vista (el motor fusiona FOV y resolucion). Ratio ajustable
    # con DS_NUM/DS_DEN, p.ej. --downsample-test 3 4  -> 3/4 (zoom mas leve).
    VPK_OUTPUT_NAME="zenonia_4_downsample_test"
    DS_N="${2:-2}"; DS_D="${3:-3}"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DDOWNSAMPLE_RENDER=ON -DDS_NUM="$DS_N" -DDS_DEN="$DS_D")
elif [ "$1" = "--primitives-opt-test" ]; then
    # Variante 4: optimizacion de primitivas / ancho de banda de memoria --
    # combina las dos optimizaciones YA verificadas-seguras del proyecto:
    # FastClearNonClipping reimplementado (FASTCLEAR_NEON_OPTIMIZE, Fase 41) +
    # memcpy/memset NEON (NEON_LIBC_INTERCEPT, #4). Las ~13 primitivas restantes
    # de CGsGraphics pasan por RGBBlend16 (alpha per-pixel sin confirmar) y no se
    # pueden reimplementar sin riesgo hasta medir cual domina (--cgsgraphics-probe)
    # y reversear ese path -- ver optimization_ideas.md.
    VPK_OUTPUT_NAME="zenonia_4_primitives_opt_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DFASTCLEAR_NEON_OPTIMIZE=ON -DNEON_LIBC_INTERCEPT=ON)
fi

# Fase 95 (2026-07-25): guardia dura contra DISABLE_VSYNC=ON + GPU_QUADS=ON --
# la Fase 81 confirmo en consola real que esta combinacion puede colgar la
# consola ENTERA (sin `.psp2dmp`, requiere reiniciar hardware), y un comentario
# de texto en --gpu-quads-pure-test NO impidio que se siguiera construyendo/
# usando esa combinacion (ver el fix de ese flag mas arriba). En vez de confiar
# en que quien corre el script haya leido los comentarios, se verifica el
# array real de flags: si alguna variante (incluido --gpu-quads-novsync-test,
# que existe A PROPOSITO para reproducir el cuelgue) trae ambos, hace falta
# poner explicitamente I_ACCEPT_CONSOLE_HANG_RISK=1 en el entorno para
# continuar -- sin eso, el script se detiene ANTES de compilar nada.
HAS_DISABLE_VSYNC_ON=0
HAS_GPU_QUADS_ON=0
for arg in "${CMAKE_EXTRA_ARGS[@]}"; do
    [ "$arg" = "-DDISABLE_VSYNC=ON" ] && HAS_DISABLE_VSYNC_ON=1
    [ "$arg" = "-DGPU_QUADS=ON" ] && HAS_GPU_QUADS_ON=1
done
if [ "$HAS_DISABLE_VSYNC_ON" = "1" ] && [ "$HAS_GPU_QUADS_ON" = "1" ] && [ "$I_ACCEPT_CONSOLE_HANG_RISK" != "1" ]; then
    echo "================================================================"
    echo "  PELIGRO: esta variante combina DISABLE_VSYNC=ON + GPU_QUADS=ON."
    echo "  La Fase 81 (port_progress.md) confirmo que esta combinacion puede"
    echo "  colgar la consola ENTERA en hardware real (no un crash de app --"
    echo "  sin .psp2dmp, requiere reiniciar el equipo)."
    echo ""
    echo "  Si de verdad queres reproducir ese cuelgue a proposito (unico uso"
    echo "  valido de esta combinacion), corre de nuevo con:"
    echo "    I_ACCEPT_CONSOLE_HANG_RISK=1 ./build.sh $1"
    echo "================================================================"
    exit 1
fi

# Configuracion
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="/tmp/zenonia4-build"
SRC_DIR="/tmp/zenonia4-src"
VPK_NAME="${VPK_OUTPUT_NAME}.vpk"

echo "================================================================"
echo "  Script de Build Automatico para Zenonia 4 (PS Vita)"
echo "================================================================"

echo "[1/4] Preparando entorno de compilacion..."
# Evitamos problemas de rutas con espacios ("PSVITA Develop") usando un
# directorio temporal (ver psvita-porting skill, toolchain_gotchas.md).
mkdir -p "$BUILD_DIR"
mkdir -p "$SRC_DIR"

if [ -z "$VITASDK" ]; then
    if [ -d "/usr/local/vitasdk" ]; then
        export VITASDK="/usr/local/vitasdk"
    elif [ -d "$HOME/vitasdk" ]; then
        export VITASDK="$HOME/vitasdk"
    else
        echo "Error: La variable de entorno VITASDK no esta definida y no se encontro en rutas por defecto."
        exit 1
    fi
    export PATH="$VITASDK/bin:$PATH"
fi

rsync -a --delete --exclude '.git' --exclude 'build' --exclude '.*' \
    --exclude 'zenonia-4-1-2-3' --exclude 'decompiled' --exclude 'ux0_data' \
    --exclude '*.apk' --exclude '*.zip' \
    "$PROJECT_DIR/" "$SRC_DIR/"
# --delete: el staging debe ser espejo exacto. Sin el, cambiar el arbol
# vendor/ deja .o/.a viejos y `make` no recompila (mtimes) -> builds
# "aislados" que linkean la version anterior. rm de *-prefix: los stamps de
# ExternalProject saltarian el make vendorizado aunque el staging haya
# cambiado -> lib obsoleta o ausente en el link. Ambas cosas ya mordieron
# en la investigacion de la pantalla negra (2026-09-04).
rm -rf "$BUILD_DIR/vitaGL_vendored-prefix"

# libgameDSO.so si vive como symlink apuntando afuera del arbol copiado por
# rsync (ver loader/lib/libgameDSO.so) -- resolverlo a una copia real dentro
# de SRC_DIR para que el build no dependa de que zenonia-4-1-2-3/ tambien se
# haya copiado.
if [ -L "$PROJECT_DIR/lib/libgameDSO.so" ]; then
    cp -L "$PROJECT_DIR/lib/libgameDSO.so" "$SRC_DIR/lib/libgameDSO.so"
fi

echo "[2/4] Ejecutando CMake y Make..."
cd "$BUILD_DIR"

# cmake instalado como binario prebuilt sin sudo (ver $HOME/tools) en la maquina
# WSL de desarrollo -- inofensivo en otras maquinas donde cmake ya este en PATH.
export PATH="$HOME/tools:$PATH"

# Los flags de perf/diagnostico son opciones de CACHE de CMake. Los branches de
# arriba pasan explicitamente TODOS los flags historicos, pero los flags nuevos
# (SKIP_SPRITE_BLIT / GPU_QUADS / DOWNSAMPLE_RENDER, agregados 2026-07-19) no
# estan en los branches viejos -- borrar el cache antes de configurar evita que
# queden "pegados" del valor de una corrida anterior con otro modo (mismo bug de
# stickiness documentado en la Fase 32, resuelto aca de raiz para no tener que
# enumerar cada flag nuevo en los ~13 branches).
rm -f "$BUILD_DIR/CMakeCache.txt"

cmake "$SRC_DIR" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release "${CMAKE_EXTRA_ARGS[@]}"
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "[3/4] Exportando archivos generados..."
mkdir -p "$PROJECT_DIR/build"
cp "$BUILD_DIR/$VPK_NAME" "$PROJECT_DIR/build/$VPK_NAME"
echo "VPK generado en: $PROJECT_DIR/build/$VPK_NAME"
# eboot.bin suelto (= el .self del VPK) para actualizar por FTP sin reinstalar el VPK; lo pisa cada build.
cp "$BUILD_DIR/${VPK_OUTPUT_NAME}.self" "$PROJECT_DIR/build/eboot.bin"
echo "eboot.bin ($VPK_OUTPUT_NAME) en: $PROJECT_DIR/build/eboot.bin"

echo "[4/4] Deploy / instalacion (opcional)"
echo "Para instalar en un Vita real o Vita3K, usar manage_vita.py."

