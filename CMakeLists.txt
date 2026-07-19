cmake_minimum_required(VERSION 3.10)

## Esto incluye el toolchain de Vita, debe ir antes de project()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(zenonia_4)
include("${VITASDK}/share/vita.cmake" REQUIRED)

## Configuracion de la app
set(VITA_APP_NAME "Zenonia 4")
# ID unico de 9 caracteres, distinto del de Zenonia 2 (PSVZ00002) y Zenonia 3
# (PSVZ00003) para poder tener los tres juegos instalados a la vez.
set(VITA_TITLEID  "PSVZ00004")
set(VITA_VERSION  "01.00")

## Log verbose de cada llamada JNI de FalsoJNI -- ver la nota completa en
## Zenonia3-vita/CMakeLists.txt (mismo criterio: imprescindible para el
## primer arranque, debe apagarse antes de medir rendimiento real).
option(ENABLE_VERBOSE_JNI_LOG "Log every FalsoJNI call (debug only, slow)" OFF)
if (ENABLE_VERBOSE_JNI_LOG)
  add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
else()
  add_definitions(-DFALSOJNI_DEBUGLEVEL=2)
endif()

## Oculta la cruceta nativa + botones de accion (zenonia_install_hide_dpad_hook()
## en loader/dynlib.c) hookeando GVUIPlayerController::CallBack2 -- confirmado
## en consola real que, a diferencia de Zenonia 3, este objeto se crea de forma
## perezosa (singleton) recien la primera vez que se llama a CallBack2, no en
## su propio constructor (ver port_progress.md Fase 15). Si el simbolo no se
## encuentra, el hook se salta con un log y no rompe el arranque.
option(HIDE_VIRTUAL_GAMEPAD "Hide the game's on-screen D-pad + action button cluster; physical Vita buttons are already mapped" OFF)
if (HIDE_VIRTUAL_GAMEPAD)
  add_definitions(-DZENONIA_HIDE_DPAD_UI=1)
endif()

## Subir el framebuffer RGB565 del motor directo a una textura
## GL_UNSIGNED_SHORT_5_6_5 sin convertir a RGBA8888 en CPU (salta por completo
## convert_rgb565_to_rgba8888(), ver loader/dynlib.c). Promovido a DEFAULT ON
## en 2026-07-17 (ver port_progress.md Fase 25/26): probado en consola real,
## subio el framerate de ~7-8 FPS (NEON) a 11-14 FPS estable, sin ningun error
## de color -- el riesgo conocido del port de Zenonia 2 (mismo motor, mismo
## enfoque, dio colores incorrectos alla) NO se manifesto en Zenonia 4.
## La ruta NEON (Fase 22) se mantiene completa en el codigo detras de este
## flag por si algun asset/locale no probado todavia revela el mismo problema
## de color que en Zenonia 2 -- ver build.sh --neon-fallback para volver a
## generar ese build sin tocar este default.
option(RGB565_DIRECT_UPLOAD "Upload the engine's RGB565 framebuffer straight to a GL texture, skipping the NEON RGB565->RGBA8888 conversion (confirmed on real hardware: +FPS, no color regressions)" ON)
if (RGB565_DIRECT_UPLOAD)
  add_definitions(-DZENONIA_RGB565_DIRECT=1)
endif()

## Fase 29 (port_progress.md): hook de SOLO CONTEO sobre DrawNative (el
## blitter real detras de CMvCharacter::DrawCharacter -> CGxPZxAni::Draw ->
## CGcxBitmap::Blt/PutNativeImage -- ver dynlib.c) para confirmar en consola
## real cuantas veces por frame se llama, antes de intentar cualquier parche
## de escala sobre el. El pass-through hace un unhook/rehook (con flush de
## i-cache) en CADA invocacion -- overhead real medible si DrawNative se llama
## muchas veces por frame. Por eso es un build de diagnostico APARTE: el FPS
## medido con esto activo no es comparable al build normal, solo importa
## avg_drawnative_calls_per_frame (ver build.sh --drawnative-probe).
option(INSTRUMENT_DRAWNATIVE_CALLS "Count DrawNative() calls per frame via a pass-through hook (diagnostic only, adds real per-call overhead -- do not use to judge FPS)" OFF)
if (INSTRUMENT_DRAWNATIVE_CALLS)
  add_definitions(-DZENONIA_INSTRUMENT_DRAWNATIVE=1)
endif()

## Fase 31 (port_progress.md): la hipotesis de la Fase 29 (DrawNative) se
## descarto en la Fase 30 -- ~0-1 llamadas/frame, no el hot path. Este flag
## instrumenta 3 candidatos a la vez (CGxPZxAni::Draw, CGxPZxZero::Blt,
## PutCompressImg -- ver dynlib.c) en vez de apostar a uno solo por ronda.
## Mismo caveat que INSTRUMENT_DRAWNATIVE_CALLS: cada hook agrega overhead
## real por invocacion, el FPS medido con esto activo no es comparable al
## build normal, solo importan los 3 contadores nuevos del log.
option(INSTRUMENT_HOTPATH_CALLS "Count CGxPZxAni::Draw / CGxPZxZero::Blt / PutCompressImg calls per frame via pass-through hooks (diagnostic only, adds real per-call overhead -- do not use to judge FPS)" OFF)
if (INSTRUMENT_HOTPATH_CALLS)
  add_definitions(-DZENONIA_INSTRUMENT_HOTPATH=1)
endif()

## optimization_ideas.md #4: redirige los imports "memcpy"/"memset" del .so
## (tabla default_dynlib en loader/dynlib.c) a neon_mem.c (loop NEON manual de
## 64/16 bytes) en vez de la libc estandar de vitasdk. TODAVIA SIN CONFIRMAR
## en consola real -- default OFF, ver build.sh --neon-libc-test para un VPK
## separado (mismo criterio de medicion que RGB565_DIRECT_UPLOAD antes de
## promoverlo a ON por default).
option(NEON_LIBC_INTERCEPT "Route memcpy/memset imports to a hand-written NEON implementation (loader/neon_mem.c) instead of vitasdk libc (experimental, not yet measured on hardware)" OFF)
if (NEON_LIBC_INTERCEPT)
  add_definitions(-DZENONIA_NEON_LIBC=1)
endif()

## optimization_ideas.md #9: prueba diagnostica para descartar si el framerate
## actual (~9-11 FPS) esta artificialmente atado a la espera de vblank de
## vglSwapBuffers en vez del costo real de CPU del blit por software. Llama
## vglWaitVblankStart(GL_FALSE) una vez en gl_init() (loader/main.c) para que
## vglSwapBuffers ya no bloquee esperando el refresco de pantalla -- permite
## tearing, SOLO para medir si el FPS sube; no pensado como default. Requiere
## que la vitaGL vendorizada/instalada tenga vglWaitVblankStart -- si el link
## falla con "undefined reference", esa version de vitaGL no la expone y esta
## opcion debe quedar en OFF.
option(DISABLE_VSYNC "Disable vitaGL's vblank wait in gl_init() to test whether display sync is capping FPS artificially (experimental diagnostic, causes tearing, requires vglWaitVblankStart in the linked vitaGL)" OFF)
if (DISABLE_VSYNC)
  add_definitions(-DZENONIA_DISABLE_VSYNC=1)
endif()

## optimization_ideas.md #2 (Fase 36): antes de subir el framebuffer RGB565
## completo cada frame via glTexSubImage2D (solo tiene sentido junto con
## RGB565_DIRECT_UPLOAD -- el camino NEON-fallback convierte a RGBA8888 antes,
## formato distinto, sin tocar por ahora), compara fila por fila contra una
## copia cacheada del frame anterior (loader/dynlib.c) y sube solo el rango de
## filas [primera,ultima] que realmente cambiaron. Sin confirmar todavia
## cuanto del framebuffer cambia por frame en combate real -- si la respuesta
## es "casi todo" (motor 2D con fondo+sprites en movimiento constante), esto
## no ahorra nada y el costo del memcmp/memcpy de diffing pasa a ser una
## regresion neta; ver build.sh --dirtyrect-test y el contador
## avg_dirtyrect_pct_uploaded del log antes de considerar promoverlo.
option(DIRTY_RECT_UPLOAD "Diff the RGB565 framebuffer against the previous frame and upload only the changed row range via glTexSubImage2D, instead of the full buffer every frame (experimental, not yet measured -- only active together with RGB565_DIRECT_UPLOAD)" OFF)
if (DIRTY_RECT_UPLOAD)
  add_definitions(-DZENONIA_DIRTY_RECT_UPLOAD=1)
endif()

## "Lo que NO se ha hecho" #3 (Fase 36): antes de vectorizar/parchear a mano
## las ~13 primitivas de dibujo mas relevantes de CGsGraphics:: (DrawLine,
## DrawFillRect, DrawRoundRect, DrawFillRoundRect, DrawEllipse x2,
## DrawFillEllipse, ClearScreen, BeginScreen, FastClearNonClipping,
## GetFrameBufferPtr, PutPixel x2 -- ver dynlib.c), confirmar primero si
## alguna se llama con suficiente frecuencia por frame como para importar,
## mismo criterio de "medir antes de parchear" que INSTRUMENT_HOTPATH_CALLS.
## A diferencia de DrawNative (Fase 29/30), estos SI son simbolos exportados
## directamente hookeables (confirmados con `nm -D` sobre libgameDSO.so, no
## despacho por vtable), asi que el riesgo de hookear la funcion equivocada es
## bajo. Contador UNICO agregado (no uno por primitiva, para no inflar el
## log) -- si sale alto, una ronda de seguimiento puede desglosar cual de las
## 13 domina.
option(INSTRUMENT_CGSGRAPHICS_CALLS "Count calls to ~13 CGsGraphics:: drawing primitives per frame via pass-through hooks, aggregated into one counter (diagnostic only, adds real per-call overhead -- do not use to judge FPS)" OFF)
if (INSTRUMENT_CGSGRAPHICS_CALLS)
  add_definitions(-DZENONIA_INSTRUMENT_CGSGRAPHICS=1)
endif()

## Nombre base del .self/.vpk generado -- separado de PROJECT_NAME (que sigue
## siendo el nombre del target de compilacion) para poder generar un VPK con
## nombre distinto en la misma build tree sin pisar build/zenonia_4.vpk, p.ej.
## para el build de respaldo NEON (ver build.sh --neon-fallback) o los de
## diagnostico (ver build.sh --drawnative-probe / --hotpath-probe).
set(VPK_OUTPUT_NAME "zenonia_4" CACHE STRING "Base filename for the generated .self/.vpk")
if (NOT RGB565_DIRECT_UPLOAD)
  set(VITA_APP_NAME "${VITA_APP_NAME} [NEON fallback]")
elseif (INSTRUMENT_DRAWNATIVE_CALLS)
  set(VITA_APP_NAME "${VITA_APP_NAME} [DrawNative probe]")
elseif (INSTRUMENT_HOTPATH_CALLS)
  set(VITA_APP_NAME "${VITA_APP_NAME} [HotPath probe]")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
# ATTRIBUTE2=12 pide el presupuesto de memoria extendido -- sin esto,
# SceShaccCg (compilador de shaders Cg de vitaGL) puede fallar al iniciar.
# Copiado de la config confirmada funcionando en Zenonia 2/3.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1 -d ATTRIBUTE2=12")

include_directories(
  ./common # para debugScreenPrintf()
  ./lib    # para <falso_jni/FalsoJNI.h>
)

link_directories(
  ${CMAKE_CURRENT_BINARY_DIR}
)

add_executable(${PROJECT_NAME}
  ./loader/main.c
  ./loader/dynlib.c
  ./loader/so_util.c
  ./loader/java.c
  ./loader/audio.c
  ./loader/neon_mem.c
  ./loader/font.c
  ./loader/htmltext.c
  ./loader/htmlview.c
  ./loader/androidui.c
  ./common/debugScreen.c
  ./lib/falso_jni/FalsoJNI.c
  ./lib/falso_jni/FalsoJNI_ImplBridge.c
  ./lib/falso_jni/FalsoJNI_Logger.c
)

target_link_libraries(${PROJECT_NAME}
  vitaGL
  vitashark
  mathneon
  m
  # Decodificador OGG Vorbis (Tremor, punto fijo) para loader/audio.c
  vorbisidec
  ogg
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  SceAudioIn_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceSysmodule_stub
  ScePower_stub
  SceGxm_stub
  SceShaccCg_stub
  SceShaccCgExt
  SceCommonDialog_stub
  SceKernelDmacMgr_stub
  taihen_stub
  kubridge_stub
)

## Crear archivos de Vita
vita_create_self(${VPK_OUTPUT_NAME}.self ${PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VPK_OUTPUT_NAME}.vpk ${VITA_TITLEID} ${VPK_OUTPUT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/pic0.png sce_sys/pic0.png
  FILE sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  # Fuente TTF para el rasterizado GFA (loader/font.c), heredada de
  # Zenonia 3 (NanumGothic, licencia OFL) hasta confirmar si Zenonia 4
  # necesita coreano de verdad (ver plan_zenonia4_port.md Fase 6).
  FILE font.ttf font.ttf
)
