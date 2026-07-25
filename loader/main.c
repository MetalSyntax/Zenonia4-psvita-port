/*
 * main.c
 *
 * ARMv7 Shared Libraries loader. Zenonia 4.
 *
 * Mismo motor (Gamevil Nexus2/"Clet") que Zenonia 2 y Zenonia 3 -- ver
 * ../Zenonia3-vita/loader/main.c (base de este archivo) y
 * plan_zenonia4_port.md para el diff de ABI/simbolos confirmado contra
 * Zenonia 3 (exports nuevos: IAB, Tapjoy, Gamevil Live, seleccion de slot,
 * timer asincrono -- ver java.c).
 */

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/io/dirent.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "debugScreen.h"
#include "so_util.h"
#include "audio.h"
#include "androidui.h"

// Override vitaGL splash screen (strong symbol overrides the one in libvitaGL.a)
void invoke_splashscreen(void) {}

#ifdef ZENONIA_GPU_QUADS
#include "gpu_quads.h"
#endif
#include <taihen.h>
#include <vitaGL.h>
#include <falso_jni/FalsoJNI.h>

#define printf psvDebugScreenPrintf
#define LOG_DIR "ux0:data/zenonia-4/logs"

// GAME_W/H/SCREEN_W/H = resolucion que el motor CREE que tiene (pasada a
// NativeInitDeviceInfo/NativeInitWithBufferSize/NativeResize) -- fijas en
// 960x544 (nativo de Vita). Confirmado en consola, en CUATRO intentos
// distintos de bajar la resolucion de render (ver port_progress.md Fases
// 16-21), que:
//   1) Bajar GAME_W/H directamente (probado con 480x272) rompe la propia
//      matematica de camara/zoom del motor -- se ve todo "zoomeado" sin
//      importar como se escale despues el resultado final a pantalla (se
//      probaron 3 mecanismos de escalado distintos con el mismo sintoma).
//   2) Dejar GAME_W/H en 960x544 pero forzar un glViewport real mas chico
//      (via un FBO intermedio) SI evita el zoom, pero NO da ninguna mejora
//      de framerate (confirmado: sigue en ~5 FPS) -- el costo caro de este
//      motor esta en el render por software que hace en CPU hacia su
//      framebuffer interno (atado a GAME_W/H) ANTES de cualquier llamada
//      GL, no en lo que la GPU rasteriza al final. Forzar el viewport no
//      toca ese costo para nada.
// Conclusion: GAME_W/H/SCREEN_W/H no se pueden bajar (rompen el juego) y
// tampoco ayuda fingir un viewport mas chico (no ataca el costo real) -- se
// abandono el enfoque de "bajar resolucion" por completo. La ganancia de
// framerate se busca ahora en scePowerSet*ClockFrequency (gl_init()).
#ifdef ZENONIA_DOWNSAMPLE
// Variante 3 (--downsample-test): resolucion INTERNA reducida.
//
// Advertencia honesta (verificada en el codigo decompilado, no teoria): este
// motor NO separa "campo de vision" de "resolucion" -- la camara muestra
// exactamente deviceW x deviceH pixeles de mundo, dibujados 1:1, y el blitter
// EXIGE buffer==deviceInfo (ver out_ghidra.c: posiciones absolutas, stride del
// struct global de screen buffer, sin factor de escala mundo->pantalla, ni zoom
// de camara). Por eso bajar la resolucion interna necesariamente ZOOMEA la
// vista (se ve menos mundo, magnificado), como ya observo el usuario en los 4
// intentos previos (main.c historico / port_progress.md Fases 16-21). Esta
// variante NO resuelve eso -- lo reproduce a un ratio configurable y suave, y
// deja que vitaGL haga el upscale bilinear a 960x544 en GPU (el unico aporte
// nuevo vs los intentos previos). Sirve para que el usuario juzgue en consola
// si un zoom mas leve + suavizado por GPU es un tradeoff aceptable de FPS.
//
// El AHORRO real de CPU es proporcional: a NUM/DEN=2/3 el motor rasteriza
// (2/3)^2 = ~44% de los pixeles -> ~56% menos trabajo de blit por software.
// Todo lo demas (touch, androidui, ortho) cascadea solo porque ya se calcula
// en terminos de GAME_W/SCREEN_W (ver la conversion de touch mas abajo).
#ifndef ZENONIA_DS_NUM
#define ZENONIA_DS_NUM 2
#endif
#ifndef ZENONIA_DS_DEN
#define ZENONIA_DS_DEN 3
#endif
#define GAME_W ((960 * ZENONIA_DS_NUM) / ZENONIA_DS_DEN)
#define GAME_H ((544 * ZENONIA_DS_NUM) / ZENONIA_DS_DEN)
#define SCREEN_W GAME_W
#define SCREEN_H GAME_H
#else
#define GAME_W 960
#define GAME_H 544
#define SCREEN_W 960
#define SCREEN_H 544
#endif

FILE *log_file = NULL;

// Una vez que vitaGL toma la pantalla, no se debe seguir escribiendo al
// framebuffer crudo de debugScreen -- ambos competirian por el mismo buffer.
int gl_active = 0;

int _newlib_heap_size_user = 128 * 1024 * 1024; // 128 MB para newlib (malloc)
unsigned int sceLibcHeapSize = 4 * 1024 * 1024; // 4 MB para SCE Libc (system libs)

// Un archivo de log por corrida, con timestamp, dentro de logs/ -- mantiene
// el historial completo entre pruebas en vez de pisar siempre el mismo
// log.txt (ver psvita-porting skill, hardware_debugging.md).
void init_log() {
    sceIoMkdir(LOG_DIR, 0777); // falla en silencio si ya existe

    char log_path[256];
    time_t t = time(NULL);
    snprintf(log_path, sizeof(log_path), LOG_DIR "/log_%u.txt", (unsigned int) t);

    log_file = fopen(log_path, "w");
    if (log_file) {
        fprintf(log_file, "--- ZENONIA 4 PORT LOG START (%s) ---\n", log_path);
        fflush(log_file);
    }
}

void game_log(const char *fmt, ...) {
    va_list list;
    // Fase 94 (port_progress.md): la linea periodica de "frame N alive" crecio
    // organicamente durante muchas fases (Fase 27/29/31/36/41/48/77/86/92,
    // cada una agregando un campo mas) hasta superar los 512 bytes SIN que
    // nadie lo notara -- vsnprintf trunca en silencio (no hay overflow, pero
    // tampoco error visible), asi que TODO lo que quedaba despues del corte
    // (avg_sprite_cache_hits/misses de la Fase 92, y ya desde la Fase 86,
    // total_texturas_creadas/touch.reportNum/pad.buttons/ui_status) nunca
    // llego a ningun log real -- confirmado contando bytes contra un log real
    // del usuario (534/608 caracteres segun la fase, contra el limite de 512).
    // 2048 da margen real para que la linea siga creciendo varias fases mas
    // sin repetir este bug.
    char string[2048];

    va_start(list, fmt);
    vsnprintf(string, sizeof(string), fmt, list);
    va_end(list);

    if (log_file) {
        fprintf(log_file, "%s", string);
        // fflush(log_file); // ELIMINADO: esto causa un cuello de botella de I/O brutal (10 FPS)
    }
}

void fatal_error(const char *fmt, ...) {
    va_list list;
    char string[2048]; // Fase 94: mismo motivo que game_log(), ver su comentario

    va_start(list, fmt);
    vsnprintf(string, sizeof(string), fmt, list);
    va_end(list);

    game_log("[FATAL] %s\n", string);
    // La pantalla de debug se inicializa solo aca: en un arranque sano no se
    // muestra nunca texto de consola.
    psvDebugScreenInit();
    printf("[FATAL] %s\n", string);
    sceKernelDelayThread(10 * 1000 * 1000); // 10s para poder leerlo antes de morir
    sceKernelExitProcess(0);
}

so_module zenonia4_mod;

// No confirmado que el .so de Zenonia 3 lo importe (ver Fase 1 del plan: no
// aparece en `objdump -T | grep UND`), pero se deja disponible por si algun
// build futuro lo necesita -- no hace dano tenerlo sin registrar en
// dynlib.c.
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
	va_list list;
	char string[2048]; // Fase 94: mismo motivo que game_log(), ver su comentario

	va_start(list, fmt);
	vsnprintf(string, sizeof(string), fmt, list);
	va_end(list);

	game_log("[ANDROID] %s: %s\n", tag, string);
	return 0;
}

extern so_default_dynlib default_dynlib[];
extern int default_dynlib_size;

// Punteros a funciones JNI del juego. A diferencia de Zenonia 2: no hay
// NativeInit (no existe en este .so) ni setInputEvent (idem) -- ver tabla
// de diferencias de ABI en plan_zenonia3_port.md.
int (* Game_JNI_OnLoad)(void *vm, void *reserved);
void (* NativeInitDeviceInfo)(void *env, void *obj, int w, int h);
void (* NativeInitWithBufferSize)(void *env, void *obj, int w, int h);
void (* NativeRender)(void *env, void *obj);
void (* NativeResize)(void *env, void *obj, int w, int h);
void (* NativeResumeClet)(void *env, void *obj);
void (* handleCletEvent)(void *env, void *obj, int type, int p1, int p2, int p3);

// --- Input: protocolo pendiente de reconfirmar contra el Java real de
// Zenonia 3 (Fase 5 del plan -- decompilar ZenoniaUIControllerView y el
// NexusHal equivalente, no asumir que es igual a Zenonia2UIControllerView).
// Se arranca con los mismos codigos MH_* y keycodes HAL de Zenonia 2 porque
// son constantes del MOTOR (Nexus2/Clet), no de la capa de juego -- pero
// deben confirmarse con un log real antes de dar esto por bueno.
//
// Diferencia real de ABI: no existe setInputEvent en este .so. Solo hay
// handleCletEvent, que ahora toma 4 ints (antes 3) -- ver Fase 1 del plan y
// out_ghidra.c:147900. Se sigue encolando un evento por frame y entregandolo
// justo antes de NativeRender (mismo orden que NexusGLRenderer.drawFrame en
// Zenonia 2), sin la entrega inmediata que hacia setInputEvent, porque acá
// no existe ese canal.

#define MH_KEY_PRESSEVENT       2
#define MH_KEY_RELEASEEVENT     3
#define MH_POINTER_PRESSEVENT   23
#define MH_POINTER_RELEASEEVENT 24
#define MH_POINTER_MOVEEVENT    25

#define HAL_KEY_UP    (-1)
#define HAL_KEY_DOWN  (-2)
#define HAL_KEY_LEFT  (-3)
#define HAL_KEY_RIGHT (-4)
#define HAL_KEY_OK    (-5)
#define HAL_KEY_MAP   (-6)
#define HAL_KEY_SAVE  (-10)
#define HAL_KEY_BACK  (-16)
#define HAL_KEY_SKIP  (35)

// Constantes reales de NexusHal.java -- el boton "Continuar" del menu no
// manda un press/release de tecla como los demas, manda un UNICO evento con
// este tipo/parametro (ver Natives.java: img_menu_continue.setOnClickListener
// -> handleCletEvent(NexusHal.REPLY_YESNO, NexusHal.FIRST_MOVE_REPLY_PAGE, 0, 0)).
#define NEXUS_HAL_REPLY_YESNO           19450815
#define NEXUS_HAL_FIRST_MOVE_REPLY_PAGE 20010913
// Los botones "escribir resena"/"mas tarde" de la propia pantalla de
// REPLY_PAGE (ver Natives.java: showReplyMoveComponent(), img_btn_write/
// img_btn_later OnClickListener) mandan el mismo tipo REPLY_YESNO con estos
// otros dos parametros.
#define NEXUS_HAL_YES_MOVE_REPLY_PAGE   20010911
#define NEXUS_HAL_NO_MOVE_REPLY_PAGE    20010912

#define UI_STATUS_EXIT 104

typedef struct { int type, p1, p2, p3; } input_event;
static input_event event_queue[16];
static int eq_head = 0, eq_tail = 0;

static void queue_input_event(int type, int p1, int p2, int p3) {
    static int in_log = 0;
    if (in_log < 40) {
        game_log("[INPUT] event type=%d p1=%d p2=%d p3=%d\n", type, p1, p2, p3);
        in_log++;
    }
    int next = (eq_tail + 1) % 16;
    if (next != eq_head) {
        event_queue[eq_tail].type = type;
        event_queue[eq_tail].p1 = p1;
        event_queue[eq_tail].p2 = p2;
        event_queue[eq_tail].p3 = p3;
        eq_tail = next;
    }
}

static const struct { unsigned int btn; int hal; } btn_map[] = {
    { SCE_CTRL_UP,       HAL_KEY_UP },
    { SCE_CTRL_DOWN,     HAL_KEY_DOWN },
    { SCE_CTRL_LEFT,     HAL_KEY_LEFT },
    { SCE_CTRL_RIGHT,    HAL_KEY_RIGHT },
    { SCE_CTRL_CROSS,    HAL_KEY_OK },
    { SCE_CTRL_CIRCLE,   HAL_KEY_BACK },
    { SCE_CTRL_TRIANGLE, HAL_KEY_SKIP },
    { SCE_CTRL_SQUARE,   HAL_KEY_MAP },
    { SCE_CTRL_LTRIGGER, HAL_KEY_SAVE },
};
#define BTN_MAP_COUNT (sizeof(btn_map) / sizeof(btn_map[0]))

// NOTA: a diferencia de Zenonia 2, aca NO se porta ningun parche binario
// (apply_so_patches). El bug de Zenonia2 (puntero de heap tratado como
// signed en CMvLayerData::PreLoad) es un patron de motor viejo que puede
// reaparecer en este .so, pero en un offset DISTINTO -- no reusar el numero
// 0xaec38 a ciegas. Si aparece el mismo sintoma (NULL "imposible" en datos
// que deberian haberse cargado), re-derivar el offset con vita-parse-core +
// objdump -d sobre ESTE binario (ver Fase 7 del plan).

extern volatile int g_ui_status;
extern volatile unsigned int g_rgb565_uploads_this_frame;
extern volatile unsigned int g_drawnative_calls_this_frame;
extern volatile unsigned int g_cgxpzxani_draw_calls_this_frame;
extern volatile unsigned int g_cgxpzxzero_blt_calls_this_frame;
extern volatile unsigned int g_putcompressimg_calls_this_frame;
extern volatile unsigned int g_cgsgraphics_calls_this_frame;
extern void zenonia_log_cgsgraphics_breakdown(unsigned int window_frames);
extern volatile unsigned int g_dirtyrect_rows_uploaded_this_frame;
extern volatile unsigned int g_dirtyrect_rows_total_this_frame;
extern volatile unsigned int g_interlaced_rows_uploaded_this_frame;
extern volatile unsigned int g_interlaced_rows_total_this_frame;
extern volatile unsigned int g_gpu_quads_calls_this_frame;
extern volatile unsigned int g_gpu_quads_op0_calls_this_frame;
extern volatile unsigned int g_gpu_quads_cache_hit_this_frame;
extern volatile unsigned int g_gpu_quads_decoded_this_frame;
extern volatile unsigned int g_drawtoframebuffer_calls_this_frame;
extern volatile unsigned int g_sprite_cache_hits_this_frame;
extern volatile unsigned int g_sprite_cache_misses_this_frame;
extern void zenonia_install_text_calls_probe(so_module *mod);
extern void zenonia_install_array_hooks(void);
extern void zenonia_install_hide_dpad_hook(so_module *mod);
extern void zenonia_install_drawnative_probe(so_module *mod);
extern void zenonia_install_hotpath_probe(so_module *mod);
#ifdef ZENONIA_GPU_QUADS
extern void zenonia_install_text_quads_hook(so_module *mod);
extern void zenonia_install_screen_fill_quads_hook(so_module *mod);
#endif
extern void zenonia_install_cgsgraphics_probe(so_module *mod);
extern void zenonia_install_fastclear_optimize(so_module *mod);
extern void zenonia_install_sprite_decode_optimize(so_module *mod);
extern void zenonia_install_text_native_optimize(so_module *mod);


static GLuint splash_tex = 0;

static void splash_load(void) {
    FILE *f = fopen("app0:splash.rgba", "rb");
    if (!f) {
        game_log("splash: app0:splash.rgba no encontrado (ok si todavia no se generaron los assets de LiveArea)\n");
        return;
    }
    void *data = malloc(960 * 544 * 4);
    if (!data) { fclose(f); return; }
    fread(data, 1, 960 * 544 * 4, f);
    fclose(f);

    glGenTextures(1, &splash_tex);
    glBindTexture(GL_TEXTURE_2D, splash_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 960, 544, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    free(data);
}

static void splash_draw(void) {
    if (!splash_tex) return;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, splash_tex);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    static const float verts[] = { 0, 0,  960, 0,  0, 544,  960, 544 };
    static const float uvs[]   = { 0, 0,  1, 0,    0, 1,    1, 1 };
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

void log_active_frame_buf(const char *label) {
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    int ret = sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    game_log("[DISPLAY] %s: sceDisplayGetFrameBuf ret=0x%08x base=%p w=%d h=%d pitch=%d\n",
             label, ret, fb.base, fb.width, fb.height, fb.pitch);
}

// --- Intento de FBO interno + glViewport forzado: DESCARTADO ---
//
// Historial completo en port_progress.md Fases 16-21. Bajar GAME_W/H (lo que
// el motor CREE que mide su lienzo) SI reducia trabajo real y subia el
// framerate (18 FPS), pero le rompe al motor su propia matematica de
// camara/zoom (confirmado en consola en 3 mecanismos de escalado distintos).
// La idea de esta fase era: dejar GAME_W/H en 960x544 (sin zoom) pero forzar
// el `glViewport` real a una resolucion menor via un FBO intermedio, total
// glViewport es independiente de la proyeccion ortografica del motor.
//
// Resultado real en consola: **de vuelta a ~5 FPS**, sin zoom pero tan lento
// como el 960x544 original. Conclusion (confirmada, no teorica): el costo
// caro de este motor NO esta en lo que la GPU rasteriza para el quad final
// (eso es barato sea cual sea el viewport) -- esta en el RENDER POR SOFTWARE
// que el motor hace en CPU hacia su framebuffer interno (conversion
// RGB565->RGBA8888 de ~522k pixeles, ver dynlib.c) ANTES de que exista
// ninguna llamada GL de por medio. Ese framebuffer interno esta atado a
// GAME_W/H (lo que se le paso a NativeInitDeviceInfo), no al viewport --
// forzar el viewport de la GPU no toca ese costo para nada. Osea: no hay
// forma de bajar la resolucion de render sin que el motor sepa, porque el
// costo real ocurre ANTES de la etapa que se puede interceptar con GL.
//
// Se descarta el FBO/glViewport forzado (agregaba una pasada extra sin
// ningun beneficio real) y se vuelve a render directo simple. La ganancia de
// framerate ahora se busca en otro lado: ver scePowerSet*ClockFrequency en
// gl_init() (reloj de CPU/GPU/bus, la Vita corre por debajo del maximo por
// defecto -- optimizacion estandar y de bajo riesgo en homebrew, sin tocar
// nada de la logica del motor).

void gl_init() {
    // La Vita arranca con el reloj de CPU/GPU/bus por debajo del maximo por
    // defecto (perfil de ahorro de energia) -- subirlo al maximo documentado
    // es una optimizacion estandar en homebrew de Vita (RetroArch, muchos
    // ports), segura porque no cambia nada de la logica del motor ni del
    // loader, solo la velocidad de reloj real del hardware. Valores maximos
    // conocidos: ARM 444MHz (default ~333), BUS 222MHz (default ~166),
    // GPU 222MHz (default ~166), GPU Xbar 166MHz (default ~166, sin cambio
    // real pero se deja explicito por consistencia).
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    game_log("[POWER] Reloj subido: ARM=444MHz BUS=222MHz GPU=222MHz GPUXbar=166MHz\n");

    // optimization_ideas.md #6 (Fase 36): fijar el hilo principal (el que
    // corre este render loop) al nucleo 0 -- optimizacion estandar de
    // homebrew en Vita (misma familia que el boost de reloj de arriba: segura,
    // no cambia logica del motor ni del loader, solo evita que el scheduler
    // mueva este hilo entre nucleos mientras el mezclador de audio (audio.c,
    // ver audio_init) corre fijo en otro). Los 3 nucleos de usuario
    // disponibles son SCE_KERNEL_CPU_MASK_USER_0/1/2 (el 4to esta reservado
    // por el sistema). Sin gate detras de ningun flag -- mismo criterio que
    // el boost de reloj, no un experimento de riesgo.
    int main_thread_id = sceKernelGetThreadId();
    int affinity_res = sceKernelChangeThreadCpuAffinityMask(main_thread_id, SCE_KERNEL_CPU_MASK_USER_0);
    game_log("[PERF] Afinidad de CPU: hilo principal (0x%08x) -> nucleo 0 (res=0x%08x)\n",
             main_thread_id, affinity_res);

    // Sin MSAA / sin triple buffering: config confirmada funcionando en
    // hardware real para el mismo motor (Zenonia 2) y para Prince of Persia.
    vglUseTripleBuffering(GL_FALSE);
    // vitaGL a GAME_W/H (960x544, ver nota junto a esas macros) -- render
    // directo, sin FBO intermedio (descartado arriba, sin beneficio real).
    vglInitExtended(0, GAME_W, GAME_H, 6 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);

#ifdef ZENONIA_DISABLE_VSYNC
    // optimization_ideas.md #9, prueba diagnostica SOLO para medir: descarta
    // vglSwapBuffers() esperando vblank como techo artificial del FPS actual
    // (~9-11 FPS). Causa tearing, no es un default -- ver DISABLE_VSYNC en
    // CMakeLists.txt y build.sh --no-vsync-test. Sin confirmar todavia en
    // consola si esto cambia algo (a estos FPS tan bajos, es posible que
    // sceDisplayWaitVblankStart() ya este retornando casi inmediato).
    vglWaitVblankStart(GL_FALSE);
    game_log("[PERF] VSync wait deshabilitado (DISABLE_VSYNC) -- posible tearing, build de diagnostico\n");
#endif

    gl_active = 1;
}

int main() {
    init_log();
	game_log("Iniciando Zenonia 4 port (SoLoader)\n");
#ifdef ZENONIA_BUILD_VARIANT
	// Fase 85 (port_progress.md): identifica el build variant en la
	// PRIMERA linea del log (sobrevive incluso a un log cortado a mitad de
	// sesion) -- antes no habia forma de distinguir p.ej.
	// --sprite-decode-test de --sprite-decode-exalpha-test desde el log
	// solo (ningun flag propio deja una linea de instalacion distinta).
	game_log("[Build] variant=%s\n", ZENONIA_BUILD_VARIANT);
#endif

	int res = so_file_load(&zenonia4_mod, "ux0:data/zenonia-4/libgameDSO.so", 0x98000000);
	if (res < 0) {
		game_log("Error critico cargando libgameDSO.so: 0x%08X\n", res);
        sceKernelDelayThread(5000000);
	} else {
		game_log("Libreria cargada con exito.\n");
		game_log("mod: text_base=0x%08x num_dynsym=%d dynsym=%p dynstr=%p hash=%p soname=%s\n",
			(unsigned int) zenonia4_mod.text_base, zenonia4_mod.num_dynsym,
			(void*)zenonia4_mod.dynsym, (void*)zenonia4_mod.dynstr, (void*)zenonia4_mod.hash,
			zenonia4_mod.soname ? zenonia4_mod.soname : "(null)");

		so_relocate(&zenonia4_mod);
		so_resolve(&zenonia4_mod, default_dynlib, default_dynlib_size, 0);

		// Parche binario para CMvLayerData::PreLoad (comparacion de puntero de
		// heap con signo -- confirmado como bug de motor en Zenonia 2 y
		// Zenonia 3, cada uno en un offset DISTINTO segun su propio build).
		// El offset 0x9a7b4 es el de Zenonia 3 -- casi seguro NO es el
		// correcto para este .so (1.63MB vs 1.28MB de Zenonia 3, direcciones
		// no comparables). El chequeo de bytes de abajo hace que esto sea
		// seguro de dejar como esta: si los bytes no coinciden, no parchea
		// nada (ver log de "no coincide"). Re-derivar el offset real de
		// Zenonia 4 con Ghidra/vita-parse-core en cuanto haya un crash real
		// que apunte a esta clase de bug (ver plan_zenonia4_port.md #4).
		uint32_t text_base = zenonia4_mod.text_base;
		if (*(uint16_t *)(text_base + 0x9a7b4) == 0xdd24) {
			uint16_t patch_beq = 0xd024;
			kuKernelCpuUnrestrictedMemcpy((void *)(text_base + 0x9a7b4), &patch_beq, 2);
			game_log("Parche aplicado: CMvLayerData::PreLoad (ble -> beq)\n");
		} else {
			game_log("Parche CMvLayerData::PreLoad: offset heredado de Zenonia 3 no coincide en este .so (esperado, direcciones distintas) -- sin parchear, pendiente re-derivar el offset real\n");
		}
		so_flush_caches(&zenonia4_mod);
		so_initialize(&zenonia4_mod);

		game_log("SoLoader inicializado. Iniciando vitaGL...\n");
		gl_init();
		game_log("vitaGL inicializado.\n");
		audio_init();
		splash_load();
		// androidui_load() intentaba cargar ~16 .rgba + 2 .html especificos
		// de Zenonia 3 (nombres confirmados en DOS logs reales que nunca
		// existen para Zenonia 4 -- el logo/titulo/menu de este juego se
		// dibuja nativamente via assets/menu/*.pzx, no via overlay Android,
		// ver port_progress.md Fase 1/9). Se salta la carga (18 fopen()
		// fallidos + sus logs en cada arranque) -- las texturas quedan en 0
		// exactamente igual que si la carga fallara, asi que
		// androidui_draw()/los hit-test de touch (que SI se mantienen, por
		// si la navegacion de menu depende de ellos) se comportan identico.
		// Sacar este comentario y reactivar la llamada si en algun momento
		// se generan assets Android-UI reales para Zenonia 4 (por ejemplo
		// para las pantallas nuevas de Gamevil Live/IAP).

		jni_init();
		zenonia_install_array_hooks(); // ver java.c -- reconoce nuestros bloques readAssets en GetArrayLength/GetByteArrayElements
		JNIEnv *jniEnv = &jni;

		Game_JNI_OnLoad = (void *)so_symbol(&zenonia4_mod, "JNI_OnLoad");
		NativeInitDeviceInfo = (void *)so_symbol(&zenonia4_mod, "Java_com_gamevil_nexus2_Natives_NativeInitDeviceInfo");
		NativeInitWithBufferSize = (void *)so_symbol(&zenonia4_mod, "Java_com_gamevil_nexus2_Natives_NativeInitWithBufferSize");
		NativeRender = (void *)so_symbol(&zenonia4_mod, "Java_com_gamevil_nexus2_Natives_NativeRender");
		NativeResize = (void *)so_symbol(&zenonia4_mod, "Java_com_gamevil_nexus2_Natives_NativeResize");
		NativeResumeClet = (void *)so_symbol(&zenonia4_mod, "Java_com_gamevil_nexus2_Natives_NativeResumeClet");
		handleCletEvent = (void *)so_symbol(&zenonia4_mod, "Java_com_gamevil_nexus2_Natives_handleCletEvent");
		zenonia_install_hide_dpad_hook(&zenonia4_mod); // ver dynlib.c -- oculta cruceta+botones de accion (HIDE_VIRTUAL_GAMEPAD)
		zenonia_install_drawnative_probe(&zenonia4_mod); // ver dynlib.c -- Fase 29, cuenta llamadas a DrawNative (INSTRUMENT_DRAWNATIVE_CALLS)
		zenonia_install_hotpath_probe(&zenonia4_mod); // ver dynlib.c -- Fase 31, cuenta CGxPZxAni::Draw/CGxPZxZero::Blt/PutCompressImg (INSTRUMENT_HOTPATH_CALLS)
		zenonia_install_cgsgraphics_probe(&zenonia4_mod); // ver dynlib.c -- Fase 36, cuenta ~13 primitivas de CGsGraphics:: (INSTRUMENT_CGSGRAPHICS_CALLS)
		zenonia_install_text_calls_probe(&zenonia4_mod); // ver dynlib.c -- Fase 86, cuenta CGxFACharCache::DrawToFrameBuffer (INSTRUMENT_TEXT_CALLS)
		zenonia_install_fastclear_optimize(&zenonia4_mod); // ver dynlib.c -- Fase 41, reimplementacion real (no solo conteo) de FastClearNonClipping (FASTCLEAR_NEON_OPTIMIZE)
		zenonia_install_sprite_decode_optimize(&zenonia4_mod); // ver sprite_decode.c -- Fase 43, reemplaza RLE ASM por C nativo
		zenonia_install_text_native_optimize(&zenonia4_mod); // ver sprite_decode.c -- Fase 87, texto nativo C

#ifdef ZENONIA_GPU_QUADS
		zenonia_install_text_quads_hook(&zenonia4_mod); // ver dynlib.c -- Fase 59, desvia CGxFACharCache::DrawToFrameBuffer (texto) a la misma cola de quads que los sprites
		zenonia_install_screen_fill_quads_hook(&zenonia4_mod); // ver dynlib.c -- Fase 61, desvia Screen_RGBBLEND16_Real (rellenos solidos con alpha) a la misma cola de quads
#endif

		game_log("Symbols: JNI_OnLoad=%p NativeInitDeviceInfo=%p NativeInitWithBufferSize=%p NativeRender=%p NativeResize=%p NativeResumeClet=%p handleCletEvent=%p\n",
			(void*)Game_JNI_OnLoad, (void*)NativeInitDeviceInfo, (void*)NativeInitWithBufferSize,
			(void*)NativeRender, (void*)NativeResize, (void*)NativeResumeClet, (void*)handleCletEvent);

		// Secuencia de arranque -- reemplaza al NativeInit() unico de
		// Zenonia 2 (que no existe aca).
		//
		// ORDEN CONFIRMADO CON UN CRASH REAL (ver port_progress.md Fase 3,
		// bug #2): NativeInitWithBufferSize DEBE llamarse ANTES que
		// NativeInitDeviceInfo. NativeInitWithBufferSize dispara
		// startClet() -> GxCreateGlobalHeap() -> Gcx_MM_Init(), que crea el
		// pool de memoria PROPIO del motor (un allocator custom tipo slab,
		// separado de malloc/new) del que depende Gcx_MM_Alloc/Calloc.
		// NativeInitDeviceInfo usa ese mismo allocator para el buffer de
		// pixeles interno (via getDeviceInfo() -> Gcx_MM_Calloc) -- si se
		// llama primero, el pool todavia no existe, Gcx_MM_Alloc devuelve
		// NULL, y el memset(NULL,0,n) subsiguiente hace Data Abort (visto
		// en consola real, resuelto con vita-parse-core + objdump -d contra
		// libgameDSO.so: LR cae en Gcx_MM_Calloc+0x13, justo despues del
		// blx a memset@plt).
		game_log("Llamando JNI_OnLoad...\n");
		if (Game_JNI_OnLoad) Game_JNI_OnLoad(&jvm, NULL);
		game_log("Llamando NativeInitWithBufferSize(%d,%d)...\n", GAME_W, GAME_H);
		if (NativeInitWithBufferSize) NativeInitWithBufferSize(jniEnv, NULL, GAME_W, GAME_H);
		game_log("Llamando NativeInitDeviceInfo(%d,%d)...\n", GAME_W, GAME_H);
		if (NativeInitDeviceInfo) NativeInitDeviceInfo(jniEnv, NULL, GAME_W, GAME_H);
		// El tamano de PANTALLA, no el del buffer -- igual que Android, donde
		// NativeResize recibe el tamano de la GLSurfaceView real y el motor
		// arma solo el quad de escalado (ver nota en GAME_W/GAME_H).
		game_log("Llamando NativeResize(%d,%d)...\n", SCREEN_W, SCREEN_H);
		if (NativeResize) NativeResize(jniEnv, NULL, SCREEN_W, SCREEN_H);
		game_log("Llamando NativeResumeClet...\n");
		if (NativeResumeClet) NativeResumeClet(jniEnv, NULL);

		game_log("Iniciando Bucle Principal...\n");

#ifdef ZENONIA_GPU_QUADS
		// Variante 2: preparar el offload de sprites a GPU (scratch + cache).
		// Las coordenadas de los quads van en el espacio logico de pantalla del
		// motor (= SCREEN_W/H, que ya cascadea con downsample si se combinaran).
		gpu_quads_init(SCREEN_W, SCREEN_H);
#endif

		sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

		SceCtrlData pad;
		SceTouchData touch;
		int last_touch = 0;
		int last_touch_suppressed = 0;
		int last_tx = 0, last_ty = 0;
		unsigned int old_buttons = 0;
		int frame = 0;
		unsigned int rgb565_upload_accum = 0;
		unsigned int drawnative_calls_accum = 0;
		unsigned int cgxpzxani_draw_calls_accum = 0;
		unsigned int cgxpzxzero_blt_calls_accum = 0;
		unsigned int putcompressimg_calls_accum = 0;
		unsigned int cgsgraphics_calls_accum = 0;
		unsigned int drawtoframebuffer_calls_accum = 0;
		unsigned int sprite_cache_hits_accum = 0;
		unsigned int sprite_cache_misses_accum = 0;
		unsigned int dirtyrect_rows_uploaded_accum = 0;
		unsigned int dirtyrect_rows_total_accum = 0;
		unsigned int interlaced_rows_uploaded_accum = 0;
		unsigned int interlaced_rows_total_accum = 0;
		unsigned int gpu_quads_calls_accum = 0;
		unsigned int gpu_quads_op0_calls_accum = 0;
		unsigned int gpu_quads_cache_hit_accum = 0;
		unsigned int gpu_quads_decoded_accum = 0;
#ifdef ZENONIA_INSTRUMENT_FRAME_PHASES
		// Fase 85 (port_progress.md): metodo NUEVO, no mapeado en
		// optimization_ideas.md -- ninguna ronda anterior midio DONDE se va
		// el tiempo DENTRO del frame mas alla del hot path de sprites. El
		// techo de --skip-blit-test (Fase 47, ~24 FPS/~42ms de frame contra
		// ~10 FPS/~100ms normal) implica que sacar TODO el blit de sprites
		// deja ~42ms/frame sin explicar -- podria ser logica de juego/
		// animacion DENTRO de NativeRender (que fusiona update+draw, motor
		// tipico de la epoca) o costo FUERA de NativeRender (glClear, splash/
		// androidui_draw, vglSwapBuffers+vblank). Solo 2 timestamps mas por
		// frame (sceKernelGetProcessTimeWide, ya usado para el fps de
		// arriba) -- costo insignificante, NO es logging por-llamada (ver
		// feedback_no_perf_impact_from_logging en memoria).
		SceUInt64 frame_phase_cletevent_accum_us = 0;
		SceUInt64 frame_phase_nativerender_accum_us = 0;
		SceUInt64 frame_phase_rest_accum_us = 0;
#endif

		while (1) {
			sceCtrlPeekBufferPositive(0, &pad, 1);
			sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

			if ((frame++ % 120) == 0) {
				// FPS real medido entre checkpoints (antes este log no tenia
				// ningun numero objetivo -- todos los "5 FPS"/"18 FPS" del
				// historial de optimizacion en port_progress.md eran
				// impresion subjetiva del usuario, no una medicion). Un solo
				// sceKernelGetProcessTimeWide() cada 120 frames, costo
				// insignificante.
				static SceUInt64 last_fps_tick = 0;
				SceUInt64 now = sceKernelGetProcessTimeWide();
				float fps = 0.0f;
				if (last_fps_tick != 0 && now > last_fps_tick) {
					fps = 120.0f / ((now - last_fps_tick) / 1000000.0f);
				}
				last_fps_tick = now;
				// Fase 27: promedio de subidas del framebuffer RGB565 por
				// frame en los ultimos 120 frames -- confirma o descarta si
				// el motor sube el mismo framebuffer completo mas de una vez
				// por NativeRender() (ver nota junto a
				// g_rgb565_uploads_this_frame en dynlib.c).
				float avg_uploads = rgb565_upload_accum / 120.0f;
				rgb565_upload_accum = 0;
				// Fase 29: promedio de llamadas a DrawNative() por frame --
				// solo tiene sentido en el build de diagnostico
				// INSTRUMENT_DRAWNATIVE_CALLS (ver dynlib.c); en el build
				// normal el hook no se instala y esto queda en 0.0 siempre.
				float avg_drawnative = drawnative_calls_accum / 120.0f;
				drawnative_calls_accum = 0;
				// Fase 31: promedio por frame de los 3 candidatos
				// instrumentados a la vez (INSTRUMENT_HOTPATH_CALLS, ver
				// dynlib.c) -- 0.0 siempre en el build normal.
				float avg_ani_draw = cgxpzxani_draw_calls_accum / 120.0f;
				float avg_zero_blt = cgxpzxzero_blt_calls_accum / 120.0f;
				float avg_putcompress = putcompressimg_calls_accum / 120.0f;
				cgxpzxani_draw_calls_accum = 0;
				cgxpzxzero_blt_calls_accum = 0;
				putcompressimg_calls_accum = 0;
				// Fase 86: promedio por frame de llamadas a
				// CGxFACharCache::DrawToFrameBuffer (texto, INSTRUMENT_TEXT_CALLS,
				// ver dynlib.c) -- 0.0 siempre en el build normal.
				float avg_drawtoframebuffer = drawtoframebuffer_calls_accum / 120.0f;
				drawtoframebuffer_calls_accum = 0;
				// Fase 92: hit/miss de la cache de sprites COPY (SPRITE_DECODE_CACHE,
				// ver sprite_decode.c) -- 0.0 en ambas si el flag esta OFF. El dato
				// que decide si esta cache vale la pena en combate real.
				float avg_sprite_cache_hits = sprite_cache_hits_accum / 120.0f;
				float avg_sprite_cache_misses = sprite_cache_misses_accum / 120.0f;
				sprite_cache_hits_accum = 0;
				sprite_cache_misses_accum = 0;
				// Fase 36: promedio por frame del contador agregado sobre las
				// ~13 primitivas de CGsGraphics:: instrumentadas
				// (INSTRUMENT_CGSGRAPHICS_CALLS, ver dynlib.c) -- 0.0 siempre
				// en el build normal.
				float avg_cgsgraphics = cgsgraphics_calls_accum / 120.0f;
				cgsgraphics_calls_accum = 0;
				// Desglose por primitiva (dynlib.c) -- 0.0 en todas si
				// INSTRUMENT_CGSGRAPHICS_CALLS esta OFF, mismo criterio que
				// avg_cgsgraphics de arriba. Linea separada para no romper
				// el parseo de la linea "frame N alive" existente.
				zenonia_log_cgsgraphics_breakdown(120);
				// Fase 36: porcentaje de filas del framebuffer RGB565
				// realmente subidas vs. las que se hubieran subido sin el
				// diffing de DIRTY_RECT_UPLOAD (ver dynlib.c) -- 0.0/sin
				// filas totales siempre en el build normal.
				float avg_dirtyrect_pct = dirtyrect_rows_total_accum > 0
					? (100.0f * dirtyrect_rows_uploaded_accum / dirtyrect_rows_total_accum) : 0.0f;
				dirtyrect_rows_uploaded_accum = 0;
				dirtyrect_rows_total_accum = 0;
				// Fase 41: porcentaje de filas del framebuffer RGB565
				// realmente subidas via INTERLACED_UPLOAD (ver dynlib.c) --
				// deberia rondar 50% por diseno (alterna pares/impares
				// cada frame), a diferencia de avg_dirtyrect_pct que
				// depende de cuanto cambio realmente. 0.0/sin filas
				// totales siempre en el build normal.
				float avg_interlaced_pct = interlaced_rows_total_accum > 0
					? (100.0f * interlaced_rows_uploaded_accum / interlaced_rows_total_accum) : 0.0f;
				interlaced_rows_uploaded_accum = 0;
				interlaced_rows_total_accum = 0;
				// Fase 48: promedio por frame de llamadas a PutCompressImg vistas
				// por el hook de GPU_QUADS (siempre activo en ese build, a
				// diferencia de avg_putcompressimg_per_frame que depende de
				// INSTRUMENT_HOTPATH_CALLS) -- cuantas son op==0 (candidatas a
				// offload), cuantas de esas pegaron en cache vs. se decodificaron
				// de nuevo. 0.0 siempre en builds sin GPU_QUADS.
				float avg_gpu_quads_calls = gpu_quads_calls_accum / 120.0f;
				float avg_gpu_quads_op0 = gpu_quads_op0_calls_accum / 120.0f;
				float avg_gpu_quads_hit = gpu_quads_cache_hit_accum / 120.0f;
				float avg_gpu_quads_decoded = gpu_quads_decoded_accum / 120.0f;
				gpu_quads_calls_accum = 0;
				gpu_quads_op0_calls_accum = 0;
				gpu_quads_cache_hit_accum = 0;
				gpu_quads_decoded_accum = 0;
#ifdef ZENONIA_GPU_QUADS
				// Fase 77 (2026-07-24): total acumulado de texturas creadas
				// desde el boot, nunca reseteado -- para correlacionar con el
				// crash determinista de las Fases 72-76 (mismo .psp2dmp 5
				// veces, sobrevivio a batching y a re-bind de textura) sin
				// tener que esperar a un evento "frame pesado"/"cache
				// invalidada" -- siempre hay una lectura reciente cerca de
				// cualquier crash.
				unsigned long total_tex = gpu_quads_get_total_textures_created();
#else
				unsigned long total_tex = 0;
#endif
				game_log("frame %d alive, fps=%.1f avg_rgb565_uploads_per_frame=%.1f avg_drawnative_calls_per_frame=%.1f avg_cgxpzxani_draw_per_frame=%.1f avg_cgxpzxzero_blt_per_frame=%.1f avg_putcompressimg_per_frame=%.1f avg_drawtoframebuffer_per_frame=%.1f avg_cgsgraphics_per_frame=%.1f avg_dirtyrect_pct_uploaded=%.1f avg_interlaced_pct_uploaded=%.1f avg_gpu_quads_calls_per_frame=%.1f avg_gpu_quads_op0_per_frame=%.1f avg_gpu_quads_hit_per_frame=%.1f avg_gpu_quads_decoded_per_frame=%.1f avg_sprite_cache_hits_per_frame=%.1f avg_sprite_cache_misses_per_frame=%.1f total_texturas_creadas=%lu touch.reportNum=%d pad.buttons=0x%08x ui_status=%d\n",
					frame, fps, avg_uploads, avg_drawnative, avg_ani_draw, avg_zero_blt, avg_putcompress, avg_drawtoframebuffer, avg_cgsgraphics, avg_dirtyrect_pct, avg_interlaced_pct, avg_gpu_quads_calls, avg_gpu_quads_op0, avg_gpu_quads_hit, avg_gpu_quads_decoded, avg_sprite_cache_hits, avg_sprite_cache_misses, total_tex, touch.reportNum, (unsigned int) pad.buttons, g_ui_status);
#ifdef ZENONIA_INSTRUMENT_FRAME_PHASES
				// Fase 85: desglose de DONDE se va el tiempo dentro del frame --
				// ver comentario junto a los accum de arriba. 0.0 en todas si
				// INSTRUMENT_FRAME_PHASES esta OFF (nunca se acumula nada).
				game_log("[FramePhases] avg ms/frame (ultimos 120): handleCletEvent=%.2f NativeRender=%.2f resto(glClear+splash+androidui+swap+vblank)=%.2f\n",
					frame_phase_cletevent_accum_us / 120.0f / 1000.0f,
					frame_phase_nativerender_accum_us / 120.0f / 1000.0f,
					frame_phase_rest_accum_us / 120.0f / 1000.0f);
				frame_phase_cletevent_accum_us = 0;
				frame_phase_nativerender_accum_us = 0;
				frame_phase_rest_accum_us = 0;
#endif
				// Fase 71 (2026-07-23): game_log() no hace fflush por linea (ver
				// su propio comentario -- eso costaba 10 FPS reales). Efecto
				// colateral confirmado con 2 logs de usuario reales seguidos:
				// si se extrae el archivo por FTP con el juego TODAVIA corriendo
				// (sin cerrarlo, que es el unico lugar que hace fclose), el
				// contenido mas reciente queda atrapado en el buffer de stdio y
				// nunca llega al archivo -- ambos logs cortaban a mitad de linea,
				// exactamente antes del evento que se pedia diagnosticar. Este
				// bloque ya corre una vez cada 120 frames (~cada 6-10s a este
				// FPS) -- un fflush() aca cuesta 1 syscall cada varios segundos,
				// nada que ver con el cuello de botella de flushear cada linea.
				if (log_file) fflush(log_file);
			}

			// Salida de emergencia: START+SELECT juntos
			if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT)) break;

			// UI_STATUS_EXIT (104): en Android, ZenoniaUIControllerView.setUIState()
			// llama NexusGLActivity.myActivity.finishApp() para este estado --
			// una llamada puramente de Android que este loader no tiene forma
			// de reproducir. Sin este chequeo, el loop seguia corriendo para
			// siempre sin dibujar nada util (pantalla negra permanente) --
			// confirmado como la causa mas probable del "pantalla negra" al
			// navegar Triangulo(SKIP)+Circulo(BACK) en el menu: BACK ahi
			// dispara el flujo de salida del motor.
			if (g_ui_status == UI_STATUS_EXIT) break;

			unsigned int pressed = pad.buttons & ~old_buttons;
			unsigned int released = old_buttons & ~pad.buttons;

			// ABOUT/HELP (ui_status 5/4): en el APK original el scroll de
			// aboutWebView/helpWebView era gesto tactil -- sin capa Android
			// (ver androidui.c) no hay forma de reproducir eso, asi que se
			// reusa D-pad arriba/abajo para scrollear en vez de reenviarlo al
			// motor (que de todos modos no hace nada con UP/DOWN en estos 2
			// estados). Mantenido presionado = scroll continuo.
			if (g_ui_status == 4 || g_ui_status == 5) {
				if (pad.buttons & SCE_CTRL_UP)   androidui_scroll_info_text(g_ui_status, -8.0f, SCREEN_W, SCREEN_H);
				if (pad.buttons & SCE_CTRL_DOWN) androidui_scroll_info_text(g_ui_status,  8.0f, SCREEN_W, SCREEN_H);
				pressed  &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN);
				released &= ~(SCE_CTRL_UP | SCE_CTRL_DOWN);
			}

			for (int i = 0; i < BTN_MAP_COUNT; i++) {
				if (pressed & btn_map[i].btn)
					queue_input_event(MH_KEY_PRESSEVENT, btn_map[i].hal, 0, 0);
				if (released & btn_map[i].btn)
					queue_input_event(MH_KEY_RELEASEEVENT, btn_map[i].hal, 0, 0);
			}
			old_buttons = pad.buttons;

			// Touch: panel 1920x1088 -> espacio de JUEGO (GAME_W x GAME_H =
			// 400x240). Confirmado en UIFullTouch.java (la ruta de touch
			// in-game real de Zenonia 3): convertScreenX/Y multiplican por
			// gameScreenWidth/Height ANTES de setTouchEvent(23/25/24, x, y,
			// pointerId) -- el motor recibe coordenadas de juego, no de
			// pantalla. p3 es el pointerId (un solo dedo: 0). El evento 25
			// (MOVE) se manda cuando el dedo se arrastra -- Android lo manda
			// en cada ACTION_MOVE; aca, cuando cambia la celda de pixel.
			if (touch.reportNum > 0) {
				int x = touch.report[0].x * GAME_W / 1920;
				int y = touch.report[0].y * GAME_H / 1088;

				if (!last_touch) {
					// En el APK original, el logo/titulo/menu (ui_status
					// 1/2) son ImageView/ImageButton de Android que
					// CONSUMEN el toque antes de que llegue al
					// GLSurfaceView -- el motor nunca recibe un evento de
					// puntero crudo ahi, solo la tecla sintetica que el
					// listener de Android manda (ver androidui.c y
					// Natives.java: showTitleComponent()/
					// showMenuItemComponent()). Replicar eso: en vez de
					// reenviar el toque como MH_POINTER_*, interceptarlo
					// aca y mandar el evento equivalente -- o nada, si cae
					// afuera de cualquier boton (el fondo del menu no tiene
					// listener propio en el original).
					int sx = touch.report[0].x * SCREEN_W / 1920;
					int sy = touch.report[0].y * SCREEN_H / 1088;

					if (g_ui_status == 1) {
						// Natives.showTitleComponent(): gb_titleImg (fondo,
						// pantalla completa) manda BACK press+release ante
						// CUALQUIER toque.
						queue_input_event(MH_KEY_PRESSEVENT, HAL_KEY_BACK, 0, 0);
						queue_input_event(MH_KEY_RELEASEEVENT, HAL_KEY_BACK, 0, 0);
						last_touch_suppressed = 1;
					} else if (g_ui_status == 2) {
						androidui_menu_hit hit = androidui_menu_hit_test((float) sx, (float) sy, SCREEN_W, SCREEN_H);
						switch (hit) {
							case ANDROIDUI_MENU_HIT_COMMUNITY:
								queue_input_event(MH_KEY_PRESSEVENT, HAL_KEY_LEFT, 0, 0);
								queue_input_event(MH_KEY_RELEASEEVENT, HAL_KEY_LEFT, 0, 0);
								last_touch_suppressed = 1;
								break;
							case ANDROIDUI_MENU_HIT_OPTIONS:
								queue_input_event(MH_KEY_PRESSEVENT, HAL_KEY_UP, 0, 0);
								queue_input_event(MH_KEY_RELEASEEVENT, HAL_KEY_UP, 0, 0);
								last_touch_suppressed = 1;
								break;
							case ANDROIDUI_MENU_HIT_NEWGAME:
								queue_input_event(MH_KEY_PRESSEVENT, HAL_KEY_OK, 0, 0);
								queue_input_event(MH_KEY_RELEASEEVENT, HAL_KEY_OK, 0, 0);
								last_touch_suppressed = 1;
								break;
							case ANDROIDUI_MENU_HIT_CONTINUE:
								queue_input_event(NEXUS_HAL_REPLY_YESNO, NEXUS_HAL_FIRST_MOVE_REPLY_PAGE, 0, 0);
								last_touch_suppressed = 1;
								break;
							case ANDROIDUI_MENU_HIT_HELP:
								queue_input_event(MH_KEY_PRESSEVENT, HAL_KEY_DOWN, 0, 0);
								queue_input_event(MH_KEY_RELEASEEVENT, HAL_KEY_DOWN, 0, 0);
								last_touch_suppressed = 1;
								break;
							case ANDROIDUI_MENU_HIT_ABOUT:
								queue_input_event(MH_KEY_PRESSEVENT, HAL_KEY_RIGHT, 0, 0);
								queue_input_event(MH_KEY_RELEASEEVENT, HAL_KEY_RIGHT, 0, 0);
								last_touch_suppressed = 1;
								break;
							default:
								last_touch_suppressed = 0;
								break;
						}
					} else if (g_ui_status == 5 || g_ui_status == 4) {
						// Natives.showAboutComponent()/showHelpComponent(): el
						// mismo boton "uiback" (arriba a la derecha) manda BACK
						// press+release en las 2 pantallas.
						androidui_backbtn_hit hit = androidui_backbtn_hit_test((float) sx, (float) sy, SCREEN_W, SCREEN_H);
						if (hit == ANDROIDUI_BACKBTN_HIT_BACK) {
							queue_input_event(MH_KEY_PRESSEVENT, HAL_KEY_BACK, 0, 0);
							queue_input_event(MH_KEY_RELEASEEVENT, HAL_KEY_BACK, 0, 0);
							last_touch_suppressed = 1;
						} else {
							last_touch_suppressed = 0;
						}
					} else if (g_ui_status == 5000) {
						// Natives.showReplyMoveComponent(): popup de "valorar
						// la app" mostrado al tocar Continuar -- botones
						// "escribir resena"/"mas tarde", cada uno con su
						// propio OnClickListener (no touch crudo al motor).
						androidui_reply_hit hit = androidui_reply_hit_test((float) sx, (float) sy, SCREEN_W, SCREEN_H);
						switch (hit) {
							case ANDROIDUI_REPLY_HIT_WRITE:
								queue_input_event(NEXUS_HAL_REPLY_YESNO, NEXUS_HAL_YES_MOVE_REPLY_PAGE, 0, 0);
								last_touch_suppressed = 1;
								break;
							case ANDROIDUI_REPLY_HIT_LATER:
								queue_input_event(NEXUS_HAL_REPLY_YESNO, NEXUS_HAL_NO_MOVE_REPLY_PAGE, 0, 0);
								last_touch_suppressed = 1;
								break;
							default:
								last_touch_suppressed = 0;
								break;
						}
					} else {
						last_touch_suppressed = 0;
					}

					if (!last_touch_suppressed) {
						queue_input_event(MH_POINTER_PRESSEVENT, x, y, 0);
					}
					last_touch = 1;
				} else if (!last_touch_suppressed && (x != last_tx || y != last_ty)) {
					queue_input_event(MH_POINTER_MOVEEVENT, x, y, 0);
				}
				last_tx = x; last_ty = y;
			} else if (last_touch) {
				if (!last_touch_suppressed) {
					queue_input_event(MH_POINTER_RELEASEEVENT, last_tx, last_ty, 0);
				}
				last_touch = 0;
				last_touch_suppressed = 0;
			}

#ifdef ZENONIA_INSTRUMENT_FRAME_PHASES
			SceUInt64 fp_frame_start = sceKernelGetProcessTimeWide();
#endif
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);

#ifdef ZENONIA_INSTRUMENT_FRAME_PHASES
			SceUInt64 fp_before_clet = sceKernelGetProcessTimeWide();
#endif
			if (handleCletEvent && eq_head != eq_tail) {
				input_event *ev = &event_queue[eq_head];
				eq_head = (eq_head + 1) % 16;
				handleCletEvent(jniEnv, NULL, ev->type, ev->p1, ev->p2, ev->p3);
			}
#ifdef ZENONIA_INSTRUMENT_FRAME_PHASES
			SceUInt64 fp_after_clet = sceKernelGetProcessTimeWide();
#endif

#ifdef ZENONIA_GPU_QUADS
			// Variante 2 (Fase 50): los quads se dibujan en glDrawArrays_wrapper
			// (dynlib.c), justo despues del draw del FB base del motor -- aca no
			// hay nada que hacer.
#endif
			if (NativeRender) NativeRender(jniEnv, NULL);
#ifdef ZENONIA_INSTRUMENT_FRAME_PHASES
			SceUInt64 fp_after_render = sceKernelGetProcessTimeWide();
			frame_phase_cletevent_accum_us += (fp_after_clet - fp_before_clet);
			frame_phase_nativerender_accum_us += (fp_after_render - fp_after_clet);
#endif
			rgb565_upload_accum += g_rgb565_uploads_this_frame;
			g_rgb565_uploads_this_frame = 0;
			drawnative_calls_accum += g_drawnative_calls_this_frame;
			g_drawnative_calls_this_frame = 0;
			cgxpzxani_draw_calls_accum += g_cgxpzxani_draw_calls_this_frame;
			g_cgxpzxani_draw_calls_this_frame = 0;
			cgxpzxzero_blt_calls_accum += g_cgxpzxzero_blt_calls_this_frame;
			g_cgxpzxzero_blt_calls_this_frame = 0;
			putcompressimg_calls_accum += g_putcompressimg_calls_this_frame;
			g_putcompressimg_calls_this_frame = 0;
			drawtoframebuffer_calls_accum += g_drawtoframebuffer_calls_this_frame;
			g_drawtoframebuffer_calls_this_frame = 0;
			sprite_cache_hits_accum += g_sprite_cache_hits_this_frame;
			g_sprite_cache_hits_this_frame = 0;
			sprite_cache_misses_accum += g_sprite_cache_misses_this_frame;
			g_sprite_cache_misses_this_frame = 0;
			cgsgraphics_calls_accum += g_cgsgraphics_calls_this_frame;
			g_cgsgraphics_calls_this_frame = 0;
			dirtyrect_rows_uploaded_accum += g_dirtyrect_rows_uploaded_this_frame;
			dirtyrect_rows_total_accum += g_dirtyrect_rows_total_this_frame;
			g_dirtyrect_rows_uploaded_this_frame = 0;
			g_dirtyrect_rows_total_this_frame = 0;
			interlaced_rows_uploaded_accum += g_interlaced_rows_uploaded_this_frame;
			interlaced_rows_total_accum += g_interlaced_rows_total_this_frame;
			g_interlaced_rows_uploaded_this_frame = 0;
			g_interlaced_rows_total_this_frame = 0;

			gpu_quads_calls_accum += g_gpu_quads_calls_this_frame;
			g_gpu_quads_calls_this_frame = 0;
			gpu_quads_op0_calls_accum += g_gpu_quads_op0_calls_this_frame;
			g_gpu_quads_op0_calls_this_frame = 0;
			gpu_quads_cache_hit_accum += g_gpu_quads_cache_hit_this_frame;
			g_gpu_quads_cache_hit_this_frame = 0;
			gpu_quads_decoded_accum += g_gpu_quads_decoded_this_frame;
			g_gpu_quads_decoded_this_frame = 0;

			// Mientras el motor este en logo/titulo (estados de UI de Java,
			// invisibles en el loader nativo -- pendiente confirmar los
			// numeros de estado reales de Zenonia 3 en Fase 5), tapar con
			// el splash.
			if (g_ui_status >= 0 && g_ui_status <= 1) splash_draw();

			// Logo/titulo/menu (ui_status -1/1/2): el .so nativo solo dibuja
			// un fondo generico (CMvTitleState::DrawZeroGrade/DrawTeamLogo) --
			// el arte real (logo, fondo de titulo, fondo+botones de menu) lo
			// dibujaba Android como ImageView/ImageButton superpuestos al
			// GLSurfaceView (ver androidui.c). Sin capa Java, hay que
			// replicarlo aca.
			androidui_draw(g_ui_status, SCREEN_W, SCREEN_H);

			vglSwapBuffers(GL_FALSE);
#ifndef ZENONIA_DISABLE_VSYNC
            // Forzar limite a 30 FPS (esperar un vblank extra, ya que vglSwapBuffers espera el primero)
            sceDisplayWaitVblankStart();
#endif
#ifdef ZENONIA_INSTRUMENT_FRAME_PHASES
			// "rest" = todo lo que NO es handleCletEvent ni NativeRender:
			// sceCtrlPeekBufferPositive/sceTouchPeek del tope del loop, el
			// checkpoint de cada 120 frames, glClear, splash_draw/androidui_draw,
			// vglSwapBuffers + la espera de vblank. Por resta en vez de mas
			// timestamps -- ya cubre TODO lo que queda del frame.
			SceUInt64 fp_frame_end = sceKernelGetProcessTimeWide();
			SceUInt64 fp_total = fp_frame_end - fp_frame_start;
			SceUInt64 fp_clet = fp_after_clet - fp_before_clet;
			SceUInt64 fp_render = fp_after_render - fp_after_clet;
			frame_phase_rest_accum_us += (fp_total > fp_clet + fp_render) ? (fp_total - fp_clet - fp_render) : 0;
#endif
		}
	}

    if (log_file) {
        fclose(log_file);
    }
	sceKernelExitProcess(0);
	return 0;
}
