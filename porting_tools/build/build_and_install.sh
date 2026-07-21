#!/bin/bash
set -e

# Configuración
PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_SH="$PROJECT_DIR/build.sh"

echo "================================================================"
echo "  Script de Build Automatico para Zenonia 4 (PS Vita)"
echo "================================================================"

if [ ! -x "$BUILD_SH" ]; then
    echo "Error: no se encontro (o no es ejecutable) $BUILD_SH"
    exit 1
fi

# Descripciones cortas para el menu. La lista de flags REALES se descubre
# dinamicamente de build.sh (grep de sus propios "elif [ "$1" = "--xxx" ]")
# en vez de mantenerse hardcodeada aca -- asi este script nunca queda
# desincronizado cuando se agrega un flag nuevo a build.sh (si aparece un
# flag sin descripcion en el case de abajo, el menu lo muestra igual con una
# descripcion generica, ver flag_desc()). Un `case` en vez de un array
# asociativo porque el bash 3.2 que trae macOS por default no tiene arrays
# asociativos (`declare -A`).
flag_desc() {
    case "$1" in
        --neon-fallback) echo "Respaldo con conversion NEON RGB565->RGBA8888 (en vez del upload directo default)" ;;
        --drawnative-probe) echo "Diagnostico: solo-conteo sobre DrawNative() (hipotesis ya descartada, Fase 30)" ;;
        --hotpath-probe) echo "Diagnostico: solo-conteo sobre CGxPZxAni::Draw/CGxPZxZero::Blt/PutCompressImg" ;;
        --neon-libc-test) echo "Experimental: memcpy/memset redirigidos a NEON" ;;
        --no-vsync-test) echo "Diagnostico: deshabilita la espera de vblank (causa tearing, solo para medir)" ;;
        --neon-novsync-test) echo "Combina --neon-libc-test + --no-vsync-test" ;;
        --dirtyrect-test) echo "Experimental: solo sube las filas del framebuffer que cambiaron" ;;
        --cgsgraphics-probe) echo "Diagnostico: solo-conteo sobre las primitivas CGsGraphics:: (DrawLine, DrawRect, etc.)" ;;
        --all-perf-test) echo "Combina NEON_LIBC + NO_VSYNC + DIRTY_RECT + HOTPATH_PROBE a la vez" ;;
        --fastclear-neon-test) echo "Reimplementacion de CGsGraphics::FastClearNonClipping" ;;
        --interlaced-test) echo "Sube solo filas pares/impares alternadas del framebuffer por frame" ;;
        --sprite-decode-test) echo "Reimplementacion nativa de la decodificacion RLE de sprites (PutCompressImg)" ;;
        --skip-blit-test) echo "Variante 1: techo de FPS teorico (PutCompressImg no-op, sprites no se dibujan)" ;;
        --gpu-quads-test) echo "Variante 2: offload de sprites PutCompressImg a quads texturizados por GPU (vitaGL)" ;;
        --downsample-test) echo "Variante 3: resolucion interna reducida + upscale GPU (pide ratio DS_NUM/DS_DEN)" ;;
        --primitives-opt-test) echo "Variante 4: FastClearNonClipping reimplementado + memcpy/memset NEON juntos" ;;
        *) echo "(sin descripcion corta -- ver el comentario de este flag en build.sh)" ;;
    esac
}

# Flags reales: se extraen de las condiciones "$1" = "--xxx" de build.sh, en
# el orden en que aparecen en el archivo. Loop portable en vez de `mapfile`
# (no existe en el bash 3.2 que trae macOS por default).
FLAGS=()
while IFS= read -r line; do
    FLAGS+=("$line")
done < <(grep -o '"\-\-[a-z0-9-]\+"' "$BUILD_SH" | tr -d '"' | awk '!seen[$0]++')

echo ""
echo "[1/4] Elegi que build de $(basename "$BUILD_SH") queres compilar:"
echo "  0) (build normal, sin flag) -- build/zenonia_4.vpk"
i=1
for f in "${FLAGS[@]}"; do
    printf "  %d) %-24s -- %s\n" "$i" "$f" "$(flag_desc "$f")"
    i=$((i + 1))
done
echo ""
read -p "Numero de opcion [0]: " FLAG_CHOICE
FLAG_CHOICE="${FLAG_CHOICE:-0}"

BUILD_FLAG=""
if [ "$FLAG_CHOICE" != "0" ]; then
    idx=$((FLAG_CHOICE - 1))
    if [ "$idx" -lt 0 ] || [ "$idx" -ge "${#FLAGS[@]}" ]; then
        echo "Opcion invalida."
        exit 1
    fi
    BUILD_FLAG="${FLAGS[$idx]}"
fi

BUILD_ARGS=()
if [ -n "$BUILD_FLAG" ]; then
    BUILD_ARGS+=("$BUILD_FLAG")
fi

# --downsample-test acepta 2 argumentos posicionales extra (DS_NUM/DS_DEN,
# default 2/3 -- ver el comentario de este flag en build.sh).
if [ "$BUILD_FLAG" = "--downsample-test" ]; then
    read -p "Ratio de downsample DS_NUM/DS_DEN [2/3]: " DS_RATIO
    DS_RATIO="${DS_RATIO:-2/3}"
    DS_NUM="${DS_RATIO%%/*}"
    DS_DEN="${DS_RATIO##*/}"
    BUILD_ARGS+=("$DS_NUM" "$DS_DEN")
fi

echo ""
echo "[2/4] Ejecutando build.sh ${BUILD_ARGS[*]} ..."
echo "(build.sh ya se encarga del staging en /tmp para evitar el bug de"
echo " vita-pack-vpk con rutas con espacios -- ver toolchain_gotchas.md)"
"$BUILD_SH" "${BUILD_ARGS[@]}"

# VPK_OUTPUT_NAME real que uso build.sh para este flag (misma logica de
# nombres que build.sh, para saber que archivo buscar en build/ despues).
VPK_OUTPUT_NAME="zenonia_4"
case "$BUILD_FLAG" in
    --neon-fallback) VPK_OUTPUT_NAME="zenonia_4_neonfallback" ;;
    --drawnative-probe) VPK_OUTPUT_NAME="zenonia_4_drawnative_probe" ;;
    --hotpath-probe) VPK_OUTPUT_NAME="zenonia_4_hotpath_probe" ;;
    --neon-libc-test) VPK_OUTPUT_NAME="zenonia_4_neonlibc_test" ;;
    --no-vsync-test) VPK_OUTPUT_NAME="zenonia_4_novsync_test" ;;
    --neon-novsync-test) VPK_OUTPUT_NAME="zenonia_4_neonnovsync_test" ;;
    --dirtyrect-test) VPK_OUTPUT_NAME="zenonia_4_dirtyrect_test" ;;
    --cgsgraphics-probe) VPK_OUTPUT_NAME="zenonia_4_cgsgraphics_probe" ;;
    --all-perf-test) VPK_OUTPUT_NAME="zenonia_4_allperf_test" ;;
    --fastclear-neon-test) VPK_OUTPUT_NAME="zenonia_4_fastclear_neon_test" ;;
    --interlaced-test) VPK_OUTPUT_NAME="zenonia_4_interlaced_test" ;;
    --sprite-decode-test) VPK_OUTPUT_NAME="zenonia_4_sprite_decode_test" ;;
    --skip-blit-test) VPK_OUTPUT_NAME="zenonia_4_skip_blit_test" ;;
    --gpu-quads-test) VPK_OUTPUT_NAME="zenonia_4_gpu_quads_test" ;;
    --downsample-test) VPK_OUTPUT_NAME="zenonia_4_downsample_test" ;;
    --primitives-opt-test) VPK_OUTPUT_NAME="zenonia_4_primitives_opt_test" ;;
esac
VPK_NAME="${VPK_OUTPUT_NAME}.vpk"
VPK_PATH="$PROJECT_DIR/build/$VPK_NAME"

if [ ! -f "$VPK_PATH" ]; then
    echo "Error: build.sh termino pero no encuentro $VPK_PATH -- revisar el nombre de VPK_OUTPUT_NAME arriba."
    exit 1
fi
echo "Build exitoso: $VPK_PATH"

echo ""
echo "[3/4] Exportando el ELF sin firmar (para simbolizar un .psp2dmp con vita-parse-core)..."
# build.sh compila en /tmp/zenonia4-build pero solo copia el .vpk a build/ --
# el ELF sin firmar queda ahi. Se archiva junto al VPK porque /tmp se borra
# al reiniciar y es imprescindible para simbolizar un crash despues (ver
# psvita-porting/references/hardware_debugging.md, so-crash-triage). El
# target del ejecutable es SIEMPRE "zenonia_4" (viene de project(zenonia_4)
# en CMakeLists.txt, no cambia con VPK_OUTPUT_NAME) -- NO existe un
# "eboot.bin"/"so_loader" sueltos en este arbol de build (esos nombres eran
# del scaffold generico que Fase 2 de port_progress.md reemplazo; ver
# CMakeLists.txt real, que no los menciona).
TMP_BUILD_DIR="/tmp/zenonia4-build"
if [ -f "$TMP_BUILD_DIR/zenonia_4" ]; then
    cp "$TMP_BUILD_DIR/zenonia_4" "$PROJECT_DIR/build/${VPK_OUTPUT_NAME}.elf"
    echo "ELF con simbolos exportado a: $PROJECT_DIR/build/${VPK_OUTPUT_NAME}.elf"
else
    echo "(no se encontro $TMP_BUILD_DIR/zenonia_4 -- si el nombre del target de CMakeLists.txt cambio, actualizar este script)"
fi

echo ""
echo "[4/4] Instalacion (opcional)"
VITA3K_APP="/Applications/Vita3K.app/Contents/MacOS/Vita3K"
if [ -x "$VITA3K_APP" ]; then
    read -p "¿Deseas instalar y ejecutar $VPK_NAME en Vita3K ahora? [s/N] " INSTALL_VITA3K
    if [[ "$INSTALL_VITA3K" =~ ^[sS]$ ]]; then
        echo "Instalando VPK y lanzando el emulador (backend OpenGL)..."
        "$VITA3K_APP" -B OpenGL "$VPK_PATH" > /dev/null 2>&1 &
        echo "Listo."
    else
        echo "Omitiendo instalacion automatica en Vita3K."
    fi
else
    echo "Vita3K no encontrado en la ruta por defecto (/Applications/Vita3K.app)."
fi
echo "Para instalar en un Vita real (FTP+comando de VitaShell), usar manage_vita.py o 'make send' desde el arbol de build."
