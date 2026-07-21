#include <stdint.h>
#include <stddef.h>
#include "so_util.h"

extern void game_log(const char *fmt, ...);

// Fase 44 (port_progress.md): triage de un crash real
// (Zenonia4-psp2core-1784490570-...) confirmo que esta rutina solo reproduce
// FIELMENTE el camino generico de DrawOP_COPY_Compress_16 real
// (out_ghidra.c:227093, sin distincion de bit 14, un byte de paleta por
// pixel) -- pero el puntero que el motor registra de verdad es
// DrawOP_COPY_Compress_16_Auto (out_ghidra.c:200261), que redirige CUALQUIER
// header en [-7,-3] (formula real: `-header-3 < 5`) a una tabla de 5
// rutinas "Ex" con un formato de tokens mas complejo: cuando el bit 14 del
// token tambien esta prendido (token >= 0xC000), es un "run repetido" (UN
// solo byte de paleta se repite N veces, mascara 0x3fff, el puntero de
// origen avanza solo 1 byte en total) en vez de un "run literal" (un byte
// nuevo por pixel, mascara 0x7fff) -- ver DrawOP_COPY_Compress_16_Ex
// (out_ghidra.c:198818) y DrawOP_COPY_Compress_16_Ex_Alpha (con un canal
// alpha aparte, out_ghidra.c:199071). Esta rutina no distinguia esos dos
// casos: interpretaba TODO run con el bit 15 prendido como literal
// (mascara 0x7fff siempre), asi que un run repetido real (p.ej. token
// 0xC005 = repetir 5 veces) se leia como un run literal de 0x4005 (16389)
// pixeles -- una lectura/escritura fuera de los limites del sprite real,
// corrompiendo memoria adyacente. El crash real ocurrio en
// CGxPZxEffectExFrame::__Draw (efectos/particulas, que probablemente usan
// bloques de color solido -- el caso ideal para el run repetido) varios
// frames DESPUES del sprite que realmente disparo la corrupcion, consistente
// con corrupcion de heap silenciosa que solo se manifiesta cuando algo mas
// lee la memoria pisada.
//
// Fix real: en vez de intentar reproducir las 3 variantes "Ex" (formato de
// bits mas alpha-plane, no confirmado con la misma certeza que el camino
// generico), delegar CUALQUIER header en el rango [-7,-3] al puntero
// ORIGINAL real (capturado en SetZeroBlendFunc_hook antes de reemplazarlo,
// ver mas abajo) -- la rutina nativa solo se usa para el camino generico ya
// verificado byte a byte contra el pseudo-C real.
static void *g_orig_copy_func = NULL;
typedef void (*copy_orig_fn)(uint16_t *, uint8_t *, uint16_t *, int);

// Rutina nativa optimizada de DrawOP_COPY_Compress_16 (SOLO camino generico)
// Compilada por GCC para ARMv7+NEON de Vita
void DrawOP_COPY_Compress_16_Vita(uint16_t *dest, uint8_t *src, uint16_t *pal, int stride) {
    int16_t header = (int16_t)(src[0] | (src[1] << 8));
    if (header >= -7 && header <= -3) {
        // Uno de los 5 headers que DrawOP_COPY_Compress_16_Auto redirige a
        // una rutina "Ex" -- no reproducido aca, delegar al original real.
        if (g_orig_copy_func) {
            ((copy_orig_fn) g_orig_copy_func)(dest, src, pal, stride);
        }
        return;
    }

    uint8_t b1, b2;
    uint8_t *p_src = src;
    uint16_t token;

    b1 = p_src[1];
    b2 = p_src[0];

    while (1) {
        token = (b1 << 8) | b2;
        if (token == 0xffff) {
            return;
        }
        
        p_src += 2;
        
        if (token == 0xfffe) {
            dest += stride;
        }
        else if (token & 0x8000) { // Opaque block
            token &= 0x7fff;
            while (token != 0) {
                *dest++ = pal[*p_src++];
                token--;
            }
        }
        else { // Transparent pixels (skip)
            dest += token;
        }
        
        b1 = p_src[1];
        b2 = p_src[0];
    }
}

// Rutina nativa optimizada de DrawOP_ENLARGE_Compress_16
void DrawOP_ENLARGE_Compress_16_Vita(uint16_t *dest, uint8_t *src, uint16_t *pal, int stride, int scale) {
    uint8_t b1, b2;
    uint8_t *p_src = src;
    uint16_t token;
    uint16_t *dest_row = dest;
    
    b1 = p_src[1];
    b2 = p_src[0];
    
    if ((int16_t)(p_src[0] | (p_src[1] << 8)) == -5) {
        p_src += 10;
        b1 = p_src[1];
        b2 = p_src[0];
    }
    
    uint16_t *current_dest = dest_row;
    
    while (1) {
        token = (b1 << 8) | b2;
        if (token == 0xffff) {
            return;
        }
        
        p_src += 2;
        
        if (token == 0xfffe) {
            dest_row += scale * stride;
            current_dest = dest_row;
        }
        else if (token & 0x8000) {
            token &= 0x7fff;
            while (token != 0) {
                if (scale > 0) {
                    uint16_t *block_dest = current_dest;
                    uint16_t color = pal[*p_src];
                    // Draw a scale x scale block
                    for (int y = 0; y < scale; y++) {
                        for (int x = 0; x < scale; x++) {
                            block_dest[x] = color;
                        }
                        block_dest += stride;
                    }
                }
                p_src++;
                current_dest += scale;
                token--;
            }
        }
        else {
            current_dest += token * scale;
        }
        
        b1 = p_src[1];
        b2 = p_src[0];
    }
}

static so_hook g_setzeroblendfunc_hook;

void SetZeroBlendFunc_hook(int enumDrawOP, void *func, void *funcClip) {
    if (enumDrawOP == 0) { // COPY
        g_orig_copy_func = func; // guardar el puntero real (DrawOP_COPY_Compress_16_Auto) ANTES de reemplazarlo -- Fase 44, ver comentario junto a DrawOP_COPY_Compress_16_Vita
        func = (void*)DrawOP_COPY_Compress_16_Vita;
        game_log("[SpriteDecodeOptimize] Reemplazado COPY (0) con rutina C Vita nativa (orig=%p)\n", g_orig_copy_func);
    }
    // Comentamos temporalmente ENLARGE para ver si esto arregla los errores graficos
    // ya que no le implementamos Alpha Blending nativo aun.
    /*
    else if (enumDrawOP == 0x10) { // ENLARGE
        func = (void*)DrawOP_ENLARGE_Compress_16_Vita;
        game_log("[SpriteDecodeOptimize] Reemplazado ENLARGE (0x10) con rutina C Vita nativa\n");
    }
    */
    
    so_unhook(&g_setzeroblendfunc_hook);
    
    ((void (*)(int, void*, void*))
        (g_setzeroblendfunc_hook.thumb_addr ? g_setzeroblendfunc_hook.thumb_addr : g_setzeroblendfunc_hook.addr))
        (enumDrawOP, func, funcClip);
        
    g_setzeroblendfunc_hook = hook_addr(
        g_setzeroblendfunc_hook.thumb_addr ? g_setzeroblendfunc_hook.thumb_addr : g_setzeroblendfunc_hook.addr,
        (uintptr_t) SetZeroBlendFunc_hook);
}

void zenonia_install_sprite_decode_optimize(so_module *mod) {
#ifdef ZENONIA_SPRITE_DECODE_OPTIMIZE
    uintptr_t addr = so_symbol(mod, "_Z16SetZeroBlendFunc10enumDrawOPPvS0_");
    if (addr) {
        g_setzeroblendfunc_hook = hook_addr(addr, (uintptr_t) SetZeroBlendFunc_hook);
        so_flush_caches(mod);
        game_log("[SpriteDecodeOptimize] hook instalado en SetZeroBlendFunc (0x%08x)\n", (unsigned int) addr);
    } else {
        game_log("[SpriteDecodeOptimize] error: no se encontro SetZeroBlendFunc\n");
    }
#endif
}
