// Fase 132 (port_progress.md): sprites RLE grandes repartidos por filas entre los 3 nucleos.
//
// El draw-op COPY sin recorte del motor (DrawOP_COPY_Compress_16, out_ghidra.c:227093) es
// un stream de tokens por fila: 0xffff fin, 0xfffe "dest += stride" (sobre la posicion
// ACTUAL, no el inicio de fila), bit 15 = N pixeles opacos (N bytes de indice de paleta),
// si no = N pixeles transparentes. Las filas no se pisan entre si, asi que partir un sprite
// en franjas de filas y decodificar cada una en un nucleo distinto da EXACTAMENTE los mismos
// pixeles, y como el hilo principal espera a las franjas antes de volver, el orden respecto
// al resto del dibujo (texto, fills, efectos) no cambia. Cada franja recorre los tokens de
// las filas anteriores sin escribir, acumulando el desplazamiento real de dest.
//
// Solo sprites con h/area grandes (sincronizar cuesta ~us); el resto, y los headers "Ex"
// [-7,-3] (otro formato), van a la rutina original del motor sin tocar.

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdint.h>
#include "so_util.h"

extern void game_log(const char *fmt, ...);

#ifdef ZENONIA_PARALLEL_SPRITES

#define MIN_ROWS   24
#define MIN_AREA   (32 * 32)
#define SLEEP_IDLE_US 8000  // sin trabajo por 8ms (logica, frames saltados): dormir, no quemar el nucleo
#define NW 2                // nucleo 2 + nucleo 1 (este por debajo del audio)

typedef void (*copy_fn)(uint16_t *, uint8_t *, uint16_t *, int);
static copy_fn g_orig_copy;

static const void *g_pend_src;
static int g_pend_w, g_pend_h;
volatile unsigned int g_par_split_this_frame, g_par_alone_this_frame;

void sprite_parallel_set_pending(const void *src, int w, int h) {
    g_pend_src = src; g_pend_w = w; g_pend_h = h;
}

// Filas [r0, r1) del sprite. r1 = fila tope exclusiva (se corta en su 0xfffe).
static void decode_rows(uint16_t *dest, const uint8_t *p, const uint16_t *pal, int stride, int r0, int r1) {
    int row = 0;
    while (row < r0) {
        uint16_t t = (uint16_t) (p[0] | (p[1] << 8));
        if (t == 0xffff) return;
        p += 2;
        if (t == 0xfffe) { dest += stride; row++; }
        else if (t & 0x8000) { t &= 0x7fff; dest += t; p += t; }
        else dest += t;
    }
    for (;;) {
        uint16_t t = (uint16_t) (p[0] | (p[1] << 8));
        if (t == 0xffff) return;
        p += 2;
        if (t == 0xfffe) {
            if (++row >= r1) return;
            dest += stride;
        } else if (t & 0x8000) {
            t &= 0x7fff;
            while (t--) *dest++ = pal[*p++];
        } else {
            dest += t;
        }
    }
}

enum { W_IDLE, W_CLAIMED, W_JOB, W_DONE, W_SLEEP };
typedef struct {
    volatile int state;
    uint16_t *dest; const uint8_t *src; const uint16_t *pal; int stride, r0, r1;
} worker_t;
static worker_t g_w[NW];

static int worker_main(SceSize args, void *argp) {
    worker_t *w = *(worker_t **) argp;
    SceUInt64 idle_since = sceKernelGetProcessTimeWide();
    unsigned int spins = 0;
    for (;;) {
        int s = __atomic_load_n(&w->state, __ATOMIC_ACQUIRE);
        if (s == W_JOB) {
            decode_rows(w->dest, w->src, w->pal, w->stride, w->r0, w->r1);
            __atomic_store_n(&w->state, W_DONE, __ATOMIC_RELEASE);
            spins = 0;
            idle_since = 0;
            continue;
        }
        if (s == W_IDLE && (++spins & 1023) == 0) {
            SceUInt64 now = sceKernelGetProcessTimeWide();
            if (!idle_since) idle_since = now;
            else if (now - idle_since > SLEEP_IDLE_US) {
                int exp = W_IDLE; // si el principal ya lo reclamo, el CAS falla y sigue despierto
                if (__atomic_compare_exchange_n(&w->state, &exp, W_SLEEP, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    sceKernelDelayThread(1000);
                    __atomic_store_n(&w->state, W_IDLE, __ATOMIC_RELEASE);
                }
                idle_since = 0;
            }
        }
        __asm__ volatile("yield");
    }
    return 0;
}

static void DrawOP_COPY_Parallel(uint16_t *dest, uint8_t *src, uint16_t *pal, int stride) {
    int16_t header = (int16_t) (src[0] | (src[1] << 8));
    int w = 0, h = 0;
    if (g_pend_src == src) { w = g_pend_w; h = g_pend_h; }
    g_pend_src = 0;
    if (h < MIN_ROWS || w * h < MIN_AREA || (header >= -7 && header <= -3)) {
        g_orig_copy(dest, src, pal, stride);
        return;
    }

    worker_t *got[NW];
    int n = 0;
    for (int i = 0; i < NW; i++) {
        int exp = W_IDLE;
        if (__atomic_compare_exchange_n(&g_w[i].state, &exp, W_CLAIMED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            got[n++] = &g_w[i];
    }
    if (!n) { // los dos dormidos: solo, con la rutina original
        g_par_alone_this_frame++;
        g_orig_copy(dest, src, pal, stride);
        return;
    }
    g_par_split_this_frame++;
    int bands = n + 1;
    for (int i = 0; i < n; i++) {
        worker_t *wk = got[i];
        wk->dest = dest; wk->src = src; wk->pal = pal; wk->stride = stride;
        wk->r0 = h * (i + 1) / bands;
        wk->r1 = (i + 2 == bands) ? 0x7fffffff : h * (i + 2) / bands; // la ultima hasta 0xffff
        __atomic_store_n(&wk->state, W_JOB, __ATOMIC_RELEASE);
    }
    decode_rows(dest, src, pal, stride, 0, h / bands);
    for (int i = 0; i < n; i++) {
        while (__atomic_load_n(&got[i]->state, __ATOMIC_ACQUIRE) != W_DONE) __asm__ volatile("yield");
        __atomic_store_n(&got[i]->state, W_IDLE, __ATOMIC_RELEASE);
    }
}

static so_hook g_szb_hook;

static void SetZeroBlendFunc_par_hook(int op, void *func, void *funcClip) {
    if (op == 0) { // COPY
        g_orig_copy = (copy_fn) func;
        func = (void *) DrawOP_COPY_Parallel;
        game_log("[ParallelSprites] COPY -> paralelo (orig=%p)\n", g_orig_copy);
    }
    uintptr_t fn = g_szb_hook.thumb_addr ? g_szb_hook.thumb_addr : g_szb_hook.addr;
    so_unhook(&g_szb_hook);
    ((void (*)(int, void *, void *)) fn)(op, func, funcClip);
    g_szb_hook = hook_addr(fn, (uintptr_t) SetZeroBlendFunc_par_hook);
}

void zenonia_install_parallel_sprites(so_module *mod) {
    static const struct { int mask, prio; } cfg[NW] = {
        { SCE_KERNEL_CPU_MASK_USER_2, 0x10000100 },
        { SCE_KERNEL_CPU_MASK_USER_1, 0x10000110 }, // menos prioridad que el audio (0x10000100): no lo corta
    };
    for (int i = 0; i < NW; i++) {
        g_w[i].state = W_IDLE;
        SceUID tid = sceKernelCreateThread("zen4_spr", worker_main, cfg[i].prio, 0x4000, 0, 0, NULL);
        if (tid < 0) {
            g_w[i].state = W_DONE; // nunca IDLE: el principal no lo reclama
            game_log("[ParallelSprites] worker %d: create fallo 0x%08x\n", i, tid);
            continue;
        }
        sceKernelChangeThreadCpuAffinityMask(tid, cfg[i].mask);
        worker_t *wp = &g_w[i];
        sceKernelStartThread(tid, sizeof(wp), &wp);
    }
    uintptr_t addr = so_symbol(mod, "_Z16SetZeroBlendFunc10enumDrawOPPvS0_");
    if (!addr) { game_log("[ParallelSprites] SetZeroBlendFunc no encontrado -- sin efecto\n"); return; }
    g_szb_hook = hook_addr(addr, (uintptr_t) SetZeroBlendFunc_par_hook);
    so_flush_caches(mod);
    game_log("[ParallelSprites] %d workers (nucleos 2 y 1), hook en SetZeroBlendFunc (0x%08x)\n", NW, (unsigned int) addr);
}

#else
volatile unsigned int g_par_split_this_frame, g_par_alone_this_frame;
void sprite_parallel_set_pending(const void *src, int w, int h) { (void) src; (void) w; (void) h; }
void zenonia_install_parallel_sprites(so_module *mod) { (void) mod; }
#endif
