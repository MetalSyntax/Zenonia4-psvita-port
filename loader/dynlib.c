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
    if (p9 != 0 && (unsigned long) p9 < ZENONIA_MIN_PLAUSIBLE_HEAP_ADDR) {
        static int guard_log = 0;
        if (guard_log < 20) {
            game_log("[PutCompressImgGuard] param_9=0x%lx invalido (no NULL, no plausible) -- forzado a 0 (framebuffer principal), ver port_progress.md Fase 32\n", (unsigned long) p9);
            guard_log++;
        }
        p9 = 0;
    }
    so_unhook(&g_putcompressimg_hook);
    ((void (*)(int, int, int, int, unsigned char *, unsigned short *, int, int, long))
        (g_putcompressimg_hook.thumb_addr ? g_putcompressimg_hook.thumb_addr : g_putcompressimg_hook.addr))
        (p1, p2, p3, p4, p5, p6, op, p8, p9);
    g_putcompressimg_hook = hook_addr(
        g_putcompressimg_hook.thumb_addr ? g_putcompressimg_hook.thumb_addr : g_putcompressimg_hook.addr,
        (uintptr_t) PutCompressImg_hook);
}

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
// UN SOLO contador agregado (no uno por primitiva, para no inflar el log) --
// si sale alto, una ronda de seguimiento puede desglosar cual de las 13
// domina, mismo patron iterativo que la Fase 31.
volatile unsigned int g_cgsgraphics_calls_this_frame = 0;

#ifdef ZENONIA_INSTRUMENT_CGSGRAPHICS
static so_hook g_cgsgfx_drawline_hook;
static int CGsGraphics_DrawLine_counter_hook(void *this_ptr, int a, int b, int c, int d, unsigned int e) {
    g_cgsgraphics_calls_this_frame++;
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
    uint8x8_t a8 = vdup_n_u8(0xFF);
    for (; i < npix8; i += 8) {
        uint16x8_t p = vld1q_u16(src + i);

        uint16x8_t r5 = vshrq_n_u16(p, 11);                              // 0..31
        uint16x8_t g6 = vandq_u16(vshrq_n_u16(p, 5), vdupq_n_u16(0x3F));  // 0..63
        uint16x8_t b5 = vandq_u16(p, vdupq_n_u16(0x1F));                  // 0..31

        uint16x8_t r8_16 = vorrq_u16(vshlq_n_u16(r5, 3), vshrq_n_u16(r5, 2));
        uint16x8_t g8_16 = vorrq_u16(vshlq_n_u16(g6, 2), vshrq_n_u16(g6, 4));
        uint16x8_t b8_16 = vorrq_u16(vshlq_n_u16(b5, 3), vshrq_n_u16(b5, 2));

        uint8x8x4_t rgba;
        rgba.val[0] = vmovn_u16(r8_16);
        rgba.val[1] = vmovn_u16(g8_16);
        rgba.val[2] = vmovn_u16(b8_16);
        rgba.val[3] = a8;
        vst4_u8(dst + i * 4, rgba); // interleaved R,G,B,A directo a memoria
    }

    // Cola escalar (npix no multiplo de 8) -- misma formula de replicacion
    // de bits, para que no haya una costura visible entre el tramo NEON y
    // el escalar.
    for (; i < npix; i++) {
        uint16_t p = src[i];
        uint32_t r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
        uint32_t r = (r5 << 3) | (r5 >> 2);
        uint32_t g = (g6 << 2) | (g6 >> 4);
        uint32_t b = (b5 << 3) | (b5 >> 2);
        ((uint32_t *) dst)[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
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

// El motor sube su framebuffer interno de software (400x240, ver Fase 1 del
// plan) en RGB565 -- vitaGL no lo maneja igual que el motor original espera,
// asi que se convierte a RGBA8888 en CPU antes de subirlo. Mismo bug
// confirmado en hardware real para Zenonia 2 (mismo motor).
void glTexImage2D_wrapper(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {
    static int img_log = 0;
    if (img_log < 10) {
        game_log("[GL] glTexImage2D target=%x intFmt=%x w=%d h=%d format=%x type=%x pixels=%p\n",
                 target, internalformat, width, height, format, type, pixels);
        img_log++;
    }

    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) {
        g_rgb565_uploads_this_frame++;
    }

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

#ifdef ZENONIA_RGB565_DIRECT
#ifdef ZENONIA_DIRTY_RECT_UPLOAD
    if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5 && pixels) {
        upload_rgb565_dirty_rect(target, level, xoffset, yoffset, width, height, format, type, pixels);
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

    glDrawArrays(mode, first, count);
}

void glTexEnvf_wrapper(GLenum target, GLenum pname, GLfloat param) {
    game_log("[GL] glTexEnvf target=%x pname=%x param=%f ui_status=%d\n", target, pname, param, g_ui_status);
    glTexEnvf(target, pname, param);
}

void glBlendFunc_wrapper(GLenum sfactor, GLenum dfactor) {
    static int log = 0;
    if (log < 20) {
        game_log("[GL] glBlendFunc sfactor=%x dfactor=%x ui_status=%d\n", sfactor, dfactor, g_ui_status);
        log++;
    }
    glBlendFunc(sfactor, dfactor);
}

void glEnable_wrapper(GLenum cap) {
    static int enable_log = 0;
    if (enable_log < 20) {
        game_log("[GL] glEnable cap=%x\n", cap);
        enable_log++;
    }
    glEnable(cap);
}

void glDisable_wrapper(GLenum cap) {
    static int disable_log = 0;
    if (disable_log < 20) {
        game_log("[GL] glDisable cap=%x\n", cap);
        disable_log++;
    }
    glDisable(cap);
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

    if (type == GL_FIXED) {
        pending_fixed_texcoords = (const int32_t *)pointer;
        pending_fixed_texcoord_size = size;
        pending_fixed_texcoord_stride = stride;
        return;
    }

    pending_fixed_texcoords = NULL;
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

    if (type == GL_FIXED) {
        pending_fixed_colors = (const int32_t *)pointer;
        pending_fixed_color_size = size;
        pending_fixed_color_stride = stride;
        return;
    }

    pending_fixed_colors = NULL;
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

    if (type == GL_FIXED) {
        pending_fixed_verts = (const int32_t *)pointer;
        pending_fixed_size = size;
        pending_fixed_stride = stride;
        return;
    }

    pending_fixed_verts = NULL;
    glVertexPointer(size, type, stride, pointer);
}

void glEnableClientState_wrapper(GLenum array) {
    static int enable_cs_log = 0;
    if (enable_cs_log < 10) {
        game_log("[GL] glEnableClientState array=%x\n", array);
        enable_cs_log++;
    }
    glEnableClientState(array);
}

void glDisableClientState_wrapper(GLenum array) {
    static int disable_cs_log = 0;
    if (disable_cs_log < 10) {
        game_log("[GL] glDisableClientState array=%x\n", array);
        disable_cs_log++;
    }
    glDisableClientState(array);
}

void glMatrixMode_wrapper(GLenum mode) {
    static int log = 0;
    if (log < 10) {
        game_log("[GL] glMatrixMode mode=%x\n", mode);
        log++;
    }
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
    { "glBindTexture", (uintptr_t)&glBindTexture },
    { "glClear", (uintptr_t)&glClear },
    { "glClearColorx", (uintptr_t)&glClearColorx_wrapper },
    { "glColorPointer", (uintptr_t)&glColorPointer_wrapper },
    { "glColor4f", (uintptr_t)&glColor4f },
    { "glColor4x", (uintptr_t)&glClearColorx_wrapper }, /* using same logic as glClearColorx for now */
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
