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
CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF)
if [ "$1" = "--neon-fallback" ]; then
    VPK_OUTPUT_NAME="zenonia_4_neonfallback"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=OFF -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF)
elif [ "$1" = "--drawnative-probe" ]; then
    VPK_OUTPUT_NAME="zenonia_4_drawnative_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=ON -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF)
elif [ "$1" = "--hotpath-probe" ]; then
    VPK_OUTPUT_NAME="zenonia_4_hotpath_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=ON -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF)
elif [ "$1" = "--neon-libc-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_neonlibc_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF)
elif [ "$1" = "--no-vsync-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_novsync_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=ON -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF)
elif [ "$1" = "--neon-novsync-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_neonnovsync_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=ON -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF)
elif [ "$1" = "--dirtyrect-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_dirtyrect_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=ON -DINSTRUMENT_CGSGRAPHICS_CALLS=OFF)
elif [ "$1" = "--cgsgraphics-probe" ]; then
    VPK_OUTPUT_NAME="zenonia_4_cgsgraphics_probe"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=OFF -DNEON_LIBC_INTERCEPT=OFF -DDISABLE_VSYNC=OFF -DDIRTY_RECT_UPLOAD=OFF -DINSTRUMENT_CGSGRAPHICS_CALLS=ON)
elif [ "$1" = "--all-perf-test" ]; then
    VPK_OUTPUT_NAME="zenonia_4_allperf_test"
    CMAKE_EXTRA_ARGS=(-DVPK_OUTPUT_NAME="$VPK_OUTPUT_NAME" -DRGB565_DIRECT_UPLOAD=ON -DINSTRUMENT_DRAWNATIVE_CALLS=OFF -DINSTRUMENT_HOTPATH_CALLS=ON -DNEON_LIBC_INTERCEPT=ON -DDISABLE_VSYNC=ON -DDIRTY_RECT_UPLOAD=ON -DINSTRUMENT_CGSGRAPHICS_CALLS=ON)
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

cmake "$SRC_DIR" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release "${CMAKE_EXTRA_ARGS[@]}"
make -j$(sysctl -n hw.ncpu)

echo "[3/4] Exportando archivos generados..."
mkdir -p "$PROJECT_DIR/build"
cp "$BUILD_DIR/$VPK_NAME" "$PROJECT_DIR/build/$VPK_NAME"
echo "VPK generado en: $PROJECT_DIR/build/$VPK_NAME"

echo "[4/4] Deploy / instalacion (opcional)"
echo "Para instalar en un Vita real o Vita3K, usar manage_vita.py."
