#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "so_util.h"

extern void game_log(const char *fmt, ...);

// Fase 83 (port_progress.md): candidato 4 (threading acotado) reencuadrado
// tras confirmar que PutCompressImg SI recibe w/h explicitos
// (CGxPZxZero::Blt, out_ghidra.c:247337-247381 -- resuelve
// param_3/param_4 desde *(short*)(this+8)/+10, campos de CGcxBitmap
// inicializados a 0xffff sentinela, out_ghidra.c:243641) -- la rutina RLE
// de bajo nivel en si NO los tiene (Fase 43, sigue siendo cierto), pero su
// caller si, y ya lo vemos en PutCompressImg_hook (dynlib.c). Esto habilita
// una cache de sprites COPY ya decodificados en CPU, sin GPU/vitaGL/threads
// (el usuario eligio la version mas simple y segura: mismo hilo, sin hilo
// de fondo).
//
// Diseño: PutCompressImg_hook (dynlib.c) guarda (src,w,h) ANTES de delegar
// al PutCompressImg real -- la llamada es sincronica y anidada (confirmado
// en todos los .psp2dmp de este proyecto: Blt->PutCompressImg->g_funcZeroCopy
// en el mismo stack), asi que cuando DrawOP_COPY_Compress_16_Vita corre mas
// abajo, w/h siguen validos para ESE mismo src. Solo se usa para el camino
// GENERICO ya verificado (no headers [-7,-3], nunca clipping) -- mismo
// criterio conservador que el resto de este archivo.
//
// Clave de cache: puntero src + 4 bytes de contenido muestreados + w + h,
// mismo mecanismo (y misma razon: evitar que un puntero reciclado con
// contenido nuevo sirva una entrada vieja) que gpu_quads.c ya valido en
// produccion (Fase 55/69). Invalidacion completa en la misma señal de
// transicion de stage que gpu_quads_invalidate_cache() (ui_status==19,
// Zenonia_OnUIStatusChange en java.c) -- mismo motivo: un puntero de heap
// reciclado entre stages con los mismos 4 bytes iniciales por coincidencia.
//
// Seguridad del contenido cacheado: cada entrada guarda pixeles Y una
// mascara de "pixel realmente escrito" (byte por pixel) -- un run
// transparente (skip) en el sprite original deja el destino SIN TOCAR, asi
// que en un cache-hit solo se escriben los pixeles marcados, replicando el
// mismo comportamiento. El indice lineal usado para poblar la cache durante
// un miss se auto-invalida (`cache_valid=0`, la entrada NUNCA se registra)
// si en algun momento se saldria de los limites w*h -- si eso pasa, ese
// sprite simplemente nunca se cachea (se sigue decodificando cada frame,
// como hoy) en vez de arriesgar servir una entrada parcial/incorrecta.
// Fase 92 (port_progress.md): contadores de hit/miss, mismo patron que
// avg_gpu_quads_hit_per_frame -- la Fase 85/91 dejaron esta cache lista pero
// nunca medida en una sesion de combate real; sin esto, un log de esa sesion
// solo diria el FPS, no si la cache realmente esta funcionando o si el hit
// rate es demasiado bajo para importar. Siempre declarados (0.0 en el log si
// SPRITE_DECODE_CACHE esta OFF), incrementados solo bajo el ifdef de abajo --
// mismo criterio que el resto de los contadores de este proyecto.
volatile unsigned int g_sprite_cache_hits_this_frame = 0;
volatile unsigned int g_sprite_cache_misses_this_frame = 0;

#ifdef ZENONIA_SPRITE_DECODE_CACHE
#define SPRITE_CACHE_CAP 256
typedef struct {
    const void *src;
    unsigned long hash;
    int w, h;
    uint16_t *pixels;
    uint8_t *mask;
    int used;
} sc_entry;
static sc_entry g_sprite_cache[SPRITE_CACHE_CAP];
static int g_sprite_cache_evict_ring = 0;

static const void *g_pending_src = NULL;
static int g_pending_w = 0;
static int g_pending_h = 0;

void sprite_cache_set_pending_dims(const void *src, int w, int h) {
    g_pending_src = src;
    g_pending_w = w;
    g_pending_h = h;
}

static unsigned long sc_hash(const void *src, int w, int h) {
    unsigned long hv = (unsigned long) src * 2654435761u;
    hv ^= (unsigned long) w * 8191u;
    hv ^= (unsigned long) h * 97u;
    // Mismo criterio que gq_hash (gpu_quads.c, Fase 55): solo sampleamos
    // contenido real si el puntero parece una direccion de heap plausible
    // (evita leer offsets invalidos de punteros pequeños/sentinela).
    if (src && (unsigned long) src >= 0x40000000) {
        const unsigned char *d = (const unsigned char *) src;
        hv ^= d[0] * 13u;
        hv ^= d[1] * 37u;
        hv ^= d[2] * 71u;
        hv ^= d[3] * 113u;
    }
    return hv;
}

static sc_entry *sc_find(const void *src, int w, int h, unsigned long hash_val) {
    unsigned long start = hash_val % SPRITE_CACHE_CAP;
    for (int i = 0; i < SPRITE_CACHE_CAP; i++) {
        sc_entry *e = &g_sprite_cache[(start + i) % SPRITE_CACHE_CAP];
        if (!e->used) return NULL;
        if (e->src == src && e->w == w && e->h == h && e->hash == hash_val) return e;
    }
    return NULL;
}

static void sc_free_entry(sc_entry *e) {
    if (e->pixels) free(e->pixels);
    if (e->mask) free(e->mask);
    e->pixels = NULL;
    e->mask = NULL;
    e->used = 0;
}

static sc_entry *sc_alloc_slot(const void *src, int w, int h, unsigned long hash_val) {
    unsigned long start = hash_val % SPRITE_CACHE_CAP;
    for (int i = 0; i < SPRITE_CACHE_CAP; i++) {
        sc_entry *e = &g_sprite_cache[(start + i) % SPRITE_CACHE_CAP];
        if (!e->used) return e;
    }
    // cache llena: desaloja en anillo (misma politica simple que gpu_quads.c)
    sc_entry *e = &g_sprite_cache[g_sprite_cache_evict_ring];
    g_sprite_cache_evict_ring = (g_sprite_cache_evict_ring + 1) % SPRITE_CACHE_CAP;
    sc_free_entry(e);
    return e;
}

void sprite_cache_invalidate(void) {
    int freed = 0;
    for (int i = 0; i < SPRITE_CACHE_CAP; i++) {
        if (g_sprite_cache[i].used) {
            sc_free_entry(&g_sprite_cache[i]);
            freed++;
        }
    }
    g_sprite_cache_evict_ring = 0;
    game_log("[SpriteDecodeCache] cache invalidada en transicion de stage (%d entradas liberadas)\n", freed);
}

// Replay de una entrada de cache (hit): solo escribe pixeles marcados,
// deja el resto del destino intacto (equivalente a los runs transparentes
// del sprite original).
static void sc_replay(sc_entry *e, uint16_t *restrict dest, int stride) {
    for (int row = 0; row < e->h; row++) {
        uint16_t *drow = dest + (size_t) row * stride;
        const uint16_t *prow = e->pixels + (size_t) row * e->w;
        const uint8_t *mrow = e->mask + (size_t) row * e->w;
        for (int col = 0; col < e->w; col++) {
            if (mrow[col]) drow[col] = prow[col];
        }
    }
}

// Decodifica el camino generico (mismos tokens que
// DrawOP_COPY_Compress_16_Vita) escribiendo AL MISMO TIEMPO al destino real
// y a un buffer de cache privado w*h -- un unico pase, sin decodificar dos
// veces. Si la geometria derivada (indice lineal) se saldria de w*h en
// algun momento, se aborta el cacheo de ESTA entrada (cache_valid=0) sin
// afectar el dibujo real, que sigue su curso normal.
static void sc_decode_and_populate(uint16_t *restrict dest, const uint8_t *restrict src,
                                    const uint16_t *restrict pal, int stride,
                                    int w, int h, unsigned long hash_val) {
    int capacity = w * h;
    uint16_t *cache_px = (uint16_t *) malloc((size_t) capacity * sizeof(uint16_t));
    uint8_t *cache_mask = (uint8_t *) calloc((size_t) capacity, 1);
    int cache_valid = (cache_px != NULL && cache_mask != NULL);
    int cache_idx = 0;

    const uint8_t *p_src = src;
    uint16_t token;

    while (1) {
        token = (uint16_t) (p_src[1] << 8) | p_src[0];
        if (token == 0xffff) break;
        p_src += 2;

        if (token == 0xfffe) {
            dest += stride;
            if (cache_valid) {
                int row = cache_idx / w;
                cache_idx = (row + 1) * w;
                if (cache_idx > capacity) cache_valid = 0;
            }
            continue;
        }
        if (token & 0x8000) {
            uint16_t count = token & 0x7fff;
            while (count != 0) {
                uint16_t color = pal[*p_src++];
                *dest++ = color;
                if (cache_valid) {
                    if (cache_idx < capacity) {
                        cache_px[cache_idx] = color;
                        cache_mask[cache_idx] = 1;
                        cache_idx++;
                    } else {
                        cache_valid = 0;
                    }
                }
                count--;
            }
        } else {
            dest += token;
            if (cache_valid) {
                cache_idx += token;
                if (cache_idx > capacity) cache_valid = 0;
            }
        }
    }

    if (cache_valid) {
        // sc_alloc_slot ya garantiza un slot sin usar (libera la entrada
        // desalojada internamente si hizo falta) -- ver mas arriba.
        sc_entry *slot = sc_alloc_slot(src, w, h, hash_val);
        slot->src = src;
        slot->hash = hash_val;
        slot->w = w;
        slot->h = h;
        slot->pixels = cache_px;
        slot->mask = cache_mask;
        slot->used = 1;
    } else {
        if (cache_px) free(cache_px);
        if (cache_mask) free(cache_mask);
    }
}
#endif // ZENONIA_SPRITE_DECODE_CACHE

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

// Fase 83 (port_progress.md): variante nativa de header==-6
// (DrawOP_COPY_Compress_16_Ex_Alpha, out_ghidra.c:199071-199221) -- blend
// alpha por pixel contra un plano de alpha separado (offset de 32 bits
// little-endian embebido en el header extendido, src[2..5]), a diferencia
// del camino generico que es opaco puro. Formula de blend RGB565 replicada
// SIN clamping, igual que el pseudo-C real (no le agrega seguridad que el
// original no tiene).
//
// RIESGO RESIDUAL, sin confirmar en consola todavia: DrawOP_COPY_Compress_16_Ex
// (la variante SIN alpha, out_ghidra.c:198818) tambien chequea header==-6
// internamente -- Ghidra no deja ver la tabla de datos real
// (PTR_DrawOP_COPY_Compress_16_Ex_1_001a3c2c) para confirmar con certeza cual
// de las dos es el slot -6 real del dispatcher de COPY (enumDrawOP==0), o si
// Ex_Alpha en realidad pertenece a un enumDrawOP distinto que este hook nunca
// ve. Se implementa igual (a pedido explicito del usuario, como la variante
// "mas segura" de las 4 restantes en el rango [-7,-3]) pero con MENOS certeza
// que el camino generico de las Fases 43-45 -- probar especificamente escenas
// con dialogos/paneles translucidos y vigilar el mismo patron de la Fase 45
// (crash retrasado varios frames despues del sprite real que lo origino, no
// necesariamente inmediato).
#ifdef ZENONIA_SPRITE_DECODE_EX_ALPHA
static void DrawOP_COPY_Compress_16_Ex_Alpha_Vita(uint16_t *restrict dest, const uint8_t *restrict src,
                                                   const uint16_t *restrict pal, int stride) {
    uint32_t alpha_off = (uint32_t) src[2] | ((uint32_t) src[3] << 8) |
                          ((uint32_t) src[4] << 16) | ((uint32_t) src[5] << 24);
    const uint8_t *alpha_p = src + 2 + alpha_off;
    const uint8_t *p_src = src + 10;
    uint16_t token;

    while (1) {
        token = (uint16_t) (p_src[1] << 8) | p_src[0];
        if (token == 0xffff) {
            return;
        }
        p_src += 2;

        if (token == 0xfffe) {
            dest += stride;
            continue;
        }

        if (token & 0x8000) {
            int repeated = (token >= 0xc000);
            uint16_t count = repeated ? (uint16_t) (token & 0x3fff) : (uint16_t) (token & 0x7fff);
            int src_step = repeated ? 0 : 1;
            while (count != 0) {
                uint32_t src_color = pal[*p_src];
                uint32_t a = *alpha_p++;
                uint32_t inv_a = (~a) & 0xff;
                uint32_t dst_color = *dest;

                uint32_t r = ((inv_a * (dst_color >> 0xb) + a * (src_color >> 0xb)) >> 8) << 0xb;
                uint32_t g = ((inv_a * ((dst_color >> 5) & 0x3f) + a * ((src_color >> 5) & 0x3f)) >> 8) << 5;
                uint32_t b = (a * (src_color & 0x1f) + inv_a * (dst_color & 0x1f)) >> 8;

                *dest++ = (uint16_t) (r | g | b);
                p_src += src_step;
                count--;
            }
            if (repeated) p_src += 1;
        } else {
            dest += token;
        }
    }
}
#endif // ZENONIA_SPRITE_DECODE_EX_ALPHA

// Rutina nativa optimizada de DrawOP_COPY_Compress_16 (SOLO camino generico)
// Compilada por GCC para ARMv7+NEON de Vita.
//
// `restrict` en los 3 punteros: el motor original (out_ghidra.c:227093) nunca
// los hace alias entre si (dest/pal son buffers separados que el motor arma
// antes de llamar, src es de solo lectura) -- confirmado leyendo el pseudo-C,
// no asumido. Esto le da a GCC permiso real para no re-cargar `pal`/`dest`
// en cada iteracion del loop de copia, que es donde vive el costo real de
// este hot path (445-547 llamadas/frame confirmadas en consola, Fase 38).
__attribute__((hot))
void DrawOP_COPY_Compress_16_Vita(uint16_t *restrict dest, const uint8_t *restrict src,
                                   const uint16_t *restrict pal, int stride) {
    int16_t header = (int16_t)(src[0] | (src[1] << 8));
#ifdef ZENONIA_SPRITE_DECODE_EX_ALPHA
    if (header == -6) {
        // Fase 83: unica de las 4 variantes restantes de [-7,-3] reproducida
        // nativamente -- ver comentario junto a
        // DrawOP_COPY_Compress_16_Ex_Alpha_Vita (mas arriba) para el riesgo
        // residual sin confirmar en consola todavia. Gateada bajo su propio
        // flag (SPRITE_DECODE_EX_ALPHA) para no arriesgar el camino generico
        // ya confirmado si esto resulta mal -- ver CMakeLists.txt.
        DrawOP_COPY_Compress_16_Ex_Alpha_Vita(dest, src, pal, stride);
        return;
    }
#endif
    if (header >= -7 && header <= -3) {
        // Uno de los otros 4 headers que DrawOP_COPY_Compress_16_Auto
        // redirige a una rutina "Ex" -- no reproducido aca, delegar al
        // original real.
        if (g_orig_copy_func) {
            ((copy_orig_fn) g_orig_copy_func)(dest, (uint8_t *) src, (uint16_t *) pal, stride);
        }
        return;
    }

#ifdef ZENONIA_SPRITE_DECODE_CACHE
    // Fase 83: w/h solo estan disponibles si PutCompressImg_hook (dynlib.c)
    // los guardo para ESTE MISMO src, en la llamada sincronica anidada que
    // nos trajo hasta aca (ver comentario extenso mas arriba). Si no
    // coincide (llamada desde otro lado, o el stash no se seteo), cae al
    // camino de siempre sin cache -- cero riesgo.
    if (g_pending_src == src && g_pending_w > 0 && g_pending_h > 0) {
        int w = g_pending_w, h = g_pending_h;
        g_pending_src = NULL; // consumo unico, evita reuso stale
        g_pending_w = 0;
        g_pending_h = 0;
        unsigned long hash_val = sc_hash(src, w, h);
        sc_entry *hit = sc_find(src, w, h, hash_val);
        if (hit) {
            g_sprite_cache_hits_this_frame++;
            sc_replay(hit, dest, stride);
            return;
        }
        g_sprite_cache_misses_this_frame++;
        sc_decode_and_populate(dest, src, pal, stride, w, h, hash_val);
        return;
    }
#endif

    const uint8_t *p_src = src;
    uint16_t token;

    while (1) {
        token = (uint16_t) (p_src[1] << 8) | p_src[0];
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
    // ENLARGE se mantiene deshabilitado -- CONFIRMADO con el pseudo-C real
    // (out_ghidra.c:220215-220381, DrawOP_ENLARGE_Compress_16), no por
    // precaucion sin verificar: el CUERPO ENTERO de la funcion esta detras de
    // `if (alpha_context < 0x100 && alpha_context != 0)` (leido via
    // MC_grpGetContext) -- si esa condicion es falsa la funcion real NO
    // dibuja NADA, y si es verdadera SIEMPRE hace un blend alpha por pixel
    // (formula de interpolacion RGB565 completa, dos variantes de redondeo
    // segun otro flag de contexto). No existe un camino "opaco puro" en la
    // version real de este draw-op -- DrawOP_ENLARGE_Compress_16_Vita (mas
    // abajo) solo escribe opaco, asi que NO es una version incompleta de
    // ENLARGE, es una operacion DISTINTA. Reimplementar el blend correcto
    // requeriria confirmar el ABI real de MC_grpGetContext (que "4" significa
    // como segundo argumento, rango exacto de alpha_context) -- no verificado
    // con la misma certeza que el resto de este archivo, así que se delega
    // siempre al puntero original real en vez de adivinar (mismo criterio que
    // el guard de headers [-7,-3] de COPY, arriba).
    
    so_unhook(&g_setzeroblendfunc_hook);
    
    ((void (*)(int, void*, void*))
        (g_setzeroblendfunc_hook.thumb_addr ? g_setzeroblendfunc_hook.thumb_addr : g_setzeroblendfunc_hook.addr))
        (enumDrawOP, func, funcClip);
        
    g_setzeroblendfunc_hook = hook_addr(
        g_setzeroblendfunc_hook.thumb_addr ? g_setzeroblendfunc_hook.thumb_addr : g_setzeroblendfunc_hook.addr,
        (uintptr_t) SetZeroBlendFunc_hook);
}

// Fase 87 (port_progress.md): CGxFACharCache::DrawToFrameBuffer recompilado
// nativo en C moderno para ARMv7 (mismo patron/mismo tipo de ganancia
// ESPERADA que SPRITE_DECODE_OPTIMIZE, Fase 43 -- el motor corre codigo ARM
// REAL, no emulado, ver correccion de la Fase 89: la ganancia posible es
// solo de mejor codegen/scheduling, y Fase 86 ya midio que el volumen de
// llamadas a esta funcion es bajo, ~10-18/frame incluso en un menu -- no
// hay base para esperar un cambio de FPS grande aca). Gateado bajo su
// propio define (`ZENONIA_TEXT_NATIVE_OPTIMIZE`, default OFF, ver
// CMakeLists.txt) para no generar warnings de "no usado" en builds que no
// lo activan.
#ifdef ZENONIA_TEXT_NATIVE_OPTIMIZE
static so_hook g_drawtoframebuffer_native_hook;
static uintptr_t g_gxfont_getcommoninfo_addr_sprite = 0;

__attribute__((hot))
static void DrawToFrameBuffer_Vita_Native(void *this_ptr, void *char_unit_info, int screen_x, int screen_y) {
    if (!char_unit_info) return;

    unsigned char *glyph_alpha = *(unsigned char **) ((char *) char_unit_info + 8);
    int glyph_w = *(int *) ((char *) char_unit_info + 0xc);
    int glyph_h = *(int *) ((char *) char_unit_info + 0x10);

    if (!glyph_alpha || glyph_w <= 0 || glyph_h <= 0) return;

    if (!g_gxfont_getcommoninfo_addr_sprite) return;
    int (*get_common_info)(void) = (int (*)(void)) g_gxfont_getcommoninfo_addr_sprite;
    char *ci = (char *) (uintptr_t) get_common_info();
    if (!ci) return;

    int clip_x = *(int *) (ci + 0x38);
    int clip_y = *(int *) (ci + 0x3c);
    int clip_w = *(int *) (ci + 0x40);
    int clip_h = *(int *) (ci + 0x44);

    // Fase 89 (port_progress.md): el clip derecho/inferior compara contra
    // `x + glyph_w`/`y + glyph_h` (ancho/alto ORIGINALES, sin reducir por el
    // clip izquierdo/superior) -- confirmado literal en out_ghidra.c:9926-9942
    // (`iVar17 = iVar17 + iVar5; if (iVar17 < param_2 + uVar15)`, uVar15 =
    // glyph_w SIN modificar). Una version anterior de esta rutina (Fase 87)
    // comparaba contra `x + vis_w` (ya reducido) -- da el mismo resultado
    // casi siempre, pero diverge en el caso raro de un glifo clippeado en
    // AMBOS bordes a la vez (ya documentado como asimetria real del motor,
    // no un bug, en el comentario de DrawToFrameBuffer_hook de dynlib.c,
    // Fase 59 -- se replica igual aca por consistencia con esa version ya
    // verificada, no por haber encontrado un caso visible en la practica).
    int x = screen_x, y = screen_y;
    int left_off = 0, top_off = 0, vis_w, vis_h;

    if (x < clip_x) { left_off = clip_x - x; x = clip_x; vis_w = glyph_w - left_off; }
    else { vis_w = glyph_w; }
    { int clip_right = clip_x + clip_w; if (clip_right < x + glyph_w) vis_w = clip_right - (x + left_off); }

    if (y < clip_y) { top_off = clip_y - y; y = clip_y; vis_h = glyph_h - top_off; }
    else { vis_h = glyph_h; }
    { int clip_bottom = clip_y + clip_h; if (clip_bottom < y + glyph_h) vis_h = clip_bottom - (y + top_off); }

    if (vis_h < 1 || vis_w <= 0) return;

    void *gfx = *(void **)((char *)this_ptr + 0xc);
    if (!gfx) return;

    void **vtable = *(void ***)gfx;
    int (*get_fb_base)(void *) = (int (*)(void *)) vtable[4];
    int (*get_bpp)(void *) = (int (*)(void *)) vtable[5];
    int (*get_row_bytes)(void *) = (int (*)(void *)) vtable[6];

    int fb_base = get_fb_base(gfx);
    int row_bytes = get_row_bytes(gfx);
    int bpp = get_bpp(gfx);

    if (bpp == 0) return;
    int stride_pixels = row_bytes / (bpp >> 3);

    uint32_t reg = *(uint32_t *) ((char *) this_ptr + 0x48);
    uint32_t r5 = (reg << 8) >> 27;
    uint32_t g6 = (reg << 16) >> 26;
    uint32_t b5 = (reg & 0xff) >> 3;

    uint16_t *dest_row = (uint16_t *)(intptr_t)(fb_base + (stride_pixels * y + x) * 2);
    const unsigned char *src_row = glyph_alpha + (size_t)glyph_w * top_off + left_off;

    for (int row = 0; row < vis_h; row++) {
        uint16_t *restrict dest = dest_row;
        const unsigned char *restrict src = src_row;
        for (int col = 0; col < vis_w; col++) {
            uint32_t alpha = *src++;
            if (alpha == 0) {
                dest++;
            } else {
                uint32_t inv_alpha = 256 - alpha;
                uint16_t dst_color = *dest;
                uint32_t dst_r = dst_color >> 11;
                uint32_t dst_g = (dst_color >> 5) & 0x3f;
                uint32_t dst_b = dst_color & 0x1f;

                uint32_t new_r = (r5 * alpha + dst_r * inv_alpha) >> 8;
                uint32_t new_g = (g6 * alpha + dst_g * inv_alpha) >> 8;
                uint32_t new_b = (b5 * alpha + dst_b * inv_alpha) >> 8;

                *dest++ = (uint16_t)((new_r << 11) | (new_g << 5) | new_b);
            }
        }
        dest_row += stride_pixels;
        src_row += glyph_w;
    }
}
#endif // ZENONIA_TEXT_NATIVE_OPTIMIZE

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

void zenonia_install_text_native_optimize(so_module *mod) {
#ifdef ZENONIA_TEXT_NATIVE_OPTIMIZE
    uintptr_t draw_to_fb_addr = so_symbol(mod, "_ZN14CGxFACharCache17DrawToFrameBufferEPN4gxfa12CharUnitInfoEii");
    g_gxfont_getcommoninfo_addr_sprite = so_symbol(mod, "GxFont_GetCommonInfo");
    if (draw_to_fb_addr && g_gxfont_getcommoninfo_addr_sprite) {
        g_drawtoframebuffer_native_hook = hook_addr(draw_to_fb_addr, (uintptr_t) DrawToFrameBuffer_Vita_Native);
        so_flush_caches(mod);
        game_log("[TextNativeOptimize] hook nativo instalado en CGxFACharCache::DrawToFrameBuffer (0x%08x)\n", (unsigned int) draw_to_fb_addr);
    } else {
        game_log("[TextNativeOptimize] error: no se encontro CGxFACharCache::DrawToFrameBuffer o GxFont_GetCommonInfo\n");
    }
#endif
}
