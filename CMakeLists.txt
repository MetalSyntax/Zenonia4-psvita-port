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
## en loader/dynlib.c loader/sprite_decode.c) hookeando GVUIPlayerController::CallBack2 -- confirmado
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
## convert_rgb565_to_rgba8888(), ver loader/dynlib.c loader/sprite_decode.c). Promovido a DEFAULT ON
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
## (tabla default_dynlib en loader/dynlib.c loader/sprite_decode.c) a neon_mem.c (loop NEON manual de
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
## copia cacheada del frame anterior (loader/dynlib.c loader/sprite_decode.c) y sube solo el rango de
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

## Fase 41 (port_progress.md), "Lo que NO se ha hecho" #3: reimplementacion
## REAL (no solo conteo) de CGsGraphics::FastClearNonClipping, confirmada
## fiel contra su pseudo-C real en out_ghidra.c (ver dynlib.c
## zenonia_install_fastclear_optimize) -- colapsa el loop de memset por fila
## en un solo memset cuando el rectangulo es de ancho completo (filas
## contiguas), y usa neon_memset en el resto de los casos. De las 13
## primitivas contadas por INSTRUMENT_CGSGRAPHICS_CALLS, es la UNICA con una
## reimplementacion segura confirmada -- ClearScreen tiene un camino
## MC_grpSetContext/MC_grpFillRect para colores reales que no se puede
## reproducir sin reversear MC_grp*, y DrawFillRect/DrawRoundRect/DrawEllipse
## pasan por RGBBlend16 (alpha blending per-pixel no confirmado). TODAVIA SIN
## MEDIR en consola real -- default OFF, ver build.sh --fastclear-neon-test.
option(FASTCLEAR_NEON_OPTIMIZE "Replace CGsGraphics::FastClearNonClipping with a verified-faithful reimplementation that collapses per-row memset into one contiguous call when possible (experimental, not yet measured on hardware)" OFF)
if (FASTCLEAR_NEON_OPTIMIZE)
  add_definitions(-DZENONIA_FASTCLEAR_OPTIMIZE=1)
endif()
if (FASTCLEAR_NEON_OPTIMIZE AND INSTRUMENT_CGSGRAPHICS_CALLS)
  message(FATAL_ERROR "FASTCLEAR_NEON_OPTIMIZE e INSTRUMENT_CGSGRAPHICS_CALLS no pueden estar ON a la vez -- ambos instalan un hook sobre el mismo simbolo (CGsGraphics::FastClearNonClipping)")
endif()

## Fase 43: Reemplaza las rutinas de RLE de sprites (DrawOP_COPY_Compress_16 y ENLARGE)
## en `SetZeroBlendFunc` por las versiones decompiladas en C de Ghidra, pero compiladas
## para ARMv7 con optimizaciones modernas (-O3, NEON) en vez del viejo ARMv5 del motor.
option(SPRITE_DECODE_OPTIMIZE "Replace ARMv5 RLE assembly with Vita native optimized C code for drawing sprites" OFF)
if (SPRITE_DECODE_OPTIMIZE)
  add_definitions(-DZENONIA_SPRITE_DECODE_OPTIMIZE=1)
endif()


## optimization_ideas.md #3 (Fase 41): subidas asincronas del framebuffer
## RGB565 via PBO (glGenBuffers/glMapBuffer/glUnmapBuffer) en vez de pasar el
## puntero de pixeles directo a glTexSubImage2D -- solo tiene sentido junto
## con RGB565_DIRECT_UPLOAD. RIESGO REAL Y CONOCIDO (ver Fase 36): sin el
## codigo fuente de vitaGL no se pudo confirmar que su glTexSubImage2D
## realmente honre un GL_PIXEL_UNPACK_BUFFER en vez de tratar el argumento
## `pixels=NULL` como un puntero real -- si NO lo honra, esto es un NULL
## deref garantizado (crash). Es exactamente el tipo de incertidumbre que
## antes se evitaba probar sin datos reales; ahora se implementa la tecnica
## correcta segun el contrato real de la API GL y se deja que la consola real
## de la respuesta (ver build.sh --pbo-test) en vez de seguir sin confirmar.
option(PBO_ASYNC_UPLOAD "Upload the RGB565 framebuffer through a pixel-unpack PBO (glMapBuffer/glUnmapBuffer) instead of passing the CPU pointer directly to glTexSubImage2D (experimental, REAL crash risk if vitaGL does not honor GL_PIXEL_UNPACK_BUFFER -- see CMakeLists.txt comment)" OFF)
if (PBO_ASYNC_UPLOAD)
  add_definitions(-DZENONIA_PBO_UPLOAD=1)
endif()

## optimization_ideas.md #10 (Fase 41), reinterpretado: el rasterizador de
## software del motor es opaco (funciones "Zero" resueltas por puntero en
## tiempo de ejecucion, ver g_funcZeroCopy/g_funcEnlarge en out_ghidra.c) --
## parchearlo para dibujar filas pares/impares alternadas, como pedia la idea
## original, no es seguro sin una ronda de reversing que todavia no se hizo.
## En cambio, esta version aplica el mismo concepto (mitad del ancho de banda
## por diseno, no solo "si cambio poco" como DIRTY_RECT_UPLOAD) en la capa de
## SUBIDA del framebuffer: alterna que filas (pares/impares) se suben a la
## textura cada frame via glTexSubImage2D por fila -- mas llamadas GL que el
## build normal (fila por fila en vez de un solo bloque), asi que el
## resultado neto en este hardware es una pregunta real, no asumida. Solo
## tiene sentido junto con RGB565_DIRECT_UPLOAD. Ver build.sh
## --interlaced-test.
option(INTERLACED_UPLOAD "Upload only even/odd framebuffer rows per frame (alternating), halving upload bytes by design at the cost of many more glTexSubImage2D calls -- experimental, not yet measured (only active together with RGB565_DIRECT_UPLOAD)" OFF)
if (INTERLACED_UPLOAD)
  add_definitions(-DZENONIA_INTERLACED_UPLOAD=1)
endif()
if (INTERLACED_UPLOAD AND DIRTY_RECT_UPLOAD)
  message(FATAL_ERROR "INTERLACED_UPLOAD y DIRTY_RECT_UPLOAD no pueden estar ON a la vez -- ambos son estrategias distintas para el mismo #elif en glTexSubImage2D_wrapper (loader/dynlib.c loader/sprite_decode.c), solo una puede aplicarse por build")
endif()

## Variante 1 (--skip-blit-test, 2026-07-19): experimento de TECHO. Convierte
## PutCompressImg (el blitter de sprites del hot path) en un no-op -> los
## sprites no se dibujan, pero se mide el FPS maximo teorico si el costo del
## blit por software se eliminara. Decide si vale la pena el offload a GPU
## (Variante 2). NO es jugable. Ver loader/dynlib.c (PutCompressImg_hook).
option(SKIP_SPRITE_BLIT "Make PutCompressImg a no-op to measure the FPS ceiling if the software sprite blit were removed (diagnostic, sprites vanish)" OFF)
if (SKIP_SPRITE_BLIT)
  add_definitions(-DZENONIA_SKIP_SPRITE_BLIT=1)
endif()

## Variante 2 (--gpu-quads-test, 2026-07-19): offload de sprites COPY del hot
## path a quads texturizados en GPU (vitaGL) con cache de decode reusando el
## decoder real del motor. Idea #1+#5 de optimization_ideas.md. Ver
## loader/gpu_quads.c. EXPERIMENTAL: compositing hibrido (posibles problemas de
## capa), varios supuestos del motor sin verificar en hardware.
option(GPU_QUADS "Offload COPY sprites to GPU textured quads via vitaGL with a decode cache (experimental, hybrid compositing)" OFF)
if (GPU_QUADS)
  add_definitions(-DZENONIA_GPU_QUADS=1)
endif()

## Variante 3 (--downsample-test, 2026-07-19): resolucion INTERNA reducida
## (GAME_W/H = 960x544 * DS_NUM/DS_DEN) con upscale por GPU. AVISO: este motor
## fusiona FOV y resolucion (verificado en out_ghidra.c), asi que ESTO ZOOMEA
## la vista -- reproduce, a un ratio suave, lo que el usuario ya vio. Ver
## loader/main.c (macros GAME_W/H). DS_NUM/DS_DEN configurables.
option(DOWNSAMPLE_RENDER "Reduce internal render resolution by DS_NUM/DS_DEN and GPU-upscale (WARNING: zooms the view -- the engine fuses FOV and resolution)" OFF)
set(DS_NUM "2" CACHE STRING "Downsample numerator (internal res = 960x544 * DS_NUM/DS_DEN)")
set(DS_DEN "3" CACHE STRING "Downsample denominator")
if (DOWNSAMPLE_RENDER)
  add_definitions(-DZENONIA_DOWNSAMPLE=1 -DZENONIA_DS_NUM=${DS_NUM} -DZENONIA_DS_DEN=${DS_DEN})
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
# El toolchain vitasdk actual usa GCC 15.2, que (desde GCC 14) promueve estas
# categorias a ERROR por defecto. El codigo vendorizado (loader/so_util.c
# etc.) fue escrito para un GCC mas viejo donde eran solo warnings y compilaba
# limpio. Las bajamos de vuelta a warnings para no tener que parchear codigo
# vendorizado por una diferencia de version del compilador (no son bugs reales).
# OJO: "-Wno-error=return-mismatch" NO es una opcion valida de este GCC (cc1
# la rechaza con "no option '-Wreturn-mismatch'" -- nunca existio con ese
# nombre en el front-end de C, se confunde con el "-Wargument-mismatch" de
# Fortran) -- quitada; los 4 flags restantes ya cubren los casos reales de
# GCC 14+ (incluidos los mismatches de tipo de retorno via puntero).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration -Wno-error=implicit-int")
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
  ./loader/dynlib.c loader/sprite_decode.c
  ./loader/gpu_quads.c
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
  # La vitaGL/vitashark instaladas por vdpm (2026, gcc 15.2) estan compiladas
  # con C++ y usan std::string, operator new/delete y excepciones -> hace falta
  # la runtime real de C++. Va al final (resolucion izq->der del linker).
  stdc++
  supc++
  # dynlib.c define sus propios stubs no-op de 3 simbolos del ABI de C++
  # (__cxa_begin_cleanup/__cxa_call_unexpected/__cxa_type_match) para el .so
  # Android; libstdc++ tambien los define -> "multiple definition". Dejamos que
  # el linker tome el primero (los del loader) en vez de fallar; el resto del
  # ABI de excepciones lo aporta libstdc++.
  -Wl,--allow-multiple-definition
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
