#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <pthread.h>

#include <vitaGL.h>
#include <arm_neon.h>
#include "so_util.h"
#include "neon_mem.h"
#include "sprite_decode.h"
#ifdef ZENONIA_GPU_QUADS
#include "gpu_quads.h"
#endif

extern void game_log(const char *fmt, ...);
extern volatile int g_ui_status;

// Fase 27 (port_progress.md): cuenta cuantas veces por NativeRender() se sube
// el framebuffer de software RGB565 del motor (glTexImage2D/glTexSubImage2D
// con format=GL_RGB, type=GL_UNSIGNED_SHORT_5_6_5) -- confirma o descarta la
// sospecha ya anotada junto a convert_rgb565_to_rgba8888 ("el motor sube el
// framebuffer completo antes de CADA quad, varias veces por frame"). El loop
// principal (main.c) lo lee y resetea a 0 despues de cada NativeRender().
volatile unsigned int g_rgb565_uploads_this_frame = 0;

// Fase 36 (port_progress.md), optimization_ideas.md #2: filas realmente
// subidas vs. filas que se hubieran subido sin el diffing de
// DIRTY_RECT_UPLOAD (ver upload_rgb565_dirty_rect mas abajo) -- 0/0 siempre
// que el flag este OFF (el build normal no llama al helper que los toca).
volatile unsigned int g_dirtyrect_rows_uploaded_this_frame = 0;
volatile unsigned int g_dirtyrect_rows_total_this_frame = 0;

// Fase 41 (port_progress.md), optimization_ideas.md #10 (reinterpretado --
// ver upload_rgb565_interlaced mas abajo): igual criterio que los contadores
// de dirty-rect de arriba, 0/0 siempre que INTERLACED_UPLOAD este OFF.
volatile unsigned int g_interlaced_rows_uploaded_this_frame = 0;
volatile unsigned int g_interlaced_rows_total_this_frame = 0;

// Este helper (y el log SIN TOPE de glColorPointer mientras ui_status
// estuviera en 0-2) era instrumentacion puntual de una investigacion de
// Zenonia 3 (un bug de fade de alpha en el logo/titulo) -- nunca confirmada
// como aplicable a Zenonia 4, y confirmada como un problema real de
// PERFORMANCE aca: cada llamada a glColorPointer durante el logo/titulo/menu
// (potencialmente varias por frame) hacia un game_log() con fflush
// bloqueante sin ningun tope, agregando lentitud real justo en las primeras
// pantallas que ve el usuario (ver port_progress.md Fase 14). Removido --
// todo el logging de GL vuelve a estar topado a las primeras ~20 llamadas
// como el resto de los wrappers de este archivo.

// --- Ocultar cruceta + botones nativos (GVUIPlayerController) ---
//
// Primer intento (por textura, ver git log) NO funcionaba: confirmado con un
// log real que Dpad.pzx sube sus graficos a un atlas GL COMPARTIDO via
// glTexSubImage2D (nunca un glTexImage2D nuevo despues de su readAssets), asi
// que "etiquetar la proxima textura subida" nunca etiquetaba nada -- la
// cruceta y los botones seguian visibles siempre pese al toggle.
//
// Solucion real, mas simple y sin tocar nada de GL: GVUIController::Draw()
// (out_ghidra.c:26330, simetrico con PointerPress/Move/Release un poco mas
// abajo en el mismo archivo) es un simple `for (i in 0..*(this+0x19c))
// { dibujar hijo[i] }` -- si ese contador de "objetos activos" es 0, el
// controller no dibuja NI recibe touch de ninguno de sus hijos, sin afectar a
// ningun otro GVUIController (barra de vida/mana, minimapa, etc., cada uno
// con su propia instancia y su propio contador). GVUIPlayerController (la
// cruceta + hasta 5 botones de accion, confirmado por
// GVUIPlayerController::InitialPlayerPadSet(), out_ghidra.c:28466) es la
// UNICA instancia de esa clase en todo el juego -- alcanza con hookear su
// constructor (_ZN20GVUIPlayerControllerC2Ev), dejar que corra normal (arma
// los 5 objetos igual que siempre) y despues poner ESE contador en 0. No hace
// falta re-hookear Draw() para todos los demas controllers ni tocar el estado
// de GL en absoluto -- el touch fisico de la Vita ya cubre lo mismo via
// btn_map en main.c, asi que perder el hit-test de estos botones puntuales no
// quita ninguna funcionalidad real.
//
// Reversible sin tocar este archivo: recompilar con
// `-DHIDE_VIRTUAL_GAMEPAD=OFF` (ver CMakeLists.txt) deja este hook sin
// instalar (zenonia_install_hide_dpad_hook() se vuelve un no-op).
#ifdef ZENONIA_HIDE_DPAD_UI
static so_hook g_player_controller_callback2_hook;

// offset confirmado en GVUIController::GVUIController() (out_ghidra.c:42625,
// `*(int*)(this+0x19c)=0;`) y reusado identico por
// Draw()/PointerPress()/PointerMove()/PointerRelease() (el override de
// GVUIPlayerController::Draw() incluido) como limite del loop sobre los
// punteros a hijo que arrancan en this+8 -- confirmado idéntico en el .so de
// Zenonia 4 (mismo offset que Zenonia 3).
#define GVUI_CONTROLLER_ACTIVE_COUNT_OFFSET 0x19c

// Hookeando GVUIPlayerController::CallBack2(GVUIController*), NO el
// constructor -- a diferencia de Zenonia 3 (donde el constructor corria
// directo durante el arranque normal del HUD), en Zenonia 4
// GVUIPlayerController se crea de forma PEREZOSA como singleton
// (`GVUIMainSystem::pInstance`) recien la primera vez que se llama a
// CallBack2 (out_ghidra.c:44634-44648) -- confirmado en consola real: el
// hook original sobre el constructor JAMAS disparo en 7 sesiones de juego
// reales (nunca aparecio su log de confirmacion), pese a que el usuario veia
// la cruceta/botones en pantalla. CallBack2 es ademas la funcion que hace
// `ShowBtn(this, 10)` (los vuelve visibles) -- hookearla aca es el punto
// donde se garantiza que el objeto ya existe Y que se lo esta por mostrar.
// Sin auto-desinstalar (a diferencia del hook viejo del constructor): no hay
// garantia de que CallBack2 se llame una sola vez en toda la sesion (podria
// ser el callback de "volver a modo normal" del sistema de personalizacion
// de controles), asi que se re-aplica el apagado del contador cada vez que
// se invoca.
static void GVUIPlayerController_callback2_hook(void *param_1) {
    so_unhook(&g_player_controller_callback2_hook);
    ((void (*)(void *)) g_player_controller_callback2_hook.thumb_addr)(param_1);
    g_player_controller_callback2_hook = hook_addr(
        g_player_controller_callback2_hook.thumb_addr ? g_player_controller_callback2_hook.thumb_addr : g_player_controller_callback2_hook.addr,
        (uintptr_t) GVUIPlayerController_callback2_hook);
    *(int *)((char *) param_1 + GVUI_CONTROLLER_ACTIVE_COUNT_OFFSET) = 0;
    static int logged = 0;
    if (logged < 5) {
        game_log("[HideDpad] CallBack2 en %p -- contador de objetos activos puesto a 0 (Draw/touch de la cruceta y los 5 botones de accion deshabilitados)\n", param_1);
        logged++;
    }
}

// --- CZnTouchKeypad::Draw() -- la cruceta/botones REALES que se ven en juego ---
//
// `GVUIPlayerController` (arriba) resulto ser un sistema de personalizacion
// de controles OPCIONAL: confirmado con Ghidra que `GVUIPlayerController::
// CallBack2` solo se registra como callback de un boton de opciones
// (`*(code**)(iVar3+0x48) = CallBack2+1`, out_ghidra.c:44429/44448) -- nunca
// se llama durante el arranque normal, consistente con que el hook sobre esa
// funcion tampoco resolvio el problema en consola real.
//
// La cruceta/botones que en verdad se dibujan siempre en juego son de
// `CZnTouchKeypad` (prefijo "CZn" = especifico de Zenonia, no de la libreria
// generica "GVUI" de Gamevil) -- un singleton (`CGsSingleton<CZnTouchKeypad>
// ::ms_pSingleton`) referenciado por decenas de call sites de gameplay real
// (`ChangeKeypadMode`, etc., ver out_ghidra.c). Su `Draw()`
// (out_ghidra.c:191091) es simple y sin efectos secundarios de estado (solo
// chequea si hay un dialogo/menu tapando la pantalla y si hay touch
// disponible, y si ambos pasan dibuja el patron) -- hookearlo como no-op
// TOTAL (nunca llamar al original) es seguro y no depende de en que momento
// se construye el singleton, a diferencia del enfoque de
// GVUIPlayerController (que fallo dos veces por problemas de timing de
// construccion).
static so_hook g_touch_keypad_draw_hook;
static void CZnTouchKeypad_Draw_noop(void) {
    (void) g_touch_keypad_draw_hook; // referenciado solo para conservar el hook instalado
}
#endif

// Llamar DESPUES de so_relocate/so_resolve (necesita el modulo ya con
// dynsym/dynstr resueltos, ver so_symbol()) y ANTES de la primera llamada a
// cualquier Native*() del juego.
void zenonia_install_hide_dpad_hook(so_module *mod) {
#ifdef ZENONIA_HIDE_DPAD_UI
    uintptr_t cb2_addr = so_symbol(mod, "_ZN20GVUIPlayerController9CallBack2EP14GVUIController");
    if (!cb2_addr) {
        game_log("[HideDpad] simbolo _ZN20GVUIPlayerController9CallBack2EP14GVUIController no encontrado -- cruceta/botones NO se ocultan\n");
        return;
    }
    g_player_controller_callback2_hook = hook_addr(cb2_addr, (uintptr_t) GVUIPlayerController_callback2_hook);
    so_flush_caches(mod);
    game_log("[HideDpad] hook instalado en GVUIPlayerController::CallBack2 (0x%08x)\n", (unsigned int) cb2_addr);

    uintptr_t keypad_draw_addr = so_symbol(mod, "_ZN14CZnTouchKeypad4DrawEv");
    if (!keypad_draw_addr) {
        game_log("[HideDpad] simbolo _ZN14CZnTouchKeypad4DrawEv no encontrado -- cruceta/botones de juego NO se ocultan\n");
        return;
    }
    g_touch_keypad_draw_hook = hook_addr(keypad_draw_addr, (uintptr_t) CZnTouchKeypad_Draw_noop);
    so_flush_caches(mod);
    game_log("[HideDpad] hook instalado en CZnTouchKeypad::Draw (0x%08x) -- no-op permanente\n", (unsigned int) keypad_draw_addr);
#else
    (void) mod;
#endif
}

// --- DrawNative() call-count probe (Fase 29, diagnostico, ver CMakeLists.txt
// INSTRUMENT_DRAWNATIVE_CALLS) ---
//
// Investigacion de Ghidra (port_progress.md Fase 24 vs Fase 29): la Fase 24
// penso que DrawNative (_Z10DrawNativeliiiiii10enumDrawOPih, confirmado con
// `nm -D` en libgameDSO.so) solo tenia 4 call sites (minimapa/sombra
// circular/efectos de pantalla) porque busco nombres mangleados con grep
// plano -- eso no ve despacho por vtable. La cadena real de dibujo de
// sprites/personajes/animaciones .pzx es:
//   CMvCharacter::DrawCharacter -> CGxPZxAni::Draw (45 call sites reales de
//   gameplay) -> puntero a funcion cargado del vtable-like struct del frame
//   (this+0x10) -> CGxPZxBitmap::Blt -> CGcxBitmap::Blt -> PutNativeImage ->
//   DrawNative -- invisible a un grep de nombres porque el ultimo salto es
//   indirecto (puntero a funcion), no una llamada con nombre.
//
// DrawNative YA tiene logica de escala incorporada (rama `param_8 ==
// (enumDrawOP)0x10`, ver out_ghidra.c:~226057) que escala tanto el rect
// destino como el paso de la fuente DESPUES del clipping contra
// GcxGetMainScreenBuffer() -- si esto es realmente el hot path, es un solo
// punto de parche con la matematica de escala ya resuelta por el desarrollo
// original, no ~20 primitivas de CGsGraphics:: para re-derivar a mano (Fase
// 24). Antes de tocar nada, confirmar en consola real cuantas veces por
// frame se llama de verdad -- mismo criterio de "medir antes de parchear" que
// RGB565 directo (Fase 22-28).
//
// Hook de SOLO CONTEO: llama siempre a la funcion original sin cambiar nada
// de su comportamiento (unhook -> llamar directo -> re-hookear, mismo patron
// que GVUIPlayerController_callback2_hook mas arriba). Esto tiene costo real
// por invocacion (el ciclo unhook/rehook hace flush de i-cache dos veces) --
// deliberadamente detras de un flag de build de diagnostico aparte, nunca en
// el build normal: el FPS que se mida con esto activo va a ser mas lento de
// lo real, lo unico que importa de esa sesion es avg_drawnative_calls_per_frame.
volatile unsigned int g_drawnative_calls_this_frame = 0;

#ifdef ZENONIA_INSTRUMENT_DRAWNATIVE
static so_hook g_drawnative_hook;

static void DrawNative_counter_hook(long a, int b, int c, int d, int e, int f, int g, int op, int alpha, unsigned char ck) {
    g_drawnative_calls_this_frame++;
    so_unhook(&g_drawnative_hook);
    ((void (*)(long, int, int, int, int, int, int, int, int, unsigned char))
        (g_drawnative_hook.thumb_addr ? g_drawnative_hook.thumb_addr : g_drawnative_hook.addr))
        (a, b, c, d, e, f, g, op, alpha, ck);
    g_drawnative_hook = hook_addr(
        g_drawnative_hook.thumb_addr ? g_drawnative_hook.thumb_addr : g_drawnative_hook.addr,
        (uintptr_t) DrawNative_counter_hook);
}
#endif

// Llamar DESPUES de so_relocate/so_resolve, igual que
// zenonia_install_hide_dpad_hook.
void zenonia_install_drawnative_probe(so_module *mod) {
#ifdef ZENONIA_INSTRUMENT_DRAWNATIVE
    uintptr_t addr = so_symbol(mod, "_Z10DrawNativeliiiiii10enumDrawOPih");
    if (!addr) {
        game_log("[DrawNativeProbe] simbolo DrawNative no encontrado -- sin conteo\n");
        return;
    }
    g_drawnative_hook = hook_addr(addr, (uintptr_t) DrawNative_counter_hook);
    so_flush_caches(mod);
    game_log("[DrawNativeProbe] hook de conteo instalado en DrawNative (0x%08x)\n", (unsigned int) addr);
#else
    (void) mod;
#endif
}

// --- Fase 31 (port_progress.md): triangulacion de 3 candidatos simultaneos
// para el hot path real de sprites, ver CMakeLists.txt INSTRUMENT_HOTPATH_CALLS ---
//
// La Fase 29/30 penso que CGcxBitmap::Blt/DrawNative era el candidato real
// (via CGxPZxAni::Draw -> despacho por vtable) pero el conteo real en consola
// dio ~0-1 llamadas por frame -- casi nunca se alcanza. Investigacion de
// seguimiento encontro el sibling real: CGxPZxZero (misma familia de clases
// "PZx" = formato de asset .pzx, "Zero" = convencion de color-key de
// transparencia), cuyo Blt/BltFast SI son implementaciones reales (a
// diferencia de CGcxBitmap::BltFast, que es un stub muerto), y ambas terminan
// en PutCompressImg -- el despachador real por enumDrawOP hacia ~80 blitters
// DrawOP_<MODO>_Compress_16* (confirmados via g_funcZero*/SetBlendFunc en
// CMvGraphics::InitialBlend).
//
// En vez de apostar a un solo candidato como en las Fases 29/30 (cada intento
// fallido cuesta una ronda completa de consola), se instrumentan los 3
// niveles a la vez:
//   CGxPZxAni::Draw   -- punto de entrada garantizado: dispara SIEMPRE antes
//                        de cualquier despacho por vtable, a diferencia de
//                        CGcxBitmap::Blt/DrawNative (Fase 30). Si esto sale
//                        alto (decenas/frame), confirma que el motor SI
//                        dibuja muchos sprites via esta clase por frame.
//   CGxPZxZero::Blt   -- si el conteo aca es ~igual al de CGxPZxAni::Draw,
//                        confirma que el slot de vtable resuelve a esta clase
//                        en el caso comun (la pregunta que quedo abierta
//                        tras la Fase 30).
//   PutCompressImg    -- si este conteo es MUCHO mayor que CGxPZxZero::Blt,
//                        hay otros callers ademas de CGxPZxZero (posible
//                        ruta de tiles/mapa separada, no encontrada todavia
//                        por busqueda estatica de clases).
//
// Simbolos confirmados con `nm -D` sobre lib/libgameDSO.so (el .so real que
// carga este port) y firmas confirmadas leyendo el cuerpo decompilado en
// out_ghidra.c directamente (no solo el comentario de firma que genera
// Ghidra) antes de escribir estos wrappers:
//   CGxPZxAni::Draw   _ZN9CGxPZxAni4DrawEii10enumDrawOPil    (0x00159b10)
//                     void CGxPZxAni::Draw(CGxPZxAni*,int,int,enumDrawOP,int,long)
//   CGxPZxZero::Blt   _ZN10CGxPZxZero3BltEiiiiii10enumDrawOPil (0x00161e10)
//                     undefined4 CGxPZxZero::Blt(CGxPZxZero*,int,int,int,int,int,int,enumDrawOP,int,long)
//   PutCompressImg    _Z14PutCompressImgiiiiPhPt10enumDrawOPil (0x001509f8)
//                     void PutCompressImg(int,int,int,int,uchar*,ushort*,enumDrawOP,int,long)
volatile unsigned int g_cgxpzxani_draw_calls_this_frame = 0;
volatile unsigned int g_cgxpzxzero_blt_calls_this_frame = 0;
volatile unsigned int g_putcompressimg_calls_this_frame = 0;

// Fase 48 (port_progress.md): diagnostico siempre activo (no detras de
// INSTRUMENT_HOTPATH_CALLS, que casi nunca esta prendido) para responder,
// con datos reales del proximo log, la pregunta abierta tras restringir
// GPU_QUADS a op==0: es op==0 realmente el enumDrawOP dominante de sprites
// jugables (personajes/espada/tiles), o la mayoria usa otro valor y quedaron
// afuera del offload a GPU sin que hubiera regresion real (serian los mismos
// errores gráficos preexistentes, solo la escena de la sesion es distinta)?
volatile unsigned int g_gpu_quads_calls_this_frame = 0;
volatile unsigned int g_gpu_quads_op0_calls_this_frame = 0;
volatile unsigned int g_gpu_quads_cache_hit_this_frame = 0;
volatile unsigned int g_gpu_quads_decoded_this_frame = 0;

#ifdef ZENONIA_INSTRUMENT_HOTPATH
static so_hook g_cgxpzxani_draw_hook;
static so_hook g_cgxpzxzero_blt_hook;

static void CGxPZxAni_Draw_counter_hook(void *this_ptr, int p1, int p2, int op, int p4, long p5) {
    g_cgxpzxani_draw_calls_this_frame++;
    so_unhook(&g_cgxpzxani_draw_hook);
    ((void (*)(void *, int, int, int, int, long))
        (g_cgxpzxani_draw_hook.thumb_addr ? g_cgxpzxani_draw_hook.thumb_addr : g_cgxpzxani_draw_hook.addr))
        (this_ptr, p1, p2, op, p4, p5);
    g_cgxpzxani_draw_hook = hook_addr(
        g_cgxpzxani_draw_hook.thumb_addr ? g_cgxpzxani_draw_hook.thumb_addr : g_cgxpzxani_draw_hook.addr,
        (uintptr_t) CGxPZxAni_Draw_counter_hook);
}

static int CGxPZxZero_Blt_counter_hook(void *this_ptr, int p1, int p2, int p3, int p4, int p5, int p6, int op, int p8, long p9) {
    g_cgxpzxzero_blt_calls_this_frame++;
    so_unhook(&g_cgxpzxzero_blt_hook);
    int ret = ((int (*)(void *, int, int, int, int, int, int, int, int, long))
        (g_cgxpzxzero_blt_hook.thumb_addr ? g_cgxpzxzero_blt_hook.thumb_addr : g_cgxpzxzero_blt_hook.addr))
        (this_ptr, p1, p2, p3, p4, p5, p6, op, p8, p9);
    g_cgxpzxzero_blt_hook = hook_addr(
        g_cgxpzxzero_blt_hook.thumb_addr ? g_cgxpzxzero_blt_hook.thumb_addr : g_cgxpzxzero_blt_hook.addr,
        (uintptr_t) CGxPZxZero_Blt_counter_hook);
    return ret;
}
#endif

// --- Fase 32 (port_progress.md): guardia de crash en PutCompressImg ---
//
// Crash confirmado en consola real (log + .psp2dmp, metodologia so-crash-triage):
// Data abort con PC dentro de PutCompressImg+0x30, LR dentro de
// CGxPZxZero::Blt+0x41 (el retorno inmediato de la llamada a PutCompressImg
// dentro de Blt). Cruzando con out_ghidra.c:224597: el noveno parametro de
// PutCompressImg (`long param_9`, un puntero OPCIONAL a un framebuffer
// alternativo -- 0/NULL significa "usar GcxGetMainScreenBuffer()") se
// dereferencia sin mas chequeo que `== 0`:
//
//   if (param_9 == 0) { ... usa GcxGetMainScreenBuffer() ... }
//   else { local_2c = *(int *)(param_9 + 0x10); }  <- crash real, param_9=5
//
// Confirmado con el registro R2 (=fp=param_9) en 5 al momento del abort -- no
// es NULL, pero tampoco un puntero valido (una direccion de heap real en este
// loader siempre esta muy por encima de esto, ver LOAD_ADDRESS 0x98000000 en
// source/utils/init.c y las direcciones reales de datos vistas en el propio
// stack dump, todas en el rango 0x81xxxxxx/0x98xxxxxx/0x83xxxxxx). El chequeo
// `== 0` del motor no protege contra este caso -- mismo patron de bug ya
// documentado para este tipo de ports (un valor "no exactamente el sentinel
// esperado pero tampoco valido" que el motor no anticipa, ver la skill
// so-crash-triage).
//
// Verificado con el desensamblado real de CGxPZxAni::Draw (0x159b10) que el
// hook de conteo de la Fase 31 (arriba) NO puede ser la causa: el propio
// motor arma los argumentos de esta cadena de llamadas sin pasar por ningun
// codigo nuestro antes de llegar a Blt/PutCompressImg -- el valor invalido ya
// viene asi desde el motor real, esto es un bug preexistente (o del build
// original), no algo introducido por la instrumentacion.
//
// Fix: hook SIEMPRE activo (no detras de ningun flag de diagnostico, a
// diferencia de los dos de arriba) que sanea param_9 antes de dejar pasar la
// llamada -- si no es NULL pero esta por debajo de una direccion de heap
// plausible, se lo trata como NULL (usa el framebuffer principal, que es
// exactamente lo que el motor ya hace para el caso NULL legitimo).
#define ZENONIA_MIN_PLAUSIBLE_HEAP_ADDR 0x1000

static so_hook g_putcompressimg_hook;

static void PutCompressImg_hook(int p1, int p2, int p3, int p4, unsigned char *p5, unsigned short *p6, int op, int p8, long p9) {
#ifdef ZENONIA_INSTRUMENT_HOTPATH
    g_putcompressimg_calls_this_frame++;
#endif

#ifdef ZENONIA_GPU_QUADS
    g_gpu_quads_calls_this_frame++;
    // Fase 48: primera vez que se ve cada enumDrawOP real -- confirma o
    // descarta si op==0 es realmente el caso comun de sprites jugables.
    {
        static int seen_op[256];
        int op_idx = (op >= 0 && op < 256) ? op : 255;
        if (!seen_op[op_idx]) {
            seen_op[op_idx] = 1;
            game_log("[GpuQuadsDiag] enumDrawOP=%d visto por primera vez (p1=%d p2=%d p3=%d p4=%d p8=%d)\n",
                     op, p1, p2, p3, p4, p8);
        }
    }
#endif

#ifdef ZENONIA_SKIP_SPRITE_BLIT
    // Variante 1 (--skip-blit-test): experimento de TECHO. PutCompressImg es el
    // blitter real de sprites del motor (hot path confirmado: 445-547
    // llamadas/frame en combate, Fase 38). Retornar sin dibujar nada mide el
    // FPS maximo teorico si el costo del blit por software se eliminara por
    // completo (los sprites simplemente no aparecen). NO es un build jugable --
    // es la medicion que decide si vale la pena el offload a GPU (Variante 2):
    //   - FPS salta a 30-60  -> el blit ES el cuello, la GPU vale la pena.
    //   - FPS sigue en ~10   -> el cuello esta en otro lado (conversion/subida
    //                           del framebuffer, o primitivas CGsGraphics).
    // Se sigue contando (arriba) y no se llama al original. El hook queda
    // instalado (no hace falta unhook/rehook porque no delegamos).
    (void)p1; (void)p2; (void)p3; (void)p4; (void)p5; (void)p6; (void)op; (void)p8; (void)p9;
    return;
#endif

#ifdef ZENONIA_GPU_QUADS
    // Fase 50: TODOS los enumDrawOP se desvian a quads GPU (antes solo op==0,
    // Fase 48), dibujados encima del FB base en el orden de llamada original.
    // El decode reusa el blitter REAL del motor con el op REAL (nada de
    // adivinar formatos por op, la leccion de la Fase 48): decode dual sobre
    // fondo negro y blanco -> alpha por pixel exacto para cualquier operador
    // afin en dest, textura premultiplicada (ver gpu_quads.h). Solo se desvia
    // si p9==0 (destino = pantalla principal): si el motor esta componiendo
    // hacia un buffer alternativo propio, se respeta el camino de software.
    // Sprites que se salen del scratch (parcialmente fuera de pantalla, p.ej.
    // tiles en los bordes con la camara en movimiento) tambien caen al camino
    // de software: el motor los clippea y quedan en el FB base (debajo de los
    // quads) -- aceptable porque los tiles no se solapan entre si.
    if (p9 == 0) {
        if (op == 0) g_gpu_quads_op0_calls_this_frame++;
        if (gpu_quads_lookup_and_enqueue(p5, p6, op, p8, p1, p2, p3, p4)) {
            g_gpu_quads_cache_hit_this_frame++;
            return; // cache hit: quad encolado, no rasterizar por CPU
        }
        void *fake_fb = gpu_quads_begin_pass(p3, p4, 0x0000);
        if (!fake_fb) {
            // Fase 51 (2026-07-20): diagnostico para el reporte del usuario de
            // sprite del mapa/casas/bordes del mundo faltantes.
            // NOTA: Con la correccion de Fase 54 (decodificar en 0,0), esto solo
            // ocurre si el sprite es gigante (> GPU_QUADS_MAX_W).
            static int reject_log = 0;
            if (reject_log < 40) {
                game_log("[GpuQuadsDiag] fallback a software: op=%d p1=%d p2=%d p3=%d p4=%d p8=%d (fuera del scratch 960x544 o sprite > 1024x1024, ver gpu_quads.h)\n",
                         op, p1, p2, p3, p4, p8);
                reject_log++;
            }
        }
        if (fake_fb) {
            g_gpu_quads_decoded_this_frame++;
            int dual = (op != 2); // op 2 = additive: una sola pasada (gpu_quads.h)
            so_unhook(&g_putcompressimg_hook);
            void (*orig)(int, int, int, int, unsigned char *, unsigned short *, int, int, long) =
                (void (*)(int, int, int, int, unsigned char *, unsigned short *, int, int, long))
                (g_putcompressimg_hook.thumb_addr ? g_putcompressimg_hook.thumb_addr : g_putcompressimg_hook.addr);
            orig(0, 0, p3, p4, p5, p6, op, p8, (long) fake_fb); // pasada negra
            if (dual) {
                gpu_quads_capture_pass1(p3, p4);
                gpu_quads_begin_pass(p3, p4, 0xFFFF);
                orig(0, 0, p3, p4, p5, p6, op, p8, (long) fake_fb); // pasada blanca
            }
            g_putcompressimg_hook = hook_addr(
                g_putcompressimg_hook.thumb_addr ? g_putcompressimg_hook.thumb_addr : g_putcompressimg_hook.addr,
                (uintptr_t) PutCompressImg_hook);
            gpu_quads_finish_decode(p5, p6, op, p8, p1, p2, p3, p4, dual);
            return; // quad encolado
        }
    }
#endif

    if (p9 != 0 && (unsigned long) p9 < ZENONIA_MIN_PLAUSIBLE_HEAP_ADDR) {
        static int guard_log = 0;
        if (guard_log < 20) {
            game_log("[PutCompressImgGuard] param_9=0x%lx invalido (no NULL, no plausible) -- forzado a 0 (framebuffer principal), ver port_progress.md Fase 32\n", (unsigned long) p9);
            guard_log++;
        }
        p9 = 0;
    }
#ifdef ZENONIA_SPRITE_DECODE_CACHE
    // Fase 83: solo op==0 (COPY, mismo criterio conservador que GPU_QUADS
    // usaba antes de la Fase 50) -- ver sprite_decode.c para el resto del
    // mecanismo (consumido por DrawOP_COPY_Compress_16_Vita mas abajo en la
    // misma llamada sincronica).
    if (op == 0) {
        sprite_cache_set_pending_dims(p5, p3, p4);
    }
#endif
    so_unhook(&g_putcompressimg_hook);
    ((void (*)(int, int, int, int, unsigned char *, unsigned short *, int, int, long))
        (g_putcompressimg_hook.thumb_addr ? g_putcompressimg_hook.thumb_addr : g_putcompressimg_hook.addr))
        (p1, p2, p3, p4, p5, p6, op, p8, p9);
    g_putcompressimg_hook = hook_addr(
        g_putcompressimg_hook.thumb_addr ? g_putcompressimg_hook.thumb_addr : g_putcompressimg_hook.addr,
        (uintptr_t) PutCompressImg_hook);
}

// --- Fase 86 (port_progress.md): conteo de CGxFACharCache::DrawToFrameBuffer,
// INDEPENDIENTE de GPU_QUADS ---
//
// Motivado por un dato real de --frame-phases-probe (Fase 85): la pantalla de
// menu (ui_status=1, sin sprites .pzx de gameplay) ya costaba 87ms de
// NativeRender -- casi tanto como el gameplay real (149-152ms). Un menu es
// mayormente texto/UI, no sprites -- candidato directo:
// CGxFACharCache::DrawToFrameBuffer (confirmado en la Fase 59 que dibuja texto
// DIRECTO al framebuffer, un blend alpha por pixel manual, out_ghidra.c:9872-
// 10154), simbolo ya resuelto para el hook de GPU_QUADS de mas abajo pero
// nunca medido por SU COSTO -- solo se toco antes para arreglar el orden de
// capas visual. Contador de solo-conteo pass-through (mismo patron que
// zenonia_install_cgsgraphics_probe), SIN reemplazar el dibujo real -- a
// diferencia del hook de GPU_QUADS (mas abajo), que SI reemplaza el efecto
// completo por un quad. Mismo caveat que los demas *_probe: agrega overhead
// real por-llamada (unhook/llamar original/rehook por cada glifo dibujado),
// el FPS de este build NO es comparable al normal -- solo importa el contador.
volatile unsigned int g_drawtoframebuffer_calls_this_frame = 0;

#ifdef ZENONIA_INSTRUMENT_TEXT_CALLS
static so_hook g_drawtoframebuffer_counter_hook;
static int DrawToFrameBuffer_counter_hook(void *this_ptr, void *char_unit_info, int screen_x, int screen_y) {
    g_drawtoframebuffer_calls_this_frame++;
    so_unhook(&g_drawtoframebuffer_counter_hook);
    int ret = ((int (*)(void *, void *, int, int))
        (g_drawtoframebuffer_counter_hook.thumb_addr ? g_drawtoframebuffer_counter_hook.thumb_addr : g_drawtoframebuffer_counter_hook.addr))
        (this_ptr, char_unit_info, screen_x, screen_y);
    g_drawtoframebuffer_counter_hook = hook_addr(
        g_drawtoframebuffer_counter_hook.thumb_addr ? g_drawtoframebuffer_counter_hook.thumb_addr : g_drawtoframebuffer_counter_hook.addr,
        (uintptr_t) DrawToFrameBuffer_counter_hook);
    return ret;
}

void zenonia_install_text_calls_probe(so_module *mod) {
    uintptr_t addr = so_symbol(mod, "_ZN14CGxFACharCache17DrawToFrameBufferEPN4gxfa12CharUnitInfoEii");
    if (addr) {
        g_drawtoframebuffer_counter_hook = hook_addr(addr, (uintptr_t) DrawToFrameBuffer_counter_hook);
        so_flush_caches(mod);
        game_log("[TextCallsProbe] hook de conteo instalado en CGxFACharCache::DrawToFrameBuffer (0x%08x)\n", (unsigned int) addr);
    } else {
        game_log("[TextCallsProbe] error: no se encontro CGxFACharCache::DrawToFrameBuffer\n");
    }
}
#else
void zenonia_install_text_calls_probe(so_module *mod) { (void) mod; }
#endif

#ifdef ZENONIA_GPU_QUADS
// --- Fase 59 (port_progress.md): CGxFACharCache::DrawToFrameBuffer -> quads GPU ---
//
// Hallazgo clave (leyendo out_ghidra.c a fondo, no adivinando): TODO el texto
// del motor (tips, dialogos, descripciones, guardado, opciones, ayuda -- todo
// lo que NO es un sprite .pzx) se dibuja via CGxFACharCache::drawString ->
// DrawQueueChars -> DrawToFrameBuffer, un blend alpha-sobre-RGB565 manual que
// escribe DIRECTO al framebuffer de software (confirmado en
// out_ghidra.c:9872-10154) -- NUNCA pasa por PutCompressImg, a diferencia de
// lo que asumia el resumen de la Fase 50 ("el motor blitea los glifos el
// mismo -> pasan por PutCompressImg"). Esa asuncion nunca se verifico contra
// el .so real para este call site especifico.
//
// Por que esto oculta el texto bajo GPU_QUADS: la arquitectura de la Fase 50
// compone el framebuffer de software COMPLETO como una sola capa base, SIEMPRE
// debajo de TODOS los quads de sprites del frame (ver gpu_quads_flush,
// llamado una sola vez, justo despues del unico glDrawArrays que presenta esa
// capa). El texto queda atrapado en esa capa base -- cualquier sprite/panel
// desviado a GPU_QUADS que el motor dibuje DESPUES del texto en la MISMA
// llamada (p.ej. un panel de dialogo dibujado encima de texto que deberia
// quedar debajo, o cualquier sprite en la misma zona) termina tapandolo
// SIEMPRE, sin importar el orden real de llamadas -- porque los quads se
// dibujan en un solo batch al final, y la capa base (con el texto adentro)
// siempre va primero. Fix: sacar el texto de la capa base por completo y
// encolarlo como un quad mas, en la MISMA cola que los sprites
// (gpu_quads_enqueue_text) -- su posicion en la cola pasa a reflejar el
// momento real en que el motor lo dibujo, igual que un sprite.
//
// Formato de datos confirmado en out_ghidra.c:
//   - CharUnitInfo (param_1): +8 = puntero a bytes de cobertura 0-255 (el
//     char-cache SOLO guarda alfa, confirmado tambien en
//     CopyPixelsToCharCacheBuffer via el comentario de java.c), +0xc = ancho
//     del glifo (tambien el STRIDE del buffer de alfa, sin padding), +0x10 =
//     alto.
//   - this+0x48 = color de texto actual, pero NO como RGB565 crudo:
//     CGxFACharCache::setColor565 lo reempaqueta a R5/G6/B5 pre-posicionados
//     para el blend (confirmado leyendo setColor565 Y los shifts que
//     DrawToFrameBuffer aplica sobre este mismo campo -- son inversos exactos).
//   - El clip contra la ventana de texto activa (GxFont_GetCommonInfo(), +0x38
//     x, +0x3c y, +0x40 ancho, +0x44 alto) se replica tal cual (formula
//     literal de out_ghidra.c:9917-9943, incluida su asimetria entre
//     clip-izquierda y clip-derecha -- no se "corrigio" por las dudas, un
//     glifo individual practicamente nunca choca contra ambos bordes del
//     clip a la vez en el juego real).
//
// Se hookea DIRECTO (no via un puntero "framebuffer alternativo" como el p9
// de PutCompressImg -- DrawToFrameBuffer no tiene ese parametro, obtiene el
// destino via una llamada virtual interna) y NUNCA se llama al original: el
// quad reemplaza el efecto visual completo del blend, asi que la capa base de
// software queda limpia (solo primitivas CGsGraphics), consistente con la
// asuncion de diseno de la Fase 50.
static so_hook g_drawtoframebuffer_hook;
static uintptr_t g_gxfont_getcommoninfo_addr = 0;

static int DrawToFrameBuffer_hook(void *this_ptr, void *char_unit_info, int screen_x, int screen_y) {
    unsigned char *glyph_alpha = char_unit_info ? *(unsigned char **) ((char *) char_unit_info + 8) : NULL;
    int glyph_w = char_unit_info ? *(int *) ((char *) char_unit_info + 0xc) : 0;
    int glyph_h = char_unit_info ? *(int *) ((char *) char_unit_info + 0x10) : 0;

    if (glyph_alpha && glyph_w > 0 && glyph_h > 0 && g_gxfont_getcommoninfo_addr) {
        int (*get_common_info)(void) = (int (*)(void)) g_gxfont_getcommoninfo_addr;
        char *ci = (char *) (uintptr_t) get_common_info();
        if (ci) {
            int clip_x = *(int *) (ci + 0x38);
            int clip_y = *(int *) (ci + 0x3c);
            int clip_w = *(int *) (ci + 0x40);
            int clip_h = *(int *) (ci + 0x44);

            int x = screen_x, y = screen_y;
            int left_off = 0, top_off = 0, vis_w, vis_h;
            if (x < clip_x) { left_off = clip_x - x; x = clip_x; vis_w = glyph_w - left_off; }
            else { vis_w = glyph_w; }
            { int clip_right = clip_x + clip_w; if (clip_right < x + glyph_w) vis_w = clip_right - (x + left_off); }

            if (y < clip_y) { top_off = clip_y - y; y = clip_y; vis_h = glyph_h - top_off; }
            else { vis_h = glyph_h; }
            { int clip_bottom = clip_y + clip_h; if (clip_bottom < y + glyph_h) vis_h = clip_bottom - (y + top_off); }

            if (vis_h >= 1 && vis_w > 0) {
                uint32_t reg = *(uint32_t *) ((char *) this_ptr + 0x48);
                int r5 = (int) ((reg << 8) >> 27);
                int g6 = (int) ((reg << 16) >> 26);
                int b5 = (int) (reg & 0xff) >> 3;
                unsigned char r8 = (unsigned char) ((r5 << 3) | (r5 >> 2));
                unsigned char g8 = (unsigned char) ((g6 << 2) | (g6 >> 4));
                unsigned char b8 = (unsigned char) ((b5 << 3) | (b5 >> 2));
                const unsigned char *sub_alpha = glyph_alpha + (size_t) glyph_w * top_off + left_off;
                gpu_quads_enqueue_text(sub_alpha, glyph_w, r8, g8, b8, x, y, vis_w, vis_h);
            }
        }
    }
    return 1; // ningun call site real (DrawQueueChars/drawChar) usa el retorno
}

// Llamar junto con zenonia_install_hotpath_probe (mismo momento del boot).
void zenonia_install_text_quads_hook(so_module *mod) {
    g_gxfont_getcommoninfo_addr = so_symbol(mod, "GxFont_GetCommonInfo");
    uintptr_t draw_to_fb_addr = so_symbol(mod, "_ZN14CGxFACharCache17DrawToFrameBufferEPN4gxfa12CharUnitInfoEii");
    if (draw_to_fb_addr && g_gxfont_getcommoninfo_addr) {
        g_drawtoframebuffer_hook = hook_addr(draw_to_fb_addr, (uintptr_t) DrawToFrameBuffer_hook);
        game_log("[TextQuads] hook instalado en CGxFACharCache::DrawToFrameBuffer (0x%08x)\n", (unsigned int) draw_to_fb_addr);
    } else {
        game_log("[TextQuads] simbolo CGxFACharCache::DrawToFrameBuffer o GxFont_GetCommonInfo no encontrado -- el texto sigue con el bug de orden de capas\n");
    }
    so_flush_caches(mod);
}

// --- Fase 61 (port_progress.md): Screen_RGBBLEND16_Real -> quads GPU ---
//
// Segundo caso del MISMO bug de la Fase 59 (texto), esta vez para rellenos
// solidos con alpha: `CGsGraphics::DrawFillRect`/`DrawRoundRect`/
// `DrawFillEllipse`/etc. (out_ghidra.c, ~13 primitivas ya instrumentadas en
// la Fase 36) llaman a `RGBBlend16`/`g_funcRGBBlend16` para pintar un
// rectangulo de color+alpha DIRECTO al framebuffer de software -- es
// exactamente el mecanismo candidato para el fondo negro/traslucido de los
// cuadros de tips/guardado/objetos recogidos que el usuario reporto
// invisible tras la Fase 60 (el fix de esa fase, acotado por tamano de quad,
// no podia arreglar esto porque el problema real nunca fue glColorPointer --
// el log de la Fase 60 confirmo CERO usos de un color-array UByte chico
// durante toda la sesion, descartando esa hipotesis).
//
// `RGBBlend16` (simbolo real, `_Z10RGBBlend16iiiiji`) es un wrapper delgado
// que llama a traves de un puntero de funcion global (`g_funcRGBBlend16`,
// mismo patron de indireccion ya usado para `g_funcZeroCopy`/`g_funcEnlarge`
// en sprite_decode.c) -- pero la MAYORIA de los call sites reales del .so
// llaman a `g_funcRGBBlend16` DIRECTO, sin pasar por el wrapper (confirmado
// con >15 call sites via grep en out_ghidra.c), asi que hookear el wrapper
// solo capturaria una fraccion de los casos. En cambio se hookea
// `Screen_RGBBLEND16_Real` (`_Z22Screen_RGBBLEND16_Realiiiiji`) -- la funcion
// REAL a la que `g_funcRGBBlend16` apunta tras `CMvGraphics::InitialBlend()`
// (`SetScreenEffectFunc(1, Screen_RGBBLEND16_16+1, Screen_RGBBLEND16_Real+1)`,
// confirmado en out_ghidra.c) -- hookear la funcion final por DIRECCION
// intercepta TODOS los call sites sin importar si llegan via el wrapper o
// directo por el puntero, igual que PutCompressImg_hook.
//
// Formato de parametros confirmado en out_ghidra.c: `(x,y,w,h,color565,alpha4)`
// -- `color565` es RGB565 estandar (confirmado con las mascaras de bits de
// `Screen_RGBBLEND16_16`, p.ej. 0x8610 = bit MSB de cada canal en el orden
// R:15-11/G:10-5/B:4-0 exacto de RGB565), `alpha4` es 0-15 (4 bits, NO 0-255
// -- confirmado en `Screen_RGBBLEND16_Real`: `(uint)param_6 < 0x10`, y en
// `CGsGraphics::DrawFillRect`: `RGBBlend16(...,iVar2>>4)` donde iVar2 es el
// alpha real 0-255 de `CGcxGraphicContext::GetAlpha()`).
//
// `Screen_RGBBLEND16_Real` YA clippea (x,y,w,h) contra los limites del main
// screen buffer antes de escribir -- no hace falta replicar ese clip aca (a
// diferencia del clip de texto de la Fase 59, que es contra una ventana
// LOCAL de dialogo, no la pantalla): un quad GL que cae fuera del viewport
// simplemente no se rasteriza, mismo resultado.
static so_hook g_rgbblend16_hook;

static void Screen_RGBBLEND16_Real_hook(int x, int y, int w, int h, unsigned int color565, int alpha4) {
    if (w <= 0 || h <= 0) return;
    if (alpha4 < 0) alpha4 = 0;
    if (alpha4 > 15) alpha4 = 15;
    int alpha8 = (alpha4 * 255) / 15;

    int r5 = (color565 >> 11) & 0x1F, g6 = (color565 >> 5) & 0x3F, b5 = color565 & 0x1F;
    unsigned char r8 = (unsigned char) ((r5 << 3) | (r5 >> 2));
    unsigned char g8 = (unsigned char) ((g6 << 2) | (g6 >> 4));
    unsigned char b8 = (unsigned char) ((b5 << 3) | (b5 >> 2));

    // Fase 63/64 (diagnostico ya retirado, port_progress.md): confirmo que
    // este mecanismo SI se llama durante gameplay real -- entre otras cosas,
    // el minimapa se dibuja como CIENTOS de bloques individuales de 4x4/4x20
    // pixeles (un RGBBlend16 por punto de terreno/marcador), no como un solo
    // sprite/relleno -- volumen mucho mayor de lo esperado. El log del
    // diagnostico confirmo esto pero su propio costo (scan lineal de 128
    // entradas EN CADA llamada, para siempre, no solo las primeras N) se
    // sumaba al de convertir cada uno de esos cientos de bloques en un quad
    // GPU -- contribuyo al bajon de FPS reportado en un area nueva con mucho
    // detalle de minimapa. Retirado -- ya cumplio su proposito.
    gpu_quads_enqueue_fill(r8, g8, b8, (unsigned char) alpha8, x, y, w, h);
    // Nunca se llama al original -- el quad reemplaza el efecto visual
    // completo, la capa base de software queda limpia (mismo criterio que
    // DrawToFrameBuffer_hook, Fase 59).
}

// --- Fase 62 (port_progress.md): CGsGraphics::DrawNativeFillEllipse -> quads GPU ---
//
// Tercer caso del mismo bug de fondo (Fase 59 texto, Fase 61 RGBBlend16):
// `CGsGraphics::DrawFillRoundRect` (out_ghidra.c:24187), para radios de
// esquina > 7px, dibuja las 4 esquinas via `DrawNativeFillEllipse` (una por
// cuadrante, bitmask `param_6`=1/2/4/8) y SOLO el centro/cruz via
// `RGBBlend16` (ya cubierto por la Fase 61) -- si el cuadro de "objeto
// recogido" usa esquinas redondeadas grandes, sus 4 esquinas quedarian sin
// cubrir por el fix de la Fase 61 (candidato para "le falta el fondo negro
// transparente" reportado especificamente en ESE cuadro, a diferencia de
// tips/guardado que ya se ven bien).
//
// `DrawNativeFillEllipse` es COMPLETAMENTE autonoma -- rasteriza la elipse Y
// hace su propio blend alpha por pixel, escribiendo directo al framebuffer
// (`GcxGetMainScreenBuffer()`) sin pasar por `RGBBlend16` en absoluto (codigo
// de blend duplicado, no una llamada mas al mismo mecanismo). Formato de
// color CONFIRMADO en out_ghidra.c (dual, a diferencia de RGBBlend16): si
// `color < 0x10000` es RGB565 crudo con alpha implicito 0xFF (opaco); si no,
// es ARGB8888 real (`alpha=color>>24`, R/G/B en los bytes siguientes).
//
// LIMITACION CONOCIDA Y ACEPTADA (documentada, no oculta): el hook reproduce
// el rectangulo COMPLETO x,y,w,h como relleno solido, no la forma de arco
// exacta del cuadrante -- las esquinas quedan CUADRADAS en vez de
// redondeadas (unos pocos pixeles de diferencia visual, el radio tipico de
// una UI). Preferido sobre no hacer nada: pasar de "fondo completamente
// invisible" a "fondo visible con esquinas apenas cuadradas" es una mejora
// neta aunque no sea pixel-perfect: replicar el arco exacto necesitaria una
// textura con mascara de alfa por cuadrante, no solo un quad solido.
//
// SALVAGUARDA: `DrawFillEllipse` (circulos/ovalos genuinos, no esquinas de
// rounded-rect) llama a esta MISMA funcion con `quadrant=0xf` (los 4 bits a
// la vez, ver out_ghidra.c:24181) -- desviar ESE caso a un quad cuadrado
// SI seria una regresion real (un circulo/ovalo autentico de la UI, no una
// esquina chica, se veria como un cuadrado solido en vez de redondo). Solo se
// desvia cuando `quadrant` tiene EXACTAMENTE UN bit prendido (1/2/4/8 --
// el patron real de las 4 llamadas de esquina en DrawFillRoundRect); para
// cualquier otro valor (0xf u otra combinacion) se delega al original, sin
// cambiar el comportamiento actual (ni mejor ni peor que antes de este fix).
static so_hook g_drawnativefillellipse_hook;

static void DrawNativeFillEllipse_hook(void *this_ptr, int x, int y, int w, int h,
                                        unsigned int color, int quadrant) {
    int is_single_corner = quadrant != 0 && (quadrant & (quadrant - 1)) == 0;
    if (!is_single_corner) {
        so_unhook(&g_drawnativefillellipse_hook);
        ((void (*)(void *, int, int, int, int, unsigned int, int))
            (g_drawnativefillellipse_hook.thumb_addr ? g_drawnativefillellipse_hook.thumb_addr : g_drawnativefillellipse_hook.addr))
            (this_ptr, x, y, w, h, color, quadrant);
        g_drawnativefillellipse_hook = hook_addr(
            g_drawnativefillellipse_hook.thumb_addr ? g_drawnativefillellipse_hook.thumb_addr : g_drawnativefillellipse_hook.addr,
            (uintptr_t) DrawNativeFillEllipse_hook);
        return;
    }

    unsigned char r8, g8, b8, a8;
    if (color < 0x10000) {
        int r5 = (color >> 11) & 0x1F, g6 = (color >> 5) & 0x3F, b5 = color & 0x1F;
        r8 = (unsigned char) ((r5 << 3) | (r5 >> 2));
        g8 = (unsigned char) ((g6 << 2) | (g6 >> 4));
        b8 = (unsigned char) ((b5 << 3) | (b5 >> 2));
        a8 = 255;
    } else {
        a8 = (unsigned char) (color >> 24);
        r8 = (unsigned char) (color >> 16);
        g8 = (unsigned char) (color >> 8);
        b8 = (unsigned char) color;
    }
    // Fase 63/64: diagnostico ya retirado, ver el comentario en
    // Screen_RGBBLEND16_Real_hook -- cumplio su proposito, su propio costo
    // (scan lineal en cada llamada) contribuia al bajon de FPS.

    gpu_quads_enqueue_fill(r8, g8, b8, a8, x, y, w, h);
}

void zenonia_install_screen_fill_quads_hook(so_module *mod) {
    uintptr_t addr = so_symbol(mod, "_Z22Screen_RGBBLEND16_Realiiiiji");
    if (addr) {
        g_rgbblend16_hook = hook_addr(addr, (uintptr_t) Screen_RGBBLEND16_Real_hook);
        game_log("[FillQuads] hook instalado en Screen_RGBBLEND16_Real (0x%08x)\n", (unsigned int) addr);
    } else {
        game_log("[FillQuads] simbolo Screen_RGBBLEND16_Real no encontrado -- los rellenos solidos (fondos de dialogo, etc.) siguen con el bug de orden de capas\n");
    }

    uintptr_t ellipse_addr = so_symbol(mod, "_ZN11CGsGraphics21DrawNativeFillEllipseEiiiij17EnumGsDegreeCCW90");
    if (ellipse_addr) {
        g_drawnativefillellipse_hook = hook_addr(ellipse_addr, (uintptr_t) DrawNativeFillEllipse_hook);
        game_log("[FillQuads] hook instalado en DrawNativeFillEllipse (0x%08x)\n", (unsigned int) ellipse_addr);
    } else {
        game_log("[FillQuads] simbolo DrawNativeFillEllipse no encontrado -- esquinas redondeadas grandes siguen con el bug de orden de capas\n");
    }

    so_flush_caches(mod);
}
#endif // ZENONIA_GPU_QUADS

// Llamar DESPUES de so_relocate/so_resolve, igual que
// zenonia_install_hide_dpad_hook/zenonia_install_drawnative_probe. La
// guardia de PutCompressImg (Fase 32) se instala SIEMPRE, en todos los
// builds; los otros 2 hooks (Fase 31, solo conteo) siguen detras de
// INSTRUMENT_HOTPATH_CALLS.
void zenonia_install_hotpath_probe(so_module *mod) {
#ifdef ZENONIA_INSTRUMENT_HOTPATH
    uintptr_t ani_draw_addr = so_symbol(mod, "_ZN9CGxPZxAni4DrawEii10enumDrawOPil");
    if (ani_draw_addr) {
        g_cgxpzxani_draw_hook = hook_addr(ani_draw_addr, (uintptr_t) CGxPZxAni_Draw_counter_hook);
        game_log("[HotPathProbe] hook instalado en CGxPZxAni::Draw (0x%08x)\n", (unsigned int) ani_draw_addr);
    } else {
        game_log("[HotPathProbe] simbolo CGxPZxAni::Draw no encontrado\n");
    }

    uintptr_t zero_blt_addr = so_symbol(mod, "_ZN10CGxPZxZero3BltEiiiiii10enumDrawOPil");
    if (zero_blt_addr) {
        g_cgxpzxzero_blt_hook = hook_addr(zero_blt_addr, (uintptr_t) CGxPZxZero_Blt_counter_hook);
        game_log("[HotPathProbe] hook instalado en CGxPZxZero::Blt (0x%08x)\n", (unsigned int) zero_blt_addr);
    } else {
        game_log("[HotPathProbe] simbolo CGxPZxZero::Blt no encontrado\n");
    }
#endif

    uintptr_t putcompress_addr = so_symbol(mod, "_Z14PutCompressImgiiiiPhPt10enumDrawOPil");
    if (putcompress_addr) {
        g_putcompressimg_hook = hook_addr(putcompress_addr, (uintptr_t) PutCompressImg_hook);
        game_log("[PutCompressImgGuard] hook instalado en PutCompressImg (0x%08x)\n", (unsigned int) putcompress_addr);
    } else {
        game_log("[PutCompressImgGuard] simbolo PutCompressImg no encontrado -- guardia de crash de la Fase 32 NO esta activa\n");
    }

    so_flush_caches(mod);
}

// --- Fase 36 (port_progress.md): instrumentacion de CGsGraphics:: ---
// optimization_ideas.md, "Lo que NO se ha hecho" #3: antes de vectorizar a
// mano las ~20 primitivas de CGsGraphics:: (DrawLine, DrawRect, PutPixel,
// etc.), confirmar si alguna se llama con suficiente frecuencia por frame
// como para importar -- mismo criterio de "medir antes de parchear" que
// INSTRUMENT_HOTPATH_CALLS/INSTRUMENT_DRAWNATIVE_CALLS.
//
// A diferencia de DrawNative (Fase 29/30, resuelto por despacho de vtable
// invisible a `nm`), estas 13 SI son simbolos exportados directamente
// hookeables -- confirmados con `nm -D` sobre lib/libgameDSO.so real de este
// build (no un supuesto de una firma parecida en otro port):
//   DrawLine              _ZN11CGsGraphics8DrawLineEiiiij
//   DrawFillRect          _ZN11CGsGraphics12DrawFillRectEiiiij
//   DrawRoundRect         _ZN11CGsGraphics13DrawRoundRectEiiiiij
//   DrawFillRoundRect     _ZN11CGsGraphics17DrawFillRoundRectEiiiiij
//   DrawEllipse (4 arg)   _ZN11CGsGraphics11DrawEllipseEiiij
//   DrawEllipse (7 arg)   _ZN11CGsGraphics11DrawEllipseEiiiijii
//   DrawFillEllipse       _ZN11CGsGraphics15DrawFillEllipseEiiiijii
//   ClearScreen           _ZN11CGsGraphics11ClearScreenEj
//   BeginScreen           _ZN11CGsGraphics11BeginScreenEj
//   FastClearNonClipping  _ZN11CGsGraphics20FastClearNonClippingEiiii
//   GetFrameBufferPtr     _ZN11CGsGraphics17GetFrameBufferPtrEii
//   PutPixel (2 arg)      _ZN11CGsGraphics8PutPixelEii
//   PutPixel (3 arg)      _ZN11CGsGraphics8PutPixelEiij
// Todos los parametros son escalares (int/unsigned int) -- sin structs por
// valor (a diferencia de DrawTextInRect/TGXRECT, dejadas afuera a proposito:
// sin el layout real de TGXRECT confirmado, hookear esa firma arriesgaria
// desalinear el paso de argumentos ARM AAPCS). Cada hook devuelve `int`
// (forward de r0 tal cual, sin interpretarlo) en vez de asumir cada firma
// real void/no-void -- un caller que en verdad espera void simplemente no
// lee r0, asi que forwardear de mas es inofensivo; asumir void de menos en
// una funcion que si devuelve algo si seria un bug real.
//
// Contador agregado (para el log de siempre, avg_cgsgraphics_per_frame) MAS
// un desglose por primitiva -- Fase 41/42 (port_progress.md) ya habian notado
// que el agregado solo no alcanza para saber cual de las ~13 domina (p.ej.
// --primitives-opt-test dio pantalla negra sin poder saber si el tiempo
// perdido era realmente en FastClearNonClipping/RGBBlend16 o en otra cosa).
// Array indexado por CGSGFX_* en vez de 13 variables sueltas para no repetir
// el mismo bloque de acumulacion/reset 13 veces en main.c.
volatile unsigned int g_cgsgraphics_calls_this_frame = 0;

enum {
    CGSGFX_DRAWLINE = 0,
    CGSGFX_DRAWFILLRECT,
    CGSGFX_DRAWROUNDRECT,
    CGSGFX_DRAWFILLROUNDRECT,
    CGSGFX_DRAWELLIPSE4,
    CGSGFX_DRAWELLIPSE7,
    CGSGFX_DRAWFILLELLIPSE,
    CGSGFX_CLEARSCREEN,
    CGSGFX_BEGINSCREEN,
    CGSGFX_FASTCLEARNONCLIPPING,
    CGSGFX_GETFRAMEBUFFERPTR,
    CGSGFX_PUTPIXEL2,
    CGSGFX_PUTPIXEL3,
    CGSGFX_COUNT
};
static volatile unsigned int g_cgsgraphics_calls_by_kind[CGSGFX_COUNT] = {0};

// Llamada cada 120 frames desde main.c, mismo checkpoint que el resto de los
// avg_*_per_frame -- imprime el promedio por-frame de CADA primitiva en una
// sola linea y resetea el desglose (el agregado avg_cgsgraphics_per_frame
// sigue su propio accum/reset en main.c, sin tocar). 0.0 en todas siempre que
// INSTRUMENT_CGSGRAPHICS_CALLS este OFF, ya que ningun hook incrementa
// g_cgsgraphics_calls_by_kind en ese caso.
void zenonia_log_cgsgraphics_breakdown(unsigned int window_frames) {
    if (window_frames == 0) return;
    game_log("[CGsGraphicsProbe] breakdown avg/frame (ultimos %u): "
             "DrawLine=%.1f DrawFillRect=%.1f DrawRoundRect=%.1f DrawFillRoundRect=%.1f "
             "DrawEllipse4=%.1f DrawEllipse7=%.1f DrawFillEllipse=%.1f ClearScreen=%.1f "
             "BeginScreen=%.1f FastClearNonClipping=%.1f GetFrameBufferPtr=%.1f "
             "PutPixel2=%.1f PutPixel3=%.1f\n",
             window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_DRAWLINE] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_DRAWFILLRECT] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_DRAWROUNDRECT] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_DRAWFILLROUNDRECT] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_DRAWELLIPSE4] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_DRAWELLIPSE7] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_DRAWFILLELLIPSE] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_CLEARSCREEN] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_BEGINSCREEN] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_FASTCLEARNONCLIPPING] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_GETFRAMEBUFFERPTR] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_PUTPIXEL2] / (float) window_frames,
             g_cgsgraphics_calls_by_kind[CGSGFX_PUTPIXEL3] / (float) window_frames);
    for (int i = 0; i < CGSGFX_COUNT; i++) g_cgsgraphics_calls_by_kind[i] = 0;
}

#ifdef ZENONIA_INSTRUMENT_CGSGRAPHICS
static so_hook g_cgsgfx_drawline_hook;
static int CGsGraphics_DrawLine_counter_hook(void *this_ptr, int a, int b, int c, int d, unsigned int e) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_DRAWLINE]++;
    so_unhook(&g_cgsgfx_drawline_hook);
    int ret = ((int (*)(void *, int, int, int, int, unsigned int))
        (g_cgsgfx_drawline_hook.thumb_addr ? g_cgsgfx_drawline_hook.thumb_addr : g_cgsgfx_drawline_hook.addr))
        (this_ptr, a, b, c, d, e);
    g_cgsgfx_drawline_hook = hook_addr(
        g_cgsgfx_drawline_hook.thumb_addr ? g_cgsgfx_drawline_hook.thumb_addr : g_cgsgfx_drawline_hook.addr,
        (uintptr_t) CGsGraphics_DrawLine_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_drawfillrect_hook;
static int CGsGraphics_DrawFillRect_counter_hook(void *this_ptr, int a, int b, int c, int d, unsigned int e) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_DRAWFILLRECT]++;
    so_unhook(&g_cgsgfx_drawfillrect_hook);
    int ret = ((int (*)(void *, int, int, int, int, unsigned int))
        (g_cgsgfx_drawfillrect_hook.thumb_addr ? g_cgsgfx_drawfillrect_hook.thumb_addr : g_cgsgfx_drawfillrect_hook.addr))
        (this_ptr, a, b, c, d, e);
    g_cgsgfx_drawfillrect_hook = hook_addr(
        g_cgsgfx_drawfillrect_hook.thumb_addr ? g_cgsgfx_drawfillrect_hook.thumb_addr : g_cgsgfx_drawfillrect_hook.addr,
        (uintptr_t) CGsGraphics_DrawFillRect_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_drawroundrect_hook;
static int CGsGraphics_DrawRoundRect_counter_hook(void *this_ptr, int a, int b, int c, int d, int e, unsigned int f) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_DRAWROUNDRECT]++;
    so_unhook(&g_cgsgfx_drawroundrect_hook);
    int ret = ((int (*)(void *, int, int, int, int, int, unsigned int))
        (g_cgsgfx_drawroundrect_hook.thumb_addr ? g_cgsgfx_drawroundrect_hook.thumb_addr : g_cgsgfx_drawroundrect_hook.addr))
        (this_ptr, a, b, c, d, e, f);
    g_cgsgfx_drawroundrect_hook = hook_addr(
        g_cgsgfx_drawroundrect_hook.thumb_addr ? g_cgsgfx_drawroundrect_hook.thumb_addr : g_cgsgfx_drawroundrect_hook.addr,
        (uintptr_t) CGsGraphics_DrawRoundRect_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_drawfillroundrect_hook;
static int CGsGraphics_DrawFillRoundRect_counter_hook(void *this_ptr, int a, int b, int c, int d, int e, unsigned int f) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_DRAWFILLROUNDRECT]++;
    so_unhook(&g_cgsgfx_drawfillroundrect_hook);
    int ret = ((int (*)(void *, int, int, int, int, int, unsigned int))
        (g_cgsgfx_drawfillroundrect_hook.thumb_addr ? g_cgsgfx_drawfillroundrect_hook.thumb_addr : g_cgsgfx_drawfillroundrect_hook.addr))
        (this_ptr, a, b, c, d, e, f);
    g_cgsgfx_drawfillroundrect_hook = hook_addr(
        g_cgsgfx_drawfillroundrect_hook.thumb_addr ? g_cgsgfx_drawfillroundrect_hook.thumb_addr : g_cgsgfx_drawfillroundrect_hook.addr,
        (uintptr_t) CGsGraphics_DrawFillRoundRect_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_drawellipse4_hook;
static int CGsGraphics_DrawEllipse4_counter_hook(void *this_ptr, int a, int b, int c, unsigned int d) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_DRAWELLIPSE4]++;
    so_unhook(&g_cgsgfx_drawellipse4_hook);
    int ret = ((int (*)(void *, int, int, int, unsigned int))
        (g_cgsgfx_drawellipse4_hook.thumb_addr ? g_cgsgfx_drawellipse4_hook.thumb_addr : g_cgsgfx_drawellipse4_hook.addr))
        (this_ptr, a, b, c, d);
    g_cgsgfx_drawellipse4_hook = hook_addr(
        g_cgsgfx_drawellipse4_hook.thumb_addr ? g_cgsgfx_drawellipse4_hook.thumb_addr : g_cgsgfx_drawellipse4_hook.addr,
        (uintptr_t) CGsGraphics_DrawEllipse4_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_drawellipse7_hook;
static int CGsGraphics_DrawEllipse7_counter_hook(void *this_ptr, int a, int b, int c, int d, unsigned int e, int f, int g) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_DRAWELLIPSE7]++;
    so_unhook(&g_cgsgfx_drawellipse7_hook);
    int ret = ((int (*)(void *, int, int, int, int, unsigned int, int, int))
        (g_cgsgfx_drawellipse7_hook.thumb_addr ? g_cgsgfx_drawellipse7_hook.thumb_addr : g_cgsgfx_drawellipse7_hook.addr))
        (this_ptr, a, b, c, d, e, f, g);
    g_cgsgfx_drawellipse7_hook = hook_addr(
        g_cgsgfx_drawellipse7_hook.thumb_addr ? g_cgsgfx_drawellipse7_hook.thumb_addr : g_cgsgfx_drawellipse7_hook.addr,
        (uintptr_t) CGsGraphics_DrawEllipse7_counter_hook);
    return ret;
}

// Fase 38 (port_progress.md): BeginScreen/ClearScreen/DrawFillEllipse/
// FastClearNonClipping devuelven `undefined8` en el pseudo-C de Ghidra (8
// bytes reales, par de registros r0:r1 en ARM AAPCS) -- a diferencia del
// resto de las 13 (confirmado `void` una por una contra out_ghidra.c), asi
// que forwardear solo `int` (r0) en estos 4 descartaba silenciosamente la
// mitad alta (r1) del valor real, dejando basura de nuestro propio stack en
// r1 para cualquier caller que la usara. Causa real confirmada de un crash
// (Fase 38): BeginScreen/ClearScreen corren una vez al arrancar el
// renderizado, su r1 alimenta el estado que BeginAlpha/EndAlpha leen despues
// -- con r1 corrompido, EndAlpha terminaba saltando a una direccion invalida
// (Prefetch abort). Uso `long long` para que el compilador arme r0:r1
// correctamente en la firma de retorno, no `int`.
static so_hook g_cgsgfx_drawfillellipse_hook;
static long long CGsGraphics_DrawFillEllipse_counter_hook(void *this_ptr, int a, int b, int c, int d, unsigned int e, int f, int g) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_DRAWFILLELLIPSE]++;
    so_unhook(&g_cgsgfx_drawfillellipse_hook);
    long long ret = ((long long (*)(void *, int, int, int, int, unsigned int, int, int))
        (g_cgsgfx_drawfillellipse_hook.thumb_addr ? g_cgsgfx_drawfillellipse_hook.thumb_addr : g_cgsgfx_drawfillellipse_hook.addr))
        (this_ptr, a, b, c, d, e, f, g);
    g_cgsgfx_drawfillellipse_hook = hook_addr(
        g_cgsgfx_drawfillellipse_hook.thumb_addr ? g_cgsgfx_drawfillellipse_hook.thumb_addr : g_cgsgfx_drawfillellipse_hook.addr,
        (uintptr_t) CGsGraphics_DrawFillEllipse_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_clearscreen_hook;
static long long CGsGraphics_ClearScreen_counter_hook(void *this_ptr, unsigned int a) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_CLEARSCREEN]++;
    so_unhook(&g_cgsgfx_clearscreen_hook);
    long long ret = ((long long (*)(void *, unsigned int))
        (g_cgsgfx_clearscreen_hook.thumb_addr ? g_cgsgfx_clearscreen_hook.thumb_addr : g_cgsgfx_clearscreen_hook.addr))
        (this_ptr, a);
    g_cgsgfx_clearscreen_hook = hook_addr(
        g_cgsgfx_clearscreen_hook.thumb_addr ? g_cgsgfx_clearscreen_hook.thumb_addr : g_cgsgfx_clearscreen_hook.addr,
        (uintptr_t) CGsGraphics_ClearScreen_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_beginscreen_hook;
static long long CGsGraphics_BeginScreen_counter_hook(void *this_ptr, unsigned int a) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_BEGINSCREEN]++;
    so_unhook(&g_cgsgfx_beginscreen_hook);
    long long ret = ((long long (*)(void *, unsigned int))
        (g_cgsgfx_beginscreen_hook.thumb_addr ? g_cgsgfx_beginscreen_hook.thumb_addr : g_cgsgfx_beginscreen_hook.addr))
        (this_ptr, a);
    g_cgsgfx_beginscreen_hook = hook_addr(
        g_cgsgfx_beginscreen_hook.thumb_addr ? g_cgsgfx_beginscreen_hook.thumb_addr : g_cgsgfx_beginscreen_hook.addr,
        (uintptr_t) CGsGraphics_BeginScreen_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_fastclearnonclipping_hook;
static long long CGsGraphics_FastClearNonClipping_counter_hook(void *this_ptr, int a, int b, int c, int d) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_FASTCLEARNONCLIPPING]++;
    so_unhook(&g_cgsgfx_fastclearnonclipping_hook);
    long long ret = ((long long (*)(void *, int, int, int, int))
        (g_cgsgfx_fastclearnonclipping_hook.thumb_addr ? g_cgsgfx_fastclearnonclipping_hook.thumb_addr : g_cgsgfx_fastclearnonclipping_hook.addr))
        (this_ptr, a, b, c, d);
    g_cgsgfx_fastclearnonclipping_hook = hook_addr(
        g_cgsgfx_fastclearnonclipping_hook.thumb_addr ? g_cgsgfx_fastclearnonclipping_hook.thumb_addr : g_cgsgfx_fastclearnonclipping_hook.addr,
        (uintptr_t) CGsGraphics_FastClearNonClipping_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_getframebufferptr_hook;
static int CGsGraphics_GetFrameBufferPtr_counter_hook(void *this_ptr, int a, int b) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_GETFRAMEBUFFERPTR]++;
    so_unhook(&g_cgsgfx_getframebufferptr_hook);
    int ret = ((int (*)(void *, int, int))
        (g_cgsgfx_getframebufferptr_hook.thumb_addr ? g_cgsgfx_getframebufferptr_hook.thumb_addr : g_cgsgfx_getframebufferptr_hook.addr))
        (this_ptr, a, b);
    g_cgsgfx_getframebufferptr_hook = hook_addr(
        g_cgsgfx_getframebufferptr_hook.thumb_addr ? g_cgsgfx_getframebufferptr_hook.thumb_addr : g_cgsgfx_getframebufferptr_hook.addr,
        (uintptr_t) CGsGraphics_GetFrameBufferPtr_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_putpixel2_hook;
static int CGsGraphics_PutPixel2_counter_hook(void *this_ptr, int a, int b) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_PUTPIXEL2]++;
    so_unhook(&g_cgsgfx_putpixel2_hook);
    int ret = ((int (*)(void *, int, int))
        (g_cgsgfx_putpixel2_hook.thumb_addr ? g_cgsgfx_putpixel2_hook.thumb_addr : g_cgsgfx_putpixel2_hook.addr))
        (this_ptr, a, b);
    g_cgsgfx_putpixel2_hook = hook_addr(
        g_cgsgfx_putpixel2_hook.thumb_addr ? g_cgsgfx_putpixel2_hook.thumb_addr : g_cgsgfx_putpixel2_hook.addr,
        (uintptr_t) CGsGraphics_PutPixel2_counter_hook);
    return ret;
}

static so_hook g_cgsgfx_putpixel3_hook;
static int CGsGraphics_PutPixel3_counter_hook(void *this_ptr, int a, int b, unsigned int c) {
    g_cgsgraphics_calls_this_frame++;
    g_cgsgraphics_calls_by_kind[CGSGFX_PUTPIXEL3]++;
    so_unhook(&g_cgsgfx_putpixel3_hook);
    int ret = ((int (*)(void *, int, int, unsigned int))
        (g_cgsgfx_putpixel3_hook.thumb_addr ? g_cgsgfx_putpixel3_hook.thumb_addr : g_cgsgfx_putpixel3_hook.addr))
        (this_ptr, a, b, c);
    g_cgsgfx_putpixel3_hook = hook_addr(
        g_cgsgfx_putpixel3_hook.thumb_addr ? g_cgsgfx_putpixel3_hook.thumb_addr : g_cgsgfx_putpixel3_hook.addr,
        (uintptr_t) CGsGraphics_PutPixel3_counter_hook);
    return ret;
}
#endif

// Llamar DESPUES de so_relocate/so_resolve, igual que
// zenonia_install_hotpath_probe.
void zenonia_install_cgsgraphics_probe(so_module *mod) {
#ifdef ZENONIA_INSTRUMENT_CGSGRAPHICS
    struct { const char *sym; so_hook *hook; uintptr_t fn; const char *name; } targets[] = {
        { "_ZN11CGsGraphics8DrawLineEiiiij", &g_cgsgfx_drawline_hook, (uintptr_t) CGsGraphics_DrawLine_counter_hook, "DrawLine" },
        { "_ZN11CGsGraphics12DrawFillRectEiiiij", &g_cgsgfx_drawfillrect_hook, (uintptr_t) CGsGraphics_DrawFillRect_counter_hook, "DrawFillRect" },
        { "_ZN11CGsGraphics13DrawRoundRectEiiiiij", &g_cgsgfx_drawroundrect_hook, (uintptr_t) CGsGraphics_DrawRoundRect_counter_hook, "DrawRoundRect" },
        { "_ZN11CGsGraphics17DrawFillRoundRectEiiiiij", &g_cgsgfx_drawfillroundrect_hook, (uintptr_t) CGsGraphics_DrawFillRoundRect_counter_hook, "DrawFillRoundRect" },
        { "_ZN11CGsGraphics11DrawEllipseEiiij", &g_cgsgfx_drawellipse4_hook, (uintptr_t) CGsGraphics_DrawEllipse4_counter_hook, "DrawEllipse(4)" },
        { "_ZN11CGsGraphics11DrawEllipseEiiiijii", &g_cgsgfx_drawellipse7_hook, (uintptr_t) CGsGraphics_DrawEllipse7_counter_hook, "DrawEllipse(7)" },
        { "_ZN11CGsGraphics15DrawFillEllipseEiiiijii", &g_cgsgfx_drawfillellipse_hook, (uintptr_t) CGsGraphics_DrawFillEllipse_counter_hook, "DrawFillEllipse" },
        { "_ZN11CGsGraphics11ClearScreenEj", &g_cgsgfx_clearscreen_hook, (uintptr_t) CGsGraphics_ClearScreen_counter_hook, "ClearScreen" },
        { "_ZN11CGsGraphics11BeginScreenEj", &g_cgsgfx_beginscreen_hook, (uintptr_t) CGsGraphics_BeginScreen_counter_hook, "BeginScreen" },
        { "_ZN11CGsGraphics20FastClearNonClippingEiiii", &g_cgsgfx_fastclearnonclipping_hook, (uintptr_t) CGsGraphics_FastClearNonClipping_counter_hook, "FastClearNonClipping" },
        { "_ZN11CGsGraphics17GetFrameBufferPtrEii", &g_cgsgfx_getframebufferptr_hook, (uintptr_t) CGsGraphics_GetFrameBufferPtr_counter_hook, "GetFrameBufferPtr" },
        { "_ZN11CGsGraphics8PutPixelEii", &g_cgsgfx_putpixel2_hook, (uintptr_t) CGsGraphics_PutPixel2_counter_hook, "PutPixel(2)" },
        { "_ZN11CGsGraphics8PutPixelEiij", &g_cgsgfx_putpixel3_hook, (uintptr_t) CGsGraphics_PutPixel3_counter_hook, "PutPixel(3)" },
    };
    int installed = 0;
    for (unsigned int i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        uintptr_t addr = so_symbol(mod, targets[i].sym);
        if (addr) {
            *targets[i].hook = hook_addr(addr, targets[i].fn);
            installed++;
        } else {
            game_log("[CGsGraphicsProbe] simbolo %s (%s) no encontrado\n", targets[i].name, targets[i].sym);
        }
    }
    so_flush_caches(mod);
    game_log("[CGsGraphicsProbe] %d/%d hooks de conteo instalados sobre CGsGraphics::\n",
             installed, (int)(sizeof(targets) / sizeof(targets[0])));
#else
    (void) mod;
#endif
}

// --- Fase 41 (port_progress.md): optimizacion REAL (no solo conteo) de
// CGsGraphics::FastClearNonClipping -- optimization_ideas.md, "Lo que NO se
// ha hecho" #3.
//
// A diferencia de ClearScreen (tiene un camino MC_grpSetContext/MC_grpFillRect
// para colores "reales" que no podemos reimplementar sin reversear MC_grp*) y
// DrawFillRect/DrawRoundRect/DrawEllipse (pasan por RGBBlend16, alpha blending
// per-pixel cuyo comportamiento con distintos valores de alpha no esta
// confirmado), FastClearNonClipping SI tiene una implementacion simple y
// totalmente confirmada contra el pseudo-C real de Ghidra
// (decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c:22992-23014):
//
//   undefined8 CGsGraphics::FastClearNonClipping(this, x, y, w, h) {
//       iVar3 = (*(int**)(this+8))[0x11];               // row_off
//       iVar4 = *(int*)(**(int**)(this+8) + 0x10);        // fb_base
//       ppiVar1 = (int**)GcxGetMainScreenBuffer();
//       iVar2 = **ppiVar1 << ((*ppiVar1)[3] >> 4 & 0xff); // stride (bytes/fila)
//       __s = fb_base + iVar2*(y + row_off) + x*2;        // RGB565 -- x*2 confirma 2 bytes/pixel
//       for (row = 0; row < h; row++) { memset(__s, 0, w*2); __s += iVar2; }
//       return CONCAT44(w, this);
//   }
//
// Es un clear a NEGRO incondicional (memset a 0, sin color -- a diferencia de
// ClearScreen), row por row. La unica optimizacion real y segura posible sin
// arriesgar una regresion visual: cuando el rectangulo es de ANCHO COMPLETO
// (stride == w*2, el caso comun de un clear de pantalla/fondo sin recorte
// horizontal), las filas son contiguas en memoria y se puede reemplazar el
// loop de H llamadas a memset por UNA sola memset(stride*h bytes) -- mismos
// bytes, mismo valor, mismo puntero inicial, sin cambiar el resultado. Cuando
// el rectangulo SI esta recortado horizontalmente (stride != w*2), se hace el
// mismo loop por fila que el original pero via neon_memset (loader/neon_mem.c)
// en vez de la memset de libc.
//
// Reemplaza la funcion COMPLETA (no llama al original) -- no hay ciclo
// unhook/llamar-original/rehook, asi que esto NO esta expuesto a la clase de
// bug de reinstalacion de las Fases 37-40 (esos bugs eran todos sobre hooks
// que se reinstalan en cada invocacion; este hook se instala UNA vez y ya).
//
// Retorno: el pseudo-C devuelve CONCAT44(w, this) (64 bits: r1=w, r0=this) --
// ningun caller confirmado lo consume, pero se replica igual por las dudas
// (misma leccion de la Fase 38: no asumir que un valor de retorno no importa).
#ifdef ZENONIA_FASTCLEAR_OPTIMIZE
// GcxGetMainScreenBuffer vive dentro de libgameDSO.so (simbolo global sin
// mangling C++, ver "int GcxGetMainScreenBuffer(void);" en out_ghidra.h) -- no
// esta linkeado con nuestro loader, hay que resolverlo con so_symbol() en la
// instalacion del hook y llamarlo a traves de un puntero a funcion, igual que
// PutCompressImg_hook/etc. llaman al original.
typedef int (*GcxGetMainScreenBuffer_fn)(void);
static GcxGetMainScreenBuffer_fn g_GcxGetMainScreenBuffer = NULL;

static long long CGsGraphics_FastClearNonClipping_neon_hook(void *this_ptr, int x, int y, int w, int h) {
    intptr_t this_i = (intptr_t) this_ptr;
    int *field8 = *(int **) (this_i + 8);
    int row_off = field8[0x11];
    int fb_base = *(int *) (intptr_t) (field8[0] + 0x10);

    int **ppi = (int **) (intptr_t) g_GcxGetMainScreenBuffer();
    int *inner = *ppi;
    int stride = inner[0] << ((inner[3] >> 4) & 0xff);

    uint8_t *s = (uint8_t *) (intptr_t) (fb_base + stride * (y + row_off) + x * 2);
    size_t row_bytes = (size_t) w * 2;

    if (h > 0) {
#ifdef ZENONIA_GPU_QUADS
        uint32_t ckey32 = 0x0001 | (0x0001 << 16);
        if ((size_t) stride == row_bytes) {
            uint32_t *d32 = (uint32_t *)s;
            size_t count = (row_bytes * (size_t) h) / 4;
            for (size_t i = 0; i < count; i++) d32[i] = ckey32;
        } else {
            for (int row = 0; row < h; row++) {
                uint32_t *d32 = (uint32_t *)s;
                size_t count = row_bytes / 4;
                for (size_t i = 0; i < count; i++) d32[i] = ckey32;
                s += stride;
            }
        }
#else
        if ((size_t) stride == row_bytes) {
            neon_memset(s, 0, row_bytes * (size_t) h);
        } else {
            for (int row = 0; row < h; row++) {
                neon_memset(s, 0, row_bytes);
                s += stride;
            }
        }
#endif
    }

    return ((long long) (uint32_t) (uintptr_t) this_ptr) | ((long long) (uint32_t) w << 32);
}
#endif

// Llamar DESPUES de so_relocate/so_resolve, igual que
// zenonia_install_cgsgraphics_probe. NO instalar junto con
// INSTRUMENT_CGSGRAPHICS_CALLS -- ambos hookean el mismo simbolo
// (CGsGraphics::FastClearNonClipping); CMakeLists.txt ya lo valida con un
// FATAL_ERROR si los dos flags terminan ON a la vez.
void zenonia_install_fastclear_optimize(so_module *mod) {
#ifdef ZENONIA_FASTCLEAR_OPTIMIZE
    uintptr_t gcx_addr = so_symbol(mod, "GcxGetMainScreenBuffer");
    uintptr_t addr = so_symbol(mod, "_ZN11CGsGraphics20FastClearNonClippingEiiii");
    if (gcx_addr && addr) {
        g_GcxGetMainScreenBuffer = (GcxGetMainScreenBuffer_fn) gcx_addr;
        static so_hook hook;
        hook = hook_addr(addr, (uintptr_t) CGsGraphics_FastClearNonClipping_neon_hook);
        (void) hook;
        so_flush_caches(mod);
        game_log("[FastClearOptimize] hook instalado en FastClearNonClipping (0x%08x), GcxGetMainScreenBuffer=0x%08x\n",
                 (unsigned int) addr, (unsigned int) gcx_addr);
    } else {
        game_log("[FastClearOptimize] simbolo FastClearNonClipping (0x%08x) o GcxGetMainScreenBuffer (0x%08x) no encontrado -- optimizacion NO esta activa\n",
                 (unsigned int) addr, (unsigned int) gcx_addr);
    }
#else
    (void) mod;
#endif
}

// Stub para __errno
int* __errno(void) {
    static int dummy_errno = 0;
    return &dummy_errno;
}

// Wrappers para OpenGL de punto fijo (GLES1)
void glClearColorx_wrapper(int r, int g, int b, int a) {
    glClearColor(r / 65536.0f, g / 65536.0f, b / 65536.0f, a / 65536.0f);
}

void glTexParameterx_wrapper(GLenum target, GLenum pname, int param) {
    glTexParameteri(target, pname, param);
}

// Buffer de conversion reutilizado entre llamadas: el motor sube el
// framebuffer 800x480 completo antes de CADA quad (varias veces por frame),
// y hacer malloc/free de 1.5 MB en cada upload era parte del costo por
// frame. El resultado es valido solo hasta la proxima llamada — los call
// sites lo consumen de inmediato en glTexImage2D/glTexSubImage2D.
// Conversion RGB565 -> RGBA8888 vectorizada con NEON (8 pixeles por
// iteracion). El framebuffer de software del motor se sube completo todos
// los frames (sin cache posible, los pixeles cambian por diseno) -- a la
// resolucion real de este port (960x544 = ~522k pixeles) este es el costo
// dominante confirmado en consola real (ver port_progress.md Fase 13, 21;
// bajar la resolucion de render rompe la camara del motor, asi que la unica
// via seria vectorizar esta conversion en si). Reemplaza el enfoque anterior
// de tablas de lookup (5/6 bits -> 8 bits) por replicacion de bits
// (`(v<<3)|(v>>2)` para 5 bits, `(v<<2)|(v>>4)` para 6 bits) -- el resultado
// es identico o a lo sumo +-1 respecto al *255/N exacto (imperceptible),
// pero son solo shifts/ORs, que vectorizan directo con NEON (una tabla de
// lookup de 64 entradas NO vectoriza bien: vtbl de ARMv7 solo soporta hasta
// 32 bytes por lookup). vst4_u8 hace el interleaving R/G/B/A a memoria en
// una sola instruccion.
void *convert_rgb565_to_rgba8888(const void *pixels, int width, int height) {
    static uint8_t *conv_buf = NULL;
    static int conv_buf_cap = 0;

    if (!pixels) return NULL;

    const uint16_t *src = (const uint16_t *)pixels;
    int npix = width * height;
    if (npix * 4 > conv_buf_cap) {
        conv_buf = (uint8_t *)realloc(conv_buf, npix * 4);
        conv_buf_cap = npix * 4;
    }
    uint8_t *dst = conv_buf;

    int i = 0;
    int npix8 = npix & ~7; // multiplo de 8 mas cercano hacia abajo
    for (; i < npix8; i += 8) {
        uint16x8_t p = vld1q_u16(src + i);

        uint16x8_t r5 = vshrq_n_u16(p, 11);                              // 0..31
        uint16x8_t g6 = vandq_u16(vshrq_n_u16(p, 5), vdupq_n_u16(0x3F));  // 0..63
        uint16x8_t b5 = vandq_u16(p, vdupq_n_u16(0x1F));                  // 0..31

        uint16x8_t r8_16 = vorrq_u16(vshlq_n_u16(r5, 3), vshrq_n_u16(r5, 2));
        uint16x8_t g8_16 = vorrq_u16(vshlq_n_u16(g6, 2), vshrq_n_u16(g6, 4));
        uint16x8_t b8_16 = vorrq_u16(vshlq_n_u16(b5, 3), vshrq_n_u16(b5, 2));

        // Fase 79 (2026-07-24): color-key 0x0001 -> alpha 0 -- leftover de la
        // arquitectura PRE-GPU_QUADS (ver comentario de mas arriba, "el motor
        // sube el framebuffer... antes de CADA quad"), de cuando este buffer
        // era la superficie de composicion de sprites en si y la
        // transparencia por color-key importaba para que un sprite dejara
        // ver lo ya dibujado debajo EN EL MISMO buffer. Bajo GPU_QUADS este
        // buffer es SOLO la capa base (fondo/CGsGraphics), sin nada detras --
        // aplicar el color-key aca hace transparentes los pixeles "vacios"
        // del fondo (la mayoria de la pantalla, ya que los sprites ya no
        // escriben aca), rompiendo la composicion (candidato real para "los
        // sprites desaparecen" al probar RGB565_DIRECT_UPLOAD=OFF + GPU_QUADS
        // juntos, Fase 78). Bajo GPU_QUADS, alpha SIEMPRE opaco.
#ifdef ZENONIA_GPU_QUADS
        uint8x8_t alpha = vdup_n_u8(0xFF);
#else
        uint16x8_t is_ckey = vceqq_u16(p, vdupq_n_u16(0x0001));
        uint8x8_t alpha = vmvn_u8(vmovn_u16(is_ckey));
#endif

        uint8x8x4_t rgba;
        rgba.val[0] = vand_u8(vmovn_u16(r8_16), alpha);
        rgba.val[1] = vand_u8(vmovn_u16(g8_16), alpha);
        rgba.val[2] = vand_u8(vmovn_u16(b8_16), alpha);
        rgba.val[3] = alpha;
        vst4_u8(dst + i * 4, rgba); // interleaved R,G,B,A directo a memoria
    }

    // Cola escalar (npix no multiplo de 8) -- mismo criterio de la Fase 79
    // que la pasada NEON de arriba: bajo GPU_QUADS este buffer es solo la
    // capa base opaca, el color-key 0x0001->transparente no aplica.
    for (; i < npix; i++) {
        uint16_t p = src[i];
#ifndef ZENONIA_GPU_QUADS
        if (p == 0x0001) {
            ((uint32_t *) dst)[i] = 0x00000000u;
        } else
#endif
        {
            uint32_t r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
            uint32_t r = (r5 << 3) | (r5 >> 2);
            uint32_t g = (g6 << 2) | (g6 >> 4);
            uint32_t b = (b5 << 3) | (b5 >> 2);
            ((uint32_t *) dst)[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    }

    static int conv_log = 0;
    if (conv_log < 10) {
        uint16_t min_p = 0xFFFF, max_p = 0x0000;
        for (int j = 0; j < npix; j++) {
            if (src[j] < min_p) min_p = src[j];
            if (src[j] > max_p) max_p = src[j];
        }
        game_log("[GL] convert_rgb565_to_rgba8888 (NEON): min=%04x max=%04x\n", min_p, max_p);
        conv_log++;
    }

    return dst;
}

#ifdef ZENONIA_DIRTY_RECT_UPLOAD
// Fase 36 (port_progress.md), optimization_ideas.md #2: en vez de subir el
// framebuffer RGB565 completo cada frame via glTexSubImage2D (unico patron
// confirmado -- el motor nunca vuelve a llamar glTexImage2D despues del
// inicial, ver comentario junto a glTexSubImage2D_wrapper), compara fila por
// fila contra una copia cacheada del frame anterior y sube solo el rango
// [primera fila distinta, ultima fila distinta] via glTexSubImage2D. Solo
// tiene sentido junto con RGB565_DIRECT_UPLOAD (el camino NEON-fallback
// convierte a RGBA8888 antes de subir, formato/tamano distinto, no se toca
// aca). Si el juego redibuja la pantalla entera todos los frames (esperable
// en combate, con fondo+sprites en movimiento constante), esto no ahorra
// nada y el costo del memcmp/memcpy de diffing pasa a ser una regresion neta
// -- ver avg_dirtyrect_pct_uploaded del log antes de sacar conclusiones.
static void upload_rgb565_dirty_rect(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                      GLsizei width, GLsizei height, GLenum format, GLenum type,
                                      const void *pixels) {
    static uint16_t *prev_buf = NULL;
    static int prev_w = 0, prev_h = 0;

    const uint16_t *cur = (const uint16_t *) pixels;
    size_t row_bytes = (size_t) width * sizeof(uint16_t);

    g_dirtyrect_rows_total_this_frame += height;

    if (!prev_buf || prev_w != width || prev_h != height) {
        // Primera vez (o cambio de resolucion): sin baseline valida para
        // comparar -- subir todo y arrancar el cache desde cero.
        free(prev_buf);
        prev_buf = (uint16_t *) malloc((size_t) width * height * sizeof(uint16_t));
        prev_w = width;
        prev_h = height;
        if (prev_buf) memcpy(prev_buf, cur, (size_t) width * height * sizeof(uint16_t));
        g_dirtyrect_rows_uploaded_this_frame += height;
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
        return;
    }

    int first_dirty = -1, last_dirty = -1;
    for (int row = 0; row < height; row++) {
        const uint16_t *cur_row = cur + (size_t) row * width;
        const uint16_t *prev_row = prev_buf + (size_t) row * width;
        if (memcmp(cur_row, prev_row, row_bytes) != 0) {
            if (first_dirty < 0) first_dirty = row;
            last_dirty = row;
        }
    }

    if (first_dirty < 0) {
        // Nada cambio respecto al frame anterior -- ni siquiera hace falta
        // el glTexSubImage2D.
        return;
    }

    int dirty_rows = last_dirty - first_dirty + 1;
    g_dirtyrect_rows_uploaded_this_frame += dirty_rows;

    memcpy(prev_buf + (size_t) first_dirty * width, cur + (size_t) first_dirty * width,
           (size_t) dirty_rows * row_bytes);

    glTexSubImage2D(target, level, xoffset, yoffset + first_dirty, width, dirty_rows,
                     format, type, cur + (size_t) first_dirty * width);
}
#endif

#ifdef ZENONIA_INTERLACED_UPLOAD
// Fase 41 (port_progress.md), optimization_ideas.md #10 -- REINTERPRETADO.
//
// La idea original pide parchear el rasterizador de software del motor para
// que dibuje filas pares/impares alternadas por frame (ahorro de CPU real).
// Eso requiere offsets reales de Ghidra sobre las funciones "Zero"
// (g_funcZeroCopy/g_funcEnlarge/etc en out_ghidra.c), que son punteros a
// funcion resueltos en tiempo de ejecucion -- mismo tipo de riesgo que la
// Fase 31/32, no confirmado todavia (ver optimization_ideas.md, "Lo que NO se
// ha hecho" #1 y el hallazgo de la Fase 41 en port_progress.md: el modo
// ENLARGE de PutCompressImg escala hacia ARRIBA, no hacia abajo, asi que ni
// siquiera sirve como atajo para esto).
//
// Esta version aplica el mismo concepto (mitad del ancho de banda por
// DISENO, a diferencia de DIRTY_RECT_UPLOAD que depende de cuanto cambio
// realmente) en la capa de SUBIDA que si controlamos por completo: cada
// frame sube solo las filas de una paridad (pares en frames pares, impares
// en frames impares) del framebuffer RGB565 ya renderizado por el motor. El
// motor sigue renderizando TODAS las filas en CPU igual que siempre -- esto
// NO reduce el costo de render por software (ese sigue intacto), solo el
// ancho de banda CPU->GPU de la subida, y punto medible por separado si vale
// la pena.
//
// glTexSubImage2D no tiene forma de subir filas alternadas en una sola
// llamada (el destino siempre es un rango contiguo) -- esto implica height/2
// llamadas individuales de 1 fila cada una en vez de 1 sola llamada de
// buffer completo. Si el overhead por-llamada de esta vitaGL domina sobre el
// ahorro de bytes, el resultado neto puede ser una regresion -- exactamente
// el tipo de dato real que se busca con este build de prueba (ver
// build.sh --interlaced-test), no algo asumido de antemano.
static void upload_rgb565_interlaced(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                      GLsizei width, GLsizei height, GLenum format, GLenum type,
                                      const void *pixels) {
    static int parity = 0;

    const uint8_t *cur = (const uint8_t *) pixels;
    size_t row_bytes = (size_t) width * sizeof(uint16_t);
    int uploaded = 0;

    for (int row = parity; row < height; row += 2) {
        glTexSubImage2D(target, level, xoffset, yoffset + row, width, 1,
                         format, type, cur + (size_t) row * row_bytes);
        uploaded++;
    }

    g_interlaced_rows_total_this_frame += height;
    g_interlaced_rows_uploaded_this_frame += uploaded;

    parity ^= 1;
}
#endif

// --- Fase 49/50: sombra del estado GL que el motor cree vigente ---
// gpu_quads_flush() (llamado en glDrawArrays_wrapper justo despues del draw
// real del motor) pisa punteros de arrays (a un array LOCAL del flush que
// muere al retornar), textura ligada, blend, color, client states y matrix
// mode. GLES1/vitaGL no tiene glPushAttrib, asi que trackeamos aca lo ultimo
// que el motor seteo (todos sus GL calls pasan por los wrappers de esta
// tabla) y lo re-aplicamos despues del flush -- el motor asume que su estado
// GL persiste entre llamadas y frames. Sin esto, el siguiente draw del motor
// salia con punteros colgantes y arrays deshabilitados (la causa raiz de "los
// textos no aparecen" de la Fase 49, log_1784510991.txt).
static GLuint g_engine_bound_tex = 0;
static GLenum g_engine_blend_src = GL_ONE;
static GLenum g_engine_blend_dst = GL_ZERO;
static int g_engine_blend_on = 0;
static int g_engine_tex2d_on = 0;
static int g_engine_cs_vertex = 0, g_engine_cs_color = 0, g_engine_cs_texcoord = 0;
static GLenum g_engine_matrix_mode = GL_MODELVIEW;
static float g_engine_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
// Ultimos punteros NO-fixed pasados directo a GL (los GL_FIXED no hace falta
// trackearlos: pending_fixed_* los re-aplica la conversion de
// glDrawArrays_wrapper, que corre DESPUES del flush).
typedef struct { GLint size; GLenum type; GLsizei stride; const void *ptr; int valid; } gl_ptr_shadow;
static gl_ptr_shadow g_engine_vp = {0}, g_engine_cp = {0}, g_engine_tp = {0};

#ifdef ZENONIA_GPU_QUADS
static void gpu_quads_restore_engine_state(void) {
    glMatrixMode(g_engine_matrix_mode);
    glBindTexture(GL_TEXTURE_2D, g_engine_bound_tex);
    if (g_engine_tex2d_on) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    if (g_engine_blend_on) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFunc(g_engine_blend_src, g_engine_blend_dst);
    glColor4f(g_engine_color[0], g_engine_color[1], g_engine_color[2], g_engine_color[3]);
    if (g_engine_cs_vertex) glEnableClientState(GL_VERTEX_ARRAY); else glDisableClientState(GL_VERTEX_ARRAY);
    if (g_engine_cs_color) glEnableClientState(GL_COLOR_ARRAY); else glDisableClientState(GL_COLOR_ARRAY);
    if (g_engine_cs_texcoord) glEnableClientState(GL_TEXTURE_COORD_ARRAY); else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (g_engine_vp.valid) glVertexPointer(g_engine_vp.size, g_engine_vp.type, g_engine_vp.stride, g_engine_vp.ptr);
    if (g_engine_cp.valid) glColorPointer(g_engine_cp.size, g_engine_cp.type, g_engine_cp.stride, g_engine_cp.ptr);
    if (g_engine_tp.valid) glTexCoordPointer(g_engine_tp.size, g_engine_tp.type, g_engine_tp.stride, g_engine_tp.ptr);
}
#endif

// El motor sube su framebuffer interno de software (400x240, ver Fase 1 del
// plan) en RGB565 -- vitaGL no lo maneja igual que el motor original espera,
// asi que se convierte a RGBA8888 en CPU antes de subirlo. Mismo bug
// confirmado en hardware real para Zenonia 2 (mismo motor).
void glTexImage2D_wrapper(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {
#ifdef ZENONIA_GPU_QUADS
    // Fase 76 (2026-07-23): crash determinista real (3 `.psp2dmp` identicos,
    // siempre PC=0x0 dentro de la propia `_glTexSubImage2D` de vitaGL) que
    // sobrevivio al batching de la Fase 75 -- el volumen de draw calls no era
    // la causa real. Hueco encontrado por revision de codigo:
    // `gpu_quads_restore_engine_state()` (ver arriba) restaura la textura del
    // motor SOLO al final de `glDrawArrays_wrapper` (una vez por frame) -- pero
    // `gpu_quads_finish_decode`/`enqueue_text`/`enqueue_fill` (gpu_quads.c)
    // llaman `glBindTexture` DIRECTO (sin pasar por este wrapper, sin
    // actualizar `g_engine_bound_tex`) cada vez que decodifican un sprite/
    // relleno/texto NUEVO -- esto pasa DURANTE el render del motor, ANTES de
    // que el motor llame a su propio `glTexImage2D`/`glTexSubImage2D` para
    // subir su framebuffer de software. Si eso pasa, la textura realmente
    // bindeada en ese momento es la ULTIMA que gpu_quads creo, no la del
    // motor -- el motor asume (razonablemente, para un motor que nunca
    // esperaba que algo mas tocara su estado GL) que su textura sigue
    // bindeada de la ultima vez que la bindeo el mismo. Re-bindear la textura
    // del motor (ya trackeada en `g_engine_bound_tex`, actualizada por
    // `glBindTexture_wrapper` cada vez que el motor bindea algo) justo antes
    // de reenviar la llamada real cierra ese hueco sin tocar nada de
    // gpu_quads.c ni cambiar el volumen/orden de nada.
    glBindTexture(GL_TEXTURE_2D, g_engine_bound_tex);
#endif
    static int img_log = 0;
    if (img_log < 10) {
        game_log("[GL] glTexImage2D target=%x intFmt=%x w=%d h=%d format=%x type=%x pixels=%p\n",
                 target, internalformat, width, height, format, type, pixels);
        img_log++;
    }

    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        g_rgb565_uploads_this_frame++;
    }

// Fase 50: el FB base vuelve a subirse DIRECTO en RGB565 tambien en builds
// GPU_QUADS (los quads ahora se dibujan ENCIMA del FB opaco, no debajo -- ya
// no hace falta canal alfa en el FB, y la conversion por CPU de la Fase 49
// era la regresion de FPS del log_1784517442.txt).
#ifdef ZENONIA_RGB565_DIRECT
    // Prueba A/B (ver CMakeLists.txt RGB565_DIRECT_UPLOAD, port_progress.md
    // Fase 22/24): saltar por completo convert_rgb565_to_rgba8888() y subir el
    // RGB565 tal cual el motor lo entrega. Riesgo conocido (confirmado en el
    // port de Zenonia 2, mismo motor): vitaGL puede mostrar colores
    // incorrectos con este formato -- sin confirmar todavia en Zenonia 4.
    glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
#else
    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        if (pixels && img_log < 20) {
            uint16_t *p = (uint16_t *)pixels;
            game_log("  -> First 4 pixels: %04x %04x %04x %04x\n", p[0], p[1], p[2], p[3]);
        }
        void *new_pixels = convert_rgb565_to_rgba8888(pixels, width, height);
        glTexImage2D(target, level, GL_RGBA, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, new_pixels);
    } else {
        glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    }
#endif

    // Forzar filtros min/mag para que la textura no se trate como incompleta por falta de mipmaps
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

}

void glTexSubImage2D_wrapper(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {
#ifdef ZENONIA_GPU_QUADS
    // Fase 76: ver el comentario largo en glTexImage2D_wrapper -- mismo hueco,
    // mismo fix. Esta es la llamada que realmente crashea en los 3 `.psp2dmp`
    // (siempre LR dentro de esta funcion en vitaGL) -- la subida del
    // framebuffer RGB565 del motor, que corre una vez por frame y opera sobre
    // "lo que este bindeado ahora", sin re-bindear su propia textura primero.
    //
    // Fase 90/91 (port_progress.md): se probo agregar un diagnostico con
    // glGetIntegerv(GL_TEXTURE_BINDING_2D, ...) justo aca para comparar la
    // textura REALMENTE bindeada contra `g_engine_bound_tex` -- el usuario
    // reporto que el build con ese diagnostico colgo la CONSOLA ENTERA
    // (no un crash de app, sin `.psp2dmp`) jugando con GPU_QUADS, un
    // sintoma mas grave que los 6 crashes de app ya documentados. REVERTIDO
    // de inmediato como precaucion (ver Fase 91): `glGetIntegerv` puede
    // forzar una sincronizacion/flush del pipeline de la GPU real -- si el
    // estado de la cola de comandos ya estaba comprometido (el mismo
    // territorio que la Fase 81 ya vio colgar la consola con
    // DISABLE_VSYNC+GPU_QUADS), agregar una sincronizacion extra en ese
    // momento exacto es un candidato plausible para convertir un crash
    // recuperable (con dump) en un cuelgue real de hardware (sin dump, sin
    // poder diagnosticar). No confirmado con certeza total (no se pudo
    // probar la hipotesis alternativa por seguridad), pero el costo de
    // sacarlo es cero y el riesgo de dejarlo es alto -- no se vuelve a
    // agregar ninguna llamada que pueda sincronizar/esperar a la GPU en
    // este call site sin antes confirmar que es segura.
#endif
    static int subimg_log = 0;
    if (subimg_log < 10) {
        game_log("[GL] glTexSubImage2D target=%x w=%d h=%d format=%x type=%x\n", target, width, height, format, type);
        if (type == GL_UNSIGNED_SHORT_5_6_5 && pixels) {
            uint16_t *p = (uint16_t*)pixels;
            game_log("  -> First 4 pixels: %04x %04x %04x %04x\n", p[0], p[1], p[2], p[3]);
        }
        subimg_log++;
    }

    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        g_rgb565_uploads_this_frame++;
    }

// Fase 50: subida directa tambien en GPU_QUADS (ver glTexImage2D_wrapper).
#ifdef ZENONIA_RGB565_DIRECT
#ifdef ZENONIA_DIRTY_RECT_UPLOAD
    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5 && pixels) {
        upload_rgb565_dirty_rect(target, level, xoffset, yoffset, width, height, format, type, pixels);
    } else {
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }
#elif defined(ZENONIA_INTERLACED_UPLOAD)
    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5 && pixels) {
        upload_rgb565_interlaced(target, level, xoffset, yoffset, width, height, format, type, pixels);
    } else {
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }
#else
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
#endif
#else
    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        void *new_pixels = convert_rgb565_to_rgba8888(pixels, width, height);
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, GL_RGBA, GL_UNSIGNED_BYTE, new_pixels);
    } else {
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }
#endif

}

// vitaGL no consume arrays de vertices GL_FIXED correctamente (misma clase de
// bug que glClearColorx_wrapper/glTexParameterx_wrapper arriba). Este motor
// (derivado de J2ME/BREW) manda datos de vertices en punto fijo Q16.16; sin
// esta conversion, vitaGL lee los enteros crudos como si fueran floats y la
// geometria queda fuera del frustum (pantalla en blanco aunque el motor siga
// renderizando frames). glVertexPointer_wrapper diferisce la conversion hasta
// glDrawArrays_wrapper, que es donde se sabe cuantos vertices hay.
static const int32_t *pending_fixed_verts = NULL;
static GLint pending_fixed_size = 0;
static GLsizei pending_fixed_stride = 0;
static GLfloat *fixed_vert_buf = NULL;
static int fixed_vert_buf_cap = 0;

static const int32_t *pending_fixed_colors = NULL;
static GLint pending_fixed_color_size = 0;
static GLsizei pending_fixed_color_stride = 0;
static GLfloat *fixed_color_buf = NULL;
static int fixed_color_buf_cap = 0;

static const uint8_t *pending_ubyte_colors = NULL;
static GLint pending_ubyte_color_size = 0;
static GLsizei pending_ubyte_color_stride = 0;

static const int32_t *pending_fixed_texcoords = NULL;
static GLint pending_fixed_texcoord_size = 0;
static GLsizei pending_fixed_texcoord_stride = 0;
static GLfloat *fixed_texcoord_buf = NULL;
static int fixed_texcoord_buf_cap = 0;

void glDrawArrays_wrapper(GLenum mode, GLint first, GLsizei count) {
    static int draw_count = 0;
    if (draw_count < 10) {
        game_log("[GL] glDrawArrays mode=%x first=%d count=%d\n", mode, first, count);
        draw_count++;
    }

    if (pending_fixed_verts) {
        int needed_verts = first + count;
        int needed_floats = needed_verts * pending_fixed_size;
        if (needed_floats > fixed_vert_buf_cap) {
            fixed_vert_buf = (GLfloat *)realloc(fixed_vert_buf, needed_floats * sizeof(GLfloat));
            fixed_vert_buf_cap = needed_floats;
        }
        int stride_elems = pending_fixed_stride > 0 ? pending_fixed_stride / sizeof(int32_t) : pending_fixed_size;
        for (int i = 0; i < needed_verts; i++) {
            const int32_t *src = pending_fixed_verts + i * stride_elems;
            GLfloat *dst = fixed_vert_buf + i * pending_fixed_size;
            for (int c = 0; c < pending_fixed_size; c++) {
                dst[c] = src[c] / 65536.0f;
            }
        }
        glVertexPointer(pending_fixed_size, GL_FLOAT, 0, fixed_vert_buf);
    }

    if (pending_fixed_colors) {
        int needed_verts = first + count;
        int needed_floats = needed_verts * pending_fixed_color_size;
        if (needed_floats > fixed_color_buf_cap) {
            fixed_color_buf = (GLfloat *)realloc(fixed_color_buf, needed_floats * sizeof(GLfloat));
            fixed_color_buf_cap = needed_floats;
        }
        int stride_elems = pending_fixed_color_stride > 0 ? pending_fixed_color_stride / sizeof(int32_t) : pending_fixed_color_size;
        for (int i = 0; i < needed_verts; i++) {
            const int32_t *src = pending_fixed_colors + i * stride_elems;
            GLfloat *dst = fixed_color_buf + i * pending_fixed_color_size;
            for (int c = 0; c < pending_fixed_color_size; c++) {
                dst[c] = src[c] / 65536.0f;
            }
        }
        glColorPointer(pending_fixed_color_size, GL_FLOAT, 0, fixed_color_buf);
    } else if (pending_ubyte_colors) {
        // Fase 60 (2026-07-20): blanquear este array SOLO cuando el quad que
        // se esta dibujando cubre (aprox) la pantalla completa -- el UNICO
        // caso confirmado con evidencia real (Fase 51-53, 500+ frames en 3
        // pantallas) de que este mecanismo trae basura/memoria sin
        // inicializar. Blanquear CUALQUIER uso (Fase 58) rompio el fondo
        // oscuro/traslucido de los cuadros de tips/guardado -- ver el
        // comentario largo en glColorPointer_wrapper.
        float quad_w = 0.0f, quad_h = 0.0f;
        int have_extent = 0;
        {
            const float *v = NULL;
            int stride_elems = 0, comp = 2;
            if (pending_fixed_verts) {
                v = fixed_vert_buf;
                comp = pending_fixed_size;
                stride_elems = comp;
            } else if (g_engine_vp.valid && g_engine_vp.type == GL_FLOAT && g_engine_vp.ptr) {
                v = (const float *) g_engine_vp.ptr;
                comp = g_engine_vp.size;
                stride_elems = g_engine_vp.stride > 0 ? (int)(g_engine_vp.stride / sizeof(float)) : comp;
            }
            if (v && comp >= 2 && count > 0) {
                float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
                for (int i = first; i < first + count; i++) {
                    const float *p = v + (size_t) i * stride_elems;
                    if (p[0] < minx) minx = p[0];
                    if (p[0] > maxx) maxx = p[0];
                    if (p[1] < miny) miny = p[1];
                    if (p[1] > maxy) maxy = p[1];
                }
                quad_w = maxx - minx;
                quad_h = maxy - miny;
                have_extent = 1;
            }
        }
        int force_white = have_extent && quad_w > 900.0f && quad_h > 500.0f;

        static uint8_t white_buf[65536 * 4];
        static int white_ready = 0;
        const uint8_t *color_src = pending_ubyte_colors;
        int color_stride = pending_ubyte_color_stride > 0 ? pending_ubyte_color_stride : pending_ubyte_color_size;

        static int white_log = 0, pass_log = 0, unknown_log = 0;
        if (force_white) {
            if (!white_ready) { memset(white_buf, 0xFF, sizeof(white_buf)); white_ready = 1; }
            if (white_log < 20) {
                game_log("[GpuQuadsDiag] color-array blanqueado (quad %.0fx%.0f, pantalla completa) ui_status=%d\n",
                         quad_w, quad_h, g_ui_status);
                white_log++;
            }
            color_src = white_buf;
            color_stride = 4;
        } else if (have_extent) {
            if (pass_log < 20) {
                game_log("[GpuQuadsDiag] color-array UByte SIN tocar (quad %.0fx%.0f, no es pantalla completa) color=(%d,%d,%d,%d) ui_status=%d\n",
                         quad_w, quad_h, pending_ubyte_colors[0], pending_ubyte_colors[1],
                         pending_ubyte_colors[2], pending_ubyte_colors[3], g_ui_status);
                pass_log++;
            }
        } else if (unknown_log < 20) {
            game_log("[GpuQuadsDiag] color-array UByte con extent DESCONOCIDO (sin vertices float/fixed detectados) color=(%d,%d,%d,%d) ui_status=%d\n",
                     pending_ubyte_colors[0], pending_ubyte_colors[1], pending_ubyte_colors[2], pending_ubyte_colors[3], g_ui_status);
            unknown_log++;
        }

        int needed_verts = first + count;
        int needed_floats = needed_verts * pending_ubyte_color_size;
        if (needed_floats > fixed_color_buf_cap) {
            fixed_color_buf = (GLfloat *)realloc(fixed_color_buf, needed_floats * sizeof(GLfloat));
            fixed_color_buf_cap = needed_floats;
        }
        for (int i = 0; i < needed_verts; i++) {
            const uint8_t *src = color_src + i * color_stride;
            GLfloat *dst = fixed_color_buf + i * pending_ubyte_color_size;
            for (int c = 0; c < pending_ubyte_color_size; c++) {
                dst[c] = src[c] / 255.0f;
            }
        }
        glColorPointer(pending_ubyte_color_size, GL_FLOAT, 0, fixed_color_buf);
    }

    if (pending_fixed_texcoords) {
        int needed_verts = first + count;
        int needed_floats = needed_verts * pending_fixed_texcoord_size;
        if (needed_floats > fixed_texcoord_buf_cap) {
            fixed_texcoord_buf = (GLfloat *)realloc(fixed_texcoord_buf, needed_floats * sizeof(GLfloat));
            fixed_texcoord_buf_cap = needed_floats;
        }
        int stride_elems = pending_fixed_texcoord_stride > 0 ? pending_fixed_texcoord_stride / sizeof(int32_t) : pending_fixed_texcoord_size;
        for (int i = 0; i < needed_verts; i++) {
            const int32_t *src = pending_fixed_texcoords + i * stride_elems;
            GLfloat *dst = fixed_texcoord_buf + i * pending_fixed_texcoord_size;
            for (int c = 0; c < pending_fixed_texcoord_size; c++) {
                dst[c] = src[c] / 65536.0f;
            }
        }
        glTexCoordPointer(pending_fixed_texcoord_size, GL_FLOAT, 0, fixed_texcoord_buf);
    }

    // Fase 51 (2026-07-20): se probo forzar blend aca para el splash oscuro,
    // pero el razonamiento era invalido -- el destino ya es NEGRO en este
    // punto (glClearColor(0,0,0,1) en main.c antes de cada frame), y
    // blendear un tinte no-blanco contra negro nunca puede dar blanco. La
    // causa real (el array de color siempre trae (168,26,16,129) en vez de
    // blanco) se arreglo de raiz en glColorPointer_wrapper (Fase 53) --
    // forzar blend aca quedo sin objeto (con el array ya blanco, blend
    // encendido o apagado da el mismo resultado) y se elimino.
    glDrawArrays(mode, first, count);

#ifdef ZENONIA_GPU_QUADS
    // Fase 50: dibujar los quads encolados JUSTO DESPUES del draw del motor
    // (el del FB base, opaco, que queda DEBAJO) -- los sprites van encima en
    // su orden de llamada original. Si el flush dibujo algo, restaurar el
    // estado GL del motor (el flush pisa punteros/textura/blend/color/
    // client states y el motor asume que su estado persiste entre frames).
    if (gpu_quads_flush() > 0) {
        gpu_quads_restore_engine_state();
    }
#endif
}

void glTexEnvf_wrapper(GLenum target, GLenum pname, GLfloat param) {
    game_log("[GL] glTexEnvf target=%x pname=%x param=%f ui_status=%d\n", target, pname, param, g_ui_status);
    // Fase 52 (2026-07-20): diagnostico/guardia para el reporte del usuario de
    // fondo negro persistente en splash Y title (deberia ser blanco). Este
    // call SOLO ocurre 2 veces en TODO el run (confirmado en
    // log_1784520216.txt, justo al crear la textura del FB) con
    // pname=GL_TEXTURE_ENV_MODE (0x2200) y param=480.0 luego 0.0 -- ninguno de
    // los dos es un enum GL_TEXTURE_ENV_MODE valido (MODULATE=8448,
    // DECAL=8449, GL_BLEND=8450, REPLACE=7681, ADD=260, COMBINE=34192). Real
    // GL rechaza un enum invalido (GL_INVALID_ENUM) y deja el modo anterior
    // sin tocar (default = MODULATE); si vitaGL en cambio ACEPTA 0.0 tal cual
    // sin validar, la textura del FB podria terminar en un modo de combine
    // roto -- aplicado UNA vez, pero afectando TODOS los frames/pantallas
    // subsiguientes por igual (consistente con "tanto splash como title"
    // reportado). Se guardan estas 2 llamadas especificas (no se reenvian a
    // vitaGL) para que el modo quede en lo que vitaGL ya trae por default;
    // si esto no cambia nada visualmente, descarta esta hipotesis para el
    // proximo log.
    if (pname == GL_TEXTURE_ENV_MODE &&
        param != GL_MODULATE && param != GL_DECAL && param != GL_BLEND &&
        param != GL_REPLACE && param != GL_ADD && param != GL_COMBINE) {
        static int guard_log = 0;
        if (guard_log < 20) {
            game_log("[GpuQuadsDiag] glTexEnvf: pname=TEXTURE_ENV_MODE con param=%f invalido -- IGNORADO (modo previo intacto)\n", param);
            guard_log++;
        }
        return;
    }
    glTexEnvf(target, pname, param);
}

void glBlendFunc_wrapper(GLenum sfactor, GLenum dfactor) {
    static int log = 0;
    if (log < 20) {
        game_log("[GL] glBlendFunc sfactor=%x dfactor=%x ui_status=%d\n", sfactor, dfactor, g_ui_status);
        log++;
    }
    g_engine_blend_src = sfactor;
    g_engine_blend_dst = dfactor;
    glBlendFunc(sfactor, dfactor);
}

void glEnable_wrapper(GLenum cap) {
    static int enable_log = 0;
    if (enable_log < 20) {
        game_log("[GL] glEnable cap=%x\n", cap);
        enable_log++;
    }
    if (cap == GL_BLEND) g_engine_blend_on = 1;
    else if (cap == GL_TEXTURE_2D) g_engine_tex2d_on = 1;
    glEnable(cap);
}

void glDisable_wrapper(GLenum cap) {
    static int disable_log = 0;
    if (disable_log < 20) {
        game_log("[GL] glDisable cap=%x\n", cap);
        disable_log++;
    }
    if (cap == GL_BLEND) g_engine_blend_on = 0;
    else if (cap == GL_TEXTURE_2D) g_engine_tex2d_on = 0;
    glDisable(cap);
}

// Fase 49: wrapper de glBindTexture para trackear la textura ligada del motor
// (antes iba directo a vitaGL) -- gpu_quads_flush() liga sus propias texturas
// y hay que poder restaurar la del motor, y ademas asi se detecta cual textura
// es la del FB base (la que recibe subidas RGB565).
void glBindTexture_wrapper(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D) g_engine_bound_tex = texture;
    glBindTexture(target, texture);
}

// Fase 49: wrapper de glColor4f para trackear el color del motor (antes iba
// directo) -- gpu_quads_flush() lo pisa con su alpha por-quad.
void glColor4f_wrapper(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    g_engine_color[0] = r; g_engine_color[1] = g;
    g_engine_color[2] = b; g_engine_color[3] = a;
    glColor4f(r, g, b, a);
}

// Fase 49: BUG REAL corregido -- glColor4x estaba mapeado a
// glClearColorx_wrapper ("using same logic as glClearColorx for now"), que
// llama a glClearColor: cada vez que el motor seteaba el color/alpha de
// vertice en punto fijo (fades de logo/textos, tintes), en realidad cambiaba
// el CLEAR COLOR y el color de vertice quedaba intacto. Ahora convierte
// Q16.16 -> float y llama a glColor4f de verdad.
void glColor4x_wrapper(int r, int g, int b, int a) {
    glColor4f_wrapper(r / 65536.0f, g / 65536.0f, b / 65536.0f, a / 65536.0f);
}

void glTexCoordPointer_wrapper(GLint size, GLenum type, GLsizei stride, const void *pointer) {
    static int log = 0;
    if (log < 10) {
        game_log("[GL] glTexCoordPointer size=%d type=%x stride=%d pointer=%p\n", size, type, stride, pointer);
        log++;
    }
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    static int log_uvs = 0;
    if (pointer && log_uvs < 20) {
        if (type == GL_FIXED) {
            int32_t *uv = (int32_t *)pointer;
            game_log("  -> UVs Fixed (first 6): (%.2f, %.2f) (%.2f, %.2f) (%.2f, %.2f)\n",
                     uv[0]/65536.0f, uv[1]/65536.0f, uv[2]/65536.0f, uv[3]/65536.0f, uv[4]/65536.0f, uv[5]/65536.0f);
        } else {
            float *uv = (float *)pointer;
            game_log("  -> UVs Float (first 6): (%.2f, %.2f) (%.2f, %.2f) (%.2f, %.2f)\n",
                     uv[0], uv[1], uv[2], uv[3], uv[4], uv[5]);
        }
        log_uvs++;
    }

    g_engine_cs_texcoord = 1;
    if (type == GL_FIXED) {
        pending_fixed_texcoords = (const int32_t *)pointer;
        pending_fixed_texcoord_size = size;
        pending_fixed_texcoord_stride = stride;
        g_engine_tp.valid = 0; // lo re-aplica la conversion de glDrawArrays_wrapper
        return;
    }

    pending_fixed_texcoords = NULL;
    g_engine_tp.size = size; g_engine_tp.type = type;
    g_engine_tp.stride = stride; g_engine_tp.ptr = pointer;
    g_engine_tp.valid = 1;
    glTexCoordPointer(size, type, stride, pointer);
}

void glColorPointer_wrapper(GLint size, GLenum type, GLsizei stride, const void *pointer) {
    static int log = 0;
    if (log < 10) {
        game_log("[GL] glColorPointer size=%d type=%x stride=%d pointer=%p\n", size, type, stride, pointer);
        log++;
    }
    glEnableClientState(GL_COLOR_ARRAY);
    static int log_colors = 0;
    if (pointer && log_colors < 20) {
        if (type == GL_FIXED) {
            int32_t *c = (int32_t *)pointer;
            game_log("  -> Colors Fixed (first 4): (%d, %d, %d, %d) ui_status=%d\n", c[0], c[1], c[2], c[3], g_ui_status);
        } else if (type == GL_UNSIGNED_BYTE) {
            uint8_t *c = (uint8_t *)pointer;
            game_log("  -> Colors UByte (first 4): (%d, %d, %d, %d) ui_status=%d\n", c[0], c[1], c[2], c[3], g_ui_status);
        } else {
            float *c = (float *)pointer;
            game_log("  -> Colors Float (first 4): (%.2f, %.2f, %.2f, %.2f) ui_status=%d\n", c[0], c[1], c[2], c[3], g_ui_status);
        }
        log_colors++;
    }

    // Fase 52 (2026-07-20): diagnostico dedicado para el reporte del usuario
    // de fondo negro persistente en splash Y title (deberia ser blanco). El
    // log_colors de arriba se agota (cap=20) dentro de las primeras 2-3
    // llamadas de TODO el run, asi que no alcanza para saber si este color
    // realmente cambia con el tiempo (una animacion de fade legitima) o si
    // esta CLAVADO en el mismo valor sin importar la pantalla (splash vs
    // title) -- lo segundo apuntaria a memoria stale/no inicializada en vez
    // de un fade real. Solo loguea cuando el VALOR cambia (deduplicado),
    // capado a 200 cambios distintos (irrelevante en cache, solo 4 bytes de
    // comparacion por llamada).
    if (pointer && size == 4 && type == GL_UNSIGNED_BYTE) {
        static uint8_t last_val[4] = {0xEE, 0xEE, 0xEE, 0xEE}; // sentinel invalido
        static int change_log = 0;
        const uint8_t *c = (const uint8_t *) pointer;
        if (change_log < 200 && memcmp(c, last_val, 4) != 0) {
            game_log("[GpuQuadsDiag] color-array RGBA cambio a (%d,%d,%d,%d) ptr=%p ui_status=%d\n",
                     c[0], c[1], c[2], c[3], pointer, g_ui_status);
            memcpy(last_val, c, 4);
            change_log++;
        }
    }

    g_engine_cs_color = 1;
    if (type == GL_FIXED) {
        pending_fixed_colors = (const int32_t *)pointer;
        pending_fixed_color_size = size;
        pending_fixed_color_stride = stride;
        g_engine_cp.valid = 0; // lo re-aplica la conversion de glDrawArrays_wrapper
        return;
    }

    pending_fixed_colors = NULL;
    pending_ubyte_colors = NULL;

    // Fase 53 (2026-07-20): fix confirmado para el fondo negro/oscuro
    // (splash Y title, reportado identico en ambos) y candidato fuerte para
    // "falta el sprite del mapa/casas/bordes" y "las fuentes se saltan".
    // Evidencia: `nm -D --undefined-only libgameDSO.so` confirma que el .so
    // real NUNCA importa glColor4f/glColor4x/glBlendFunc/glAlphaFunc -- el
    // UNICO mecanismo de tinte de vertice en TODO el binario es este array de
    // glColorPointer. El diagnostico de la Fase 52 (dedupe por valor) mostro
    // que a lo largo de 500+ frames, atravesando splash (ui_status=-1),
    // transicion (19) y title/menu (14), este array SIEMPRE trae el mismo
    // valor (168,26,16,129) -- nunca otro. Esto tiñe TODO lo que se presenta
    // en pantalla (la textura del framebuffer completo, incluyendo bordes,
    // texto cacheado y sprites de fondo ya renderizados correctamente en el
    // framebuffer de software) con un multiplicador oscuro constante -- lo
    // que ya es dim/tenue (bordes del mundo, texto) puede volverse
    // practicamente invisible contra un fondo ya oscurecido. Se neutraliza a
    // blanco opaco (equivalente al color "current" default de GL, que este
    // motor tampoco toca jamas ya que no importa glColor4f/x). ADVERTENCIA:
    // es un workaround empirico, no un fix de causa raiz confirmada -- el
    // origen real de por que ese puntero siempre trae (168,26,16,129) en vez
    // de blanco no se identifico (no hay JNI de color/fade/tint relacionado
    // en java.c). Si mas adelante aparece un uso LEGITIMO de este mecanismo
    // con otro valor (un flash de dano, un dialogo tintado), esto lo
    // blanquearia tambien -- sin evidencia de eso en las +500 frames
    // observadas hasta ahora.
    // Fase 58 (2026-07-20): el match exacto de la Fase 53 (168,26,16,129) era
    // demasiado angosto -- un log posterior mostro el MISMO mecanismo con un
    // valor DISTINTO (232,23,16,129), que no matchea el hardcode.
    // Fase 60 (2026-07-20): blanquear SIEMPRE (sin condicionar al valor, el
    // fix de la Fase 58) resulto DEMASIADO amplio -- el usuario reporto que el
    // fondo negro del cuadro de tips/guardado desaparecio y "se distorsiona
    // completamente" (un cuadro que deberia quedar oscuro/traslucido sale
    // blanco Y OPACO, porque el `white_buf` fuerza alpha=255 tambien -- ver
    // abajo). No hay evidencia de que ESE cuadro use el mismo mecanismo con
    // otro valor "legitimo" (el log solo mostro UN cambio de valor en toda la
    // sesion), pero tampoco hay evidencia de que NO lo use -- forzar blanco
    // aca, en base al VALOR nomas, no puede distinguir entre ambos casos.
    // Por eso la decision de blanquear se movio a `glDrawArrays_wrapper`
    // (donde ya se conoce el tamano real del quad en pantalla): el UNICO caso
    // confirmado con evidencia real (Fase 51-53, 500+ frames en 3 pantallas)
    // es el quad de PRESENTACION DE PANTALLA COMPLETA (960x544) -- cualquier
    // otro uso de este mecanismo, sea el tamano que sea, se deja pasar tal
    // cual llega del motor (su alpha/color originales, no forzados a
    // blanco/opaco) hasta tener evidencia real de que tambien esta roto.
    if (type == GL_UNSIGNED_BYTE && size == 4) {
        pending_ubyte_colors = (const uint8_t *) pointer;
        pending_ubyte_color_size = size;
        pending_ubyte_color_stride = stride;
        g_engine_cp.size = size; g_engine_cp.type = type;
        g_engine_cp.stride = stride; g_engine_cp.ptr = pointer;
        g_engine_cp.valid = 0; // la decision de blanquear (o no) se toma en glDrawArrays_wrapper
        return;
    }

    g_engine_cp.size = size; g_engine_cp.type = type;
    g_engine_cp.stride = stride; g_engine_cp.ptr = pointer;
    g_engine_cp.valid = 1;
    glColorPointer(size, type, stride, pointer);
}

void glVertexPointer_wrapper(GLint size, GLenum type, GLsizei stride, const void *pointer) {
    static int log = 0;
    if (log < 10) {
        game_log("[GL] glVertexPointer size=%d type=%x stride=%d pointer=%p\n", size, type, stride, pointer);
        log++;
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    static int log_verts = 0;
    if (pointer && log_verts < 20) {
        if (type == GL_FIXED) {
            int32_t *v = (int32_t *)pointer;
            game_log("  -> Verts Fixed (first 6): (%d, %d) (%d, %d) (%d, %d)\n",
                     v[0], v[1], v[2], v[3], v[4], v[5]);
        } else {
            float *v = (float *)pointer;
            game_log("  -> Verts Float (first 6): (%.2f, %.2f) (%.2f, %.2f) (%.2f, %.2f)\n",
                     v[0], v[1], v[2], v[3], v[4], v[5]);
        }
        log_verts++;
    }

    g_engine_cs_vertex = 1;
    if (type == GL_FIXED) {
        pending_fixed_verts = (const int32_t *)pointer;
        pending_fixed_size = size;
        pending_fixed_stride = stride;
        g_engine_vp.valid = 0; // lo re-aplica la conversion de glDrawArrays_wrapper
        return;
    }

    pending_fixed_verts = NULL;
    g_engine_vp.size = size; g_engine_vp.type = type;
    g_engine_vp.stride = stride; g_engine_vp.ptr = pointer;
    g_engine_vp.valid = 1;
    glVertexPointer(size, type, stride, pointer);
}

void glEnableClientState_wrapper(GLenum array) {
    static int enable_cs_log = 0;
    if (enable_cs_log < 10) {
        game_log("[GL] glEnableClientState array=%x\n", array);
        enable_cs_log++;
    }
    if (array == GL_VERTEX_ARRAY) g_engine_cs_vertex = 1;
    else if (array == GL_COLOR_ARRAY) g_engine_cs_color = 1;
    else if (array == GL_TEXTURE_COORD_ARRAY) g_engine_cs_texcoord = 1;
    glEnableClientState(array);
}

void glDisableClientState_wrapper(GLenum array) {
    static int disable_cs_log = 0;
    if (disable_cs_log < 10) {
        game_log("[GL] glDisableClientState array=%x\n", array);
        disable_cs_log++;
    }
    if (array == GL_VERTEX_ARRAY) g_engine_cs_vertex = 0;
    else if (array == GL_COLOR_ARRAY) g_engine_cs_color = 0;
    else if (array == GL_TEXTURE_COORD_ARRAY) g_engine_cs_texcoord = 0;
    glDisableClientState(array);
}

void glMatrixMode_wrapper(GLenum mode) {
    static int log = 0;
    if (log < 10) {
        game_log("[GL] glMatrixMode mode=%x\n", mode);
        log++;
    }
    g_engine_matrix_mode = mode;
    glMatrixMode(mode);
}

void glLoadIdentity_wrapper() {
    static int log = 0;
    if (log < 10) {
        game_log("[GL] glLoadIdentity\n");
        log++;
    }
    glLoadIdentity();
}



// Pass-through simple. Se probo forzar este viewport a una resolucion menor
// que la que pide el motor (960x544) para bajar el costo de rasterizado --
// descartado (ver port_progress.md Fase 21): el costo caro de este motor
// esta en su render por software en CPU (framebuffer interno atado a
// GAME_W/H, no al viewport de GL), asi que forzar el viewport no daba
// ninguna mejora real de framerate, solo agregaba riesgo/complejidad. El
// motor ya pide el viewport correcto (coincide con GAME_W/SCREEN_W, ver
// NativeResize/glResize) -- pasar el valor tal cual.
static int g_viewport_log = 0;
void glViewport_wrapper(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (g_viewport_log < 10) {
        game_log("[GL] glViewport x=%d y=%d w=%d h=%d\n", x, y, width, height);
        g_viewport_log++;
    }
    glViewport(x, y, width, height);
}

// Pass-through deliberado: el motor manda una proyeccion CENTRADA
// (-400..400, -240..240) y sus vertices de quad de pantalla completa usan ese
// mismo sistema (GL_FIXED -400..400/-240..240, confirmado en los logs).
// Forzar aqui un sistema con origen en la esquina (0..800, 480..0) desplaza
// ese quad a un solo cuadrante de la pantalla e invierte Y — regresion real
// vista en consola (cuadro chico blanco sobre negro, log_1783657108).
void glOrthof_wrapper(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat zNear, GLfloat zFar) {
    game_log("[GL] glOrthof left=%.2f right=%.2f bottom=%.2f top=%.2f\n", left, right, bottom, top);
    glOrthof(left, right, bottom, top, zNear, zFar);
}

void glOrthox_wrapper(GLint left, GLint right, GLint bottom, GLint top, GLint zNear, GLint zFar) {
    game_log("[GL] glOrthox left=%d right=%d bottom=%d top=%d\n", left, right, bottom, top);
    // Convert from fixed point (Q16.16) to float
    glOrthof_wrapper(left / 65536.0f, right / 65536.0f, bottom / 65536.0f, top / 65536.0f, zNear / 65536.0f, zFar / 65536.0f);
}


// Stubs de C++/GCC
void __cxa_begin_cleanup() {}
void __cxa_call_unexpected() {}
int __cxa_guard_acquire(int* g) { return !*(char*)(g); }
void __cxa_guard_release(int* g) { *(char*)g = 1; }
void __cxa_type_match() {}
void __gnu_Unwind_Find_exidx() {}
void __stack_chk_fail() {}

// Registro de destructores estaticos de C++ (llamados normalmente al
// dlclose()/exit() de una libreria dinamica real). Este loader nunca
// descarga el .so ni llama exit() de forma ordenada -- el proceso termina
// con sceKernelExitProcess() -- asi que no hace falta ejecutar los
// destructores registrados: alcanza con no-ops que devuelvan éxito. Faltaban
// en la tabla de imports pese a estar confirmados en la Fase 1 del plan
// (objdump -T | grep UND), causando "Unknown symbol" y crash real en
// consola (ver port_progress.md).
int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) {
    (void) func; (void) arg; (void) dso_handle;
    return 0;
}
void __cxa_finalize(void *dso_handle) {
    (void) dso_handle;
}
int __aeabi_atexit(void *arg, void (*func)(void *), void *dso_handle) {
    return __cxa_atexit(func, arg, dso_handle);
}
int __stack_chk_guard = 0;

// pthread_mutex_t/pthread_cond_t en VitaSDK son en realidad PUNTEROS
// (typedef struct pthread_mutex_t_ * pthread_mutex_t;) a una estructura
// interna que aloca pthread_mutex_init(). El .so esta compilado contra
// Bionic (Android), donde un mutex/condvar estatico declarado con
// PTHREAD_MUTEX_INITIALIZER/PTHREAD_COND_INITIALIZER queda en CEROS sin
// llamar nunca a pthread_mutex_init en tiempo de ejecucion -- Bionic esta
// disenado para tratar esos ceros como "mutex valido, sin lockear" de
// forma nativa. En VitaSDK esos mismos ceros son un puntero NULL real, y
// pthread_mutex_lock/unlock de PTE (pthreads-embedded) lo desreferencian
// sin chequear, crasheando (confirmado en consola real: Data abort dentro
// de pthread_mutex_unlock, llamado desde el node-allocator interno de
// libstdc++ sobre un mutex estatico nunca inicializado -- ver
// port_progress.md). Se detecta el puntero NULL y se inicializa on-demand
// antes de usarlo, en vez de pasar el valor crudo a la implementacion real.
int pthread_mutex_lock_wrapper(pthread_mutex_t *mutex) {
    if (mutex && (*mutex == NULL || (intptr_t)*mutex == 0x4000)) {
        *mutex = NULL;
        pthread_mutex_init(mutex, NULL);
    }
    return pthread_mutex_lock(mutex);
}
int pthread_mutex_unlock_wrapper(pthread_mutex_t *mutex) {
    if (mutex && (*mutex == NULL || (intptr_t)*mutex == 0x4000)) {
        *mutex = NULL;
        pthread_mutex_init(mutex, NULL);
    }
    return pthread_mutex_unlock(mutex);
}
int pthread_mutex_destroy_wrapper(pthread_mutex_t *mutex) {
    if (!mutex || *mutex == NULL || (intptr_t)*mutex == 0x4000) return 0; // nunca se inicializo, nada que destruir
    return pthread_mutex_destroy(mutex);
}
int pthread_cond_wait_wrapper(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (cond && (*cond == NULL || (intptr_t)*cond == 0x4000)) {
        *cond = NULL;
        pthread_cond_init(cond, NULL);
    }
    if (mutex && (*mutex == NULL || (intptr_t)*mutex == 0x4000)) {
        *mutex = NULL;
        pthread_mutex_init(mutex, NULL);
    }
    return pthread_cond_wait(cond, mutex);
}
int pthread_cond_broadcast_wrapper(pthread_cond_t *cond) {
    if (cond && (*cond == NULL || (intptr_t)*cond == 0x4000)) {
        *cond = NULL;
        pthread_cond_init(cond, NULL);
    }
    return pthread_cond_broadcast(cond);
}

void* malloc_wrapper(size_t size) {
    void* ptr = malloc(size);
    if (!ptr) {
        game_log("[FakeJNI] MALLOC FAILED FOR SIZE %u\n", (unsigned int)size);
    }
    return ptr;
}
void free_wrapper(void* ptr) {
    free(ptr);
}
void* calloc_wrapper(size_t n, size_t size) {
    return calloc(n, size);
}
void* realloc_wrapper(void* ptr, size_t size) {
    return realloc(ptr, size);
}

void translate_path(const char* in_path, char* out_path, size_t out_size) {
    if (strncmp(in_path, "ux0:", 4) == 0) {
        strncpy(out_path, in_path, out_size);
        return;
    }
    const char* relative = in_path;
    if (strncmp(in_path, "app0:/", 6) == 0) {
        relative += 6;
    }
    while (*relative == '/') {
        relative++;
    }
    snprintf(out_path, out_size, "ux0:data/zenonia-4/assets/%s", relative);
}

FILE* fopen_hook(const char* path, const char* mode) {
    char new_path[256];
    translate_path(path, new_path, sizeof(new_path));
    game_log("[FakeJNI] fopen_hook: %s -> %s\n", path, new_path);
    return fopen(new_path, mode);
}

// struct stat con el layout de bionic (Android ARM 32-bit, NDK android-9) --
// NO es el de newlib/vitasdk. El motor lee directamente st_mode en el offset
// 16 y st_size en el 48 (confirmado en Zenonia 2 desensamblando
// MC_fsFileAttribute -- mismo motor y mismo consumidor aqui, ver
// out_ghidra.c:150281: los offsets que Ghidra decompila para el stat local
// coinciden con bionic). Pasarle el struct stat de newlib deja esos offsets
// con basura de stack: en Zenonia 2, al cargar una partida guardada, el
// "tamano" leido era un puntero del heap (MALLOC FAILED FOR SIZE 0x81340CE0)
// y el motor crasheaba -- fix portado ANTES de que el sintoma aparezca aqui
// (Zenonia2 port_progress.md §12.4).
typedef struct {
    uint64_t st_dev;         // 0
    uint8_t  __pad0[4];      // 8
    uint32_t __st_ino;       // 12
    uint32_t st_mode;        // 16  <- leido por el motor
    uint32_t st_nlink;       // 20
    uint32_t st_uid;         // 24
    uint32_t st_gid;         // 28
    uint64_t st_rdev;        // 32
    uint8_t  __pad3[4];      // 40 (+4 de alineacion implicita)
    int64_t  st_size;        // 48  <- leido por el motor
    uint32_t st_blksize;     // 56 (+4 de alineacion implicita)
    uint64_t st_blocks;      // 64
    uint32_t st_atime;       // 72
    uint32_t st_atime_nsec;  // 76
    uint32_t st_mtime;       // 80
    uint32_t st_mtime_nsec;  // 84
    uint32_t st_ctime;       // 88
    uint32_t st_ctime_nsec;  // 92
    uint64_t st_ino;         // 96
} bionic_stat_t;             // 104 bytes (el motor reserva espacio de sobra)

_Static_assert(__builtin_offsetof(bionic_stat_t, st_mode) == 16, "bionic st_mode");
_Static_assert(__builtin_offsetof(bionic_stat_t, st_size) == 48, "bionic st_size");

int stat_hook(const char* path, void* statbuf) {
    char new_path[256];
    translate_path(path, new_path, sizeof(new_path));

    struct stat st;
    int res = stat(new_path, &st);
    game_log("[FakeJNI] stat_hook: %s -> %s = %d (size=%ld)\n", path, new_path, res, res == 0 ? (long) st.st_size : -1L);
    if (res == 0 && statbuf) {
        bionic_stat_t *bst = (bionic_stat_t *) statbuf;
        memset(bst, 0, sizeof(*bst));
        bst->st_mode = st.st_mode; // los bits S_IFDIR/permisos son POSIX, coinciden
        bst->st_nlink = st.st_nlink;
        bst->st_uid = st.st_uid;
        bst->st_gid = st.st_gid;
        bst->st_size = st.st_size;
        bst->st_blksize = st.st_blksize;
        bst->st_blocks = st.st_blocks;
        bst->st_atime = st.st_atime;
        bst->st_mtime = st.st_mtime;
        bst->st_ctime = st.st_ctime;
        bst->__st_ino = st.st_ino;
        bst->st_ino = st.st_ino;
    }
    return res;
}

int access_hook(const char* path, int amode) {
    char new_path[256];
    translate_path(path, new_path, sizeof(new_path));
    game_log("[FakeJNI] access_hook: %s -> %s\n", path, new_path);
    return access(new_path, amode);
}

// --- Simbolos que Zenonia 4 importa y que la tabla heredada de Zenonia 3
// no cubria (diff real via `objdump -T libgameDSO.so | grep UND`, ver
// plan_zenonia4_port.md Fase 3: solo 8 simbolos nuevos sobre 95 totales).
// __assert2/__sF son de bionic; dlopen/dlsym/dlclose se registran como
// "no soportado" (default seguro segun psvita-porting/jni_stubs.md -- no se
// confirmo para que los llama el motor, asumir que ninguna ruta de arranque
// depende de que tengan exito); fprintf/strtok/pthread_key_delete son libc
// estandar que simplemente no hacia falta registrar en Zenonia 3.
void __assert2(const char *file, int line, const char *func, const char *expr) {
    game_log("[assert] %s:%d (%s): %s\n", file, line, func, expr);
    abort();
}

// bionic expone stdin/stdout/stderr como FILE* dentro de un arreglo __sF[3];
// newlib los expone como simbolos sueltos -- un arreglo wrapper alcanza
// porque el motor solo los reenvia a fprintf/fputs, nunca lee los campos
// internos del FILE. Inicializado en dynlib_init() (llamado desde main.c
// antes de so_relocate/so_resolve).
FILE *__sF[3];

void dynlib_init(void) {
    __sF[0] = stdin;
    __sF[1] = stdout;
    __sF[2] = stderr;
}

static void *dlopen_stub(const char *filename, int flag) {
    game_log("[dlopen] %s (flag=%d): no soportado, devolviendo NULL\n", filename ? filename : "(null)", flag);
    return NULL;
}

static void *dlsym_stub(void *handle, const char *symbol) {
    game_log("[dlsym] %s: no soportado, devolviendo NULL\n", symbol ? symbol : "(null)");
    return NULL;
}

static int dlclose_stub(void *handle) {
    (void) handle;
    return 0;
}

// Tabla de resolucion de imports. Confirmada 1:1 contra
// `objdump -T libgameDSO.so | grep UND` (95 simbolos, ver
// plan_zenonia4_port.md Fase 3) -- este motor SI importa dlopen/dlsym/
// dlclose/__assert2/__sF/fprintf/strtok/pthread_key_delete a diferencia de
// Zenonia 3, y (como Zenonia 3) no importa __android_log_print en este
// build -- si aparece en un build futuro alcanza con agregar la entrada, la
// funcion ya existe en main.c.
so_default_dynlib default_dynlib[] = {
    // C++ ABI / GCC
    { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
    { "__cxa_finalize", (uintptr_t)&__cxa_finalize },
    { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
    { "__cxa_begin_cleanup", (uintptr_t)&__cxa_begin_cleanup },
    { "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
    { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
    { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
    { "__cxa_type_match", (uintptr_t)&__cxa_type_match },
    { "__gnu_Unwind_Find_exidx", (uintptr_t)&__gnu_Unwind_Find_exidx },
    { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
    { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },

    // Libc - Memoria
    { "malloc", (uintptr_t)&malloc_wrapper },
    { "free", (uintptr_t)&free_wrapper },
    { "calloc", (uintptr_t)&calloc_wrapper },
    { "realloc", (uintptr_t)&realloc_wrapper },
    // optimization_ideas.md #4: NEON_LIBC_INTERCEPT (CMakeLists.txt) redirige
    // memcpy/memset al hot path de blit de software a neon_mem.c en vez de la
    // libc estandar de vitasdk -- sin confirmar en consola todavia (build
    // separado, ver build.sh --neon-libc-test), memmove no se toca (necesita
    // manejar solapamiento, fuera del alcance pedido).
#ifdef ZENONIA_NEON_LIBC
    { "memcpy", (uintptr_t)&neon_memcpy },
    { "memset", (uintptr_t)&neon_memset },
#else
    { "memcpy", (uintptr_t)&memcpy },
    { "memset", (uintptr_t)&memset },
#endif
    { "memmove", (uintptr_t)&memmove },
    { "memcmp", (uintptr_t)&memcmp },

    // Libc - String
    { "strcpy", (uintptr_t)&strcpy },
    { "strncpy", (uintptr_t)&strncpy },
    { "strcmp", (uintptr_t)&strcmp },
    { "strncmp", (uintptr_t)&strncmp },
    { "strcat", (uintptr_t)&strcat },
    { "strncat", (uintptr_t)&strncat },
    { "strlen", (uintptr_t)&strlen },
    { "strstr", (uintptr_t)&strstr },
    { "strchr", (uintptr_t)&strchr },

    // Libc - I/O
    { "fopen", (uintptr_t)&fopen_hook },
    { "fread", (uintptr_t)&fread },
    { "fwrite", (uintptr_t)&fwrite },
    { "fclose", (uintptr_t)&fclose },
    { "fseek", (uintptr_t)&fseek },
    { "ftell", (uintptr_t)&ftell },
    { "printf", (uintptr_t)&printf },
    { "vprintf", (uintptr_t)&vprintf },
    { "vsprintf", (uintptr_t)&vsprintf },
    { "sprintf", (uintptr_t)&sprintf },
    { "putchar", (uintptr_t)&putchar },
    { "puts", (uintptr_t)&puts },

    // Libc - Filesystem
    { "access", (uintptr_t)&access_hook },
    { "stat", (uintptr_t)&stat_hook },
    { "unlink", (uintptr_t)&unlink },
    { "rename", (uintptr_t)&rename },
    { "close", (uintptr_t)&close },
    { "fcntl", (uintptr_t)&fcntl },
    { "select", (uintptr_t)&select },

    // Libc - Tiempo y matematica
    { "time", (uintptr_t)&time },
    { "gettimeofday", (uintptr_t)&gettimeofday },
    { "localtime", (uintptr_t)&localtime },
    { "ceil", (uintptr_t)&ceil },
    { "atoi", (uintptr_t)&atoi },
    { "abort", (uintptr_t)&abort },
    { "exit", (uintptr_t)&exit },
    { "raise", (uintptr_t)&raise },

    // pthread (usado internamente por FalsoJNI y por el motor)
    { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init },
    { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_wrapper },
    { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_wrapper },
    { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_wrapper },
    { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_wrapper },
    { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_wrapper },
    { "pthread_key_create", (uintptr_t)&pthread_key_create },
    { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
    { "pthread_setspecific", (uintptr_t)&pthread_setspecific },

    // OpenGL ES 1.1 (mapeado a vitaGL)
    { "glActiveTexture", (uintptr_t)&glActiveTexture },
    { "glBindTexture", (uintptr_t)&glBindTexture_wrapper },
    { "glClear", (uintptr_t)&glClear },
    { "glClearColorx", (uintptr_t)&glClearColorx_wrapper },
    { "glColorPointer", (uintptr_t)&glColorPointer_wrapper },
    { "glColor4f", (uintptr_t)&glColor4f_wrapper },
    { "glColor4x", (uintptr_t)&glColor4x_wrapper }, /* Fase 49: antes apuntaba a glClearColorx_wrapper (bug: seteaba el clear color, no el color de vertice) */
    { "glDisable", (uintptr_t)&glDisable_wrapper },
    { "glDisableClientState", (uintptr_t)&glDisableClientState_wrapper },
    { "glDrawArrays", (uintptr_t)&glDrawArrays_wrapper },
    { "glEnable", (uintptr_t)&glEnable_wrapper },
    { "glEnableClientState", (uintptr_t)&glEnableClientState_wrapper },
    { "glBlendFunc", (uintptr_t)&glBlendFunc_wrapper },
    { "glAlphaFunc", (uintptr_t)&glAlphaFunc },
    { "glGenTextures", (uintptr_t)&glGenTextures },
    { "glHint", (uintptr_t)&glHint },
    { "glLoadIdentity", (uintptr_t)&glLoadIdentity_wrapper },
    { "glMatrixMode", (uintptr_t)&glMatrixMode_wrapper },
    { "glNormalPointer", (uintptr_t)&glNormalPointer },
    { "glOrthof", (uintptr_t)&glOrthof_wrapper },
    { "glOrthox", (uintptr_t)&glOrthox_wrapper },
    { "glTexCoordPointer", (uintptr_t)&glTexCoordPointer_wrapper },
    { "glTexEnvf", (uintptr_t)&glTexEnvf_wrapper },
    { "glTexImage2D", (uintptr_t)&glTexImage2D_wrapper },
    { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D_wrapper },
    { "glTexParameterx", (uintptr_t)&glTexParameterx_wrapper },
    { "glVertexPointer", (uintptr_t)&glVertexPointer_wrapper },
    { "glViewport", (uintptr_t)&glViewport_wrapper },

    // Red / Sockets
    { "socket", (uintptr_t)&socket },
    { "connect", (uintptr_t)&connect },
    { "send", (uintptr_t)&send },
    { "recv", (uintptr_t)&recv },
    { "shutdown", (uintptr_t)&shutdown },
    { "inet_addr", (uintptr_t)&inet_addr },

    // Libc core
    { "__errno", (uintptr_t)&__errno },

    // Nuevos en Zenonia 4 (ver nota arriba de la tabla)
    { "__assert2", (uintptr_t)&__assert2 },
    { "__sF", (uintptr_t)&__sF },
    { "fprintf", (uintptr_t)&fprintf },
    { "strtok", (uintptr_t)&strtok },
    { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
    { "dlopen", (uintptr_t)&dlopen_stub },
    { "dlsym", (uintptr_t)&dlsym_stub },
    { "dlclose", (uintptr_t)&dlclose_stub },
};

int default_dynlib_size = sizeof(default_dynlib);
