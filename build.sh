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
CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
if [ "$1" = "--neon-fallback" ]; then
    VPK_OUTPUT_NAME="zenonia_4_neonfallback"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=OFF -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--drawnative-probe" ]; then
    VPK_OUTPUT_NAME="zenonia_4_drawnative_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=ON -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--hotpath-probe" ]; then
    VPK_OUTPUT_NAME="zenonia_4_hotpath_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=ON -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--neon-libc-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_neonlibc_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--no-vsync-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_novsync_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=ON -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--neon-novsync-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_neonnovsync_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=ON -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--dirtyrect-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_dirtyrect_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=ON -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--cgsgraphics-probe" ]; then
    VPK_OUTPUT_NAME="zenonia_4_cgsgraphics_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=ON -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--all-perf-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_allperf_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=ON -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=ON -DDIRTY_RECT_UPLOAD=ON -DINSTRUMENT_CGSGRAPHICS_CALLS=ON -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--fastclear-neon-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_fastclear_neon_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=ON -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--interlaced-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_interlaced_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=OFF -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=ON)
elif [ "$1" = "--sprite-decode-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_sprite_decode_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF -DSPRITE_DECODE_OPTIMIZE=ON -DFASTCLEAR_NEON_OPTIMIZE=OFF -DINTERLACED_UPLOAD=OFF)
elif [ "$1" = "--skip-blit-test" ]; then
    # Variante 1: techo de FPS (PutCompressImg no-op). Ver CMakeLists SKIP_SPRITE_BLIT.
    VPK_OUTPUT_NAME="zenonia_4_skip_blit_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DSKIP_SPRITE_BLIT=ON)
elif [ "$1" = "--gpu-quads-test" ]; then
    # Variante 2: sprites COPY -> quads GPU. Ver CMakeLists GPU_QUADS / loader/gpu_quads.c.
    VPK_OUTPUT_NAME="zenonia_4_gpu_quads_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DGPU_QUADS=ON)
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

rsync -a --exclude '.git' --exclude 'build' --exclude '.*' \
    --exclude 'zenonia-4-1-2-3' --exclude 'decompiled' --exclude 'ux0_data' \
    --exclude '*.apk' --exclude '*.zip' \
    "$PROJECT_DIR/" "$SRC_DIR/"

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

echo "[4/4] Deploy / instalacion (opcional)"
echo "Para instalar en un Vita real o Vita3K, usar manage_vita.py."

