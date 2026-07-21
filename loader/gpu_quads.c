// Variante 2 (--gpu-quads-test), rediseno Fase 50. Ver gpu_quads.h para la
// arquitectura completa (FB base directo DEBAJO, todos los ops como quads
// ENCIMA en orden de llamada, decode dual negro/blanco -> textura RGBA
// premultiplicada, blend unico GL_ONE/GL_ONE_MINUS_SRC_ALPHA).
#ifdef ZENONIA_GPU_QUADS

#include <vitaGL.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "gpu_quads.h"

extern void game_log(const char *fmt, ...);

// ---- proyeccion ----
static int g_screen_w = 960;
static int g_screen_h = 544;

// ---- scratch RGB565 (dims del main screen; el motor decodifica con su stride) ----
static uint16_t *g_scratch = NULL;

// framebuffer alternativo falso para param_9: el motor solo lee el offset 0x10
// (puntero de pixeles). Reservamos de sobra y ponemos el resto en 0.
static uint32_t g_fake_fb[16];

// ---- cache de texturas por (data,pal,op,p8) ----
// op y p8 forman parte de la clave porque el blitter real hornea su efecto
// (colorize con color p8, alpha global p8, etc.) en el resultado del decode:
// el mismo (data,pal) con otro (op,p8) es una textura DISTINTA.
#define GPU_QUADS_CACHE_CAP 4096
typedef struct {
    const void *data;
    const void *pal;
    int op;
    int p8;
    unsigned long hash;
    GLuint tex;
    int w, h;
    int used;
    int ready;
} gq_entry;
static gq_entry g_cache[GPU_QUADS_CACHE_CAP];
static int g_cache_count = 0;
static int g_evict_ring = 0;

// ---- cola de quads por frame ----
// Fase 61: w,h propios del quad (no siempre e->w/e->h) -- necesario para
// gpu_quads_enqueue_fill(), donde una textura de 1x1 cacheada por color se
// estira a CUALQUIER tamano de pantalla (un relleno solido no tiene "tamano
// decodificado" propio, a diferencia de un sprite/glifo).
#define GPU_QUADS_QUEUE_CAP 8192
typedef struct { gq_entry *e; float x, y, w, h; } gq_quad;
static gq_quad gq_quads[GPU_QUADS_QUEUE_CAP];
static int gq_quads_count = 0;

// buffers temporales del decode dual (max sprite razonable)
#define GPU_QUADS_MAX_W 1024
#define GPU_QUADS_MAX_H 1024
static uint32_t g_rgba_tmp[GPU_QUADS_MAX_W * GPU_QUADS_MAX_H]; // RGBA premult final
static uint16_t g_pass1_tmp[GPU_QUADS_MAX_W * GPU_QUADS_MAX_H]; // pasada negra (d0)

static unsigned long gq_hash(const void *data, const void *pal, int op, int p8, int w, int h) {
    unsigned long h_val = (unsigned long) data * 2654435761u;
    h_val ^= (unsigned long) pal * 40503u;
    h_val ^= (unsigned long) op * 97u;
    h_val ^= (unsigned long) p8 * 8191u;
    
    // Si la memoria es un buffer dinamico (heap), sampleamos bytes reales para
    // evitar que se cachee el primer frame y nunca se actualice (ej: mapa,
    // textos, splash). Esto previene los "textos faltantes" y la "pantalla
    // blanca". OJO: `data` es el buffer FUENTE tal cual lo recibe
    // PutCompressImg (comprimido/paletizado para sprites .pzx normales, o el
    // buffer crudo del char-cache de texto) -- su longitud real NO tiene
    // relacion con w*h (esa es la dimension DECODIFICADA de salida). La
    // version anterior indexaba `d[w*h/2]`/`d[w*h-1]` como si `data` fuera un
    // buffer de pixeles crudo de esas dimensiones -- para un sprite
    // comprimido tipico esos offsets caen MUY por fuera del buffer real
    // (lectura fuera de los limites del heap). Se limita a una ventana fija
    // chica desde el inicio, que siempre esta dentro de cualquier buffer no
    // vacio, sin asumir nada sobre su formato o longitud.
    if (data && (unsigned long) data >= 0x40000000) {
        const unsigned char *d = (const unsigned char *) data;
        h_val ^= d[0] * 13u;
        h_val ^= d[1] * 37u;
        h_val ^= d[2] * 71u;
        h_val ^= d[3] * 113u;
    }
    return h_val;
}

static gq_entry *gq_find(const void *data, const void *pal, int op, int p8, int w, int h, unsigned long hash_val) {
    if (g_cache_count == 0) return NULL;
    unsigned long start = hash_val % GPU_QUADS_CACHE_CAP;
    for (int i = 0; i < GPU_QUADS_CACHE_CAP; i++) {
        int idx = (start + i) % GPU_QUADS_CACHE_CAP;
        gq_entry *e = &g_cache[idx];
        if (!e->used) return NULL;
        // Fase 55: ademas de comparar punteros, la validacion final de cache dinamico 
        // depende de que el hash sea el mismo (si el contenido cambio, hash sera distinto y creara nueva textura)
        if (e->data == data && e->pal == pal && e->op == op && e->p8 == p8 && e->hash == hash_val) return e;
    }
    return NULL;
}

static gq_entry *gq_insert_slot(const void *data, const void *pal, int op, int p8, int w, int h, unsigned long hash_val) {
    unsigned long start = hash_val % GPU_QUADS_CACHE_CAP;
    for (int i = 0; i < GPU_QUADS_CACHE_CAP; i++) {
        int idx = (start + i) % GPU_QUADS_CACHE_CAP;
        if (!g_cache[idx].used) {
            g_cache_count++;
            return &g_cache[idx];
        }
    }
    // cache llena: desaloja en anillo (borra la textura vieja y reusa el slot)
    // Fase 51 (2026-07-20): diagnostico -- con TODOS los enumDrawOP desviados
    // (Fase 50, antes solo COPY) y la clave de cache ahora incluyendo (op,p8),
    // el mismo sprite con cada paso de fade/colorize es una entrada NUEVA --
    // la cache de 4096 puede llenarse mucho mas rapido que antes. Si se llena
    // en regimen (no solo una vez al cargar assets), hay TRASHING: un sprite
    // reusado se desaloja y se re-decodifica todo el tiempo, pudiendo faltar
    // 1 frame cada vez que esto pasa -- candidato para el reporte de
    // mapa/casas/bordes del mundo faltantes si esos elementos usan muchas
    // variantes de p8 (fades) sobre el mismo (data,pal).
    static int evict_log = 0;
    if (evict_log < 40) {
        game_log("[GpuQuadsDiag] cache llena (%d entradas) -- desalojando en anillo\n", GPU_QUADS_CACHE_CAP);
        evict_log++;
    }
    gq_entry *e = &g_cache[g_evict_ring];
    g_evict_ring = (g_evict_ring + 1) % GPU_QUADS_CACHE_CAP;
    if (e->tex) glDeleteTextures(1, &e->tex);
    e->used = 0;
    e->tex = 0;
    return e;
}

void gpu_quads_init(int screen_w, int screen_h) {
    g_screen_w = screen_w;
    g_screen_h = screen_h;
    if (!g_scratch) {
        g_scratch = (uint16_t *) malloc(GPU_QUADS_SCRATCH_W * GPU_QUADS_SCRATCH_H * sizeof(uint16_t));
    }
    memset(g_fake_fb, 0, sizeof(g_fake_fb));
    // offset 0x10 (16 bytes = indice 4 en uint32_t) = puntero de pixeles scratch
    g_fake_fb[4] = (uint32_t) (uintptr_t) g_scratch;
    game_log("[GpuQuads] init (Fase 50: dual-decode premult) scratch=%p screen=%dx%d cache_cap=%d\n",
             (void *) g_scratch, screen_w, screen_h, GPU_QUADS_CACHE_CAP);
}

int gpu_quads_lookup_and_enqueue(const void *data, const void *pal, int op, int p8, int x, int y, int w, int h) {
    if (gq_quads_count >= GPU_QUADS_QUEUE_CAP) return 0;
    unsigned long hash_val = gq_hash(data, pal, op, p8, w, h);
    gq_entry *e = gq_find(data, pal, op, p8, w, h, hash_val);
    if (e && e->tex) {
        gq_quads[gq_quads_count].e = e;
        gq_quads[gq_quads_count].x = (float) x;
        gq_quads[gq_quads_count].y = (float) y;
        gq_quads[gq_quads_count].w = (float) e->w;
        gq_quads[gq_quads_count].h = (float) e->h;
        gq_quads_count++;
        return 1;
    }
    return 0;
}

// RGB565 -> canales 8 bits por replicacion de bits (mismo criterio que
// convert_rgb565_to_rgba8888 en dynlib.c).
static inline void rgb565_split(uint16_t p, int *r, int *g, int *b) {
    int r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}

void *gpu_quads_begin_pass(int w, int h, uint16_t fill) {
    if (!g_scratch) return NULL;
    if (w <= 0 || h <= 0 || w > GPU_QUADS_MAX_W || h > GPU_QUADS_MAX_H) return NULL;
    if (w > GPU_QUADS_SCRATCH_W || h > GPU_QUADS_SCRATCH_H) return NULL;

    for (int row = 0; row < h; row++) {
        uint16_t *dst = g_scratch + row * GPU_QUADS_SCRATCH_W;
        for (int col = 0; col < w; col++) {
            dst[col] = fill;
        }
    }
    return g_fake_fb;
}

void gpu_quads_capture_pass1(int w, int h) {
    for (int row = 0; row < h; row++) {
        const uint16_t *src = g_scratch + row * GPU_QUADS_SCRATCH_W;
        memcpy(g_pass1_tmp + row * w, src, (size_t) w * sizeof(uint16_t));
    }
}

void gpu_quads_finish_decode(const void *data, const void *pal, int op, int p8,
                             int screen_x, int screen_y, int w, int h, int dual) {
    if (!g_scratch) return;
    if (w <= 0 || h <= 0 || w > GPU_QUADS_MAX_W || h > GPU_QUADS_MAX_H) return;

    for (int row = 0; row < h; row++) {
        const uint16_t *d1row = g_scratch + row * GPU_QUADS_SCRATCH_W; // pasada blanca (o unica)
        const uint16_t *d0row = dual ? (g_pass1_tmp + row * w) : d1row;          // pasada negra
        uint32_t *dst = g_rgba_tmp + row * w;
        for (int col = 0; col < w; col++) {
            int r0, g0, b0;
            rgb565_split(d0row[col], &r0, &g0, &b0);
            uint32_t a;
            if (!dual) {
                // op additive (una sola pasada sobre negro): alpha 0 + color
                // premultiplicado = src + dest exacto con el blend unico.
                a = 0;
            } else {
                int r1, g1, b1;
                rgb565_split(d1row[col], &r1, &g1, &b1);
                // Operador afin en dest: result = c + (1-a)*dest.
                // Sobre negro: d0 = c. Sobre blanco: d1 = c + (1-a)*255.
                // => a = 255 - (d1 - d0), promediando canales (el ruido de la
                // replicacion de bits 5/6->8 se compensa).
                int diff = ((r1 - r0) + (g1 - g0) + (b1 - b0)) / 3;
                if (diff < 0) diff = 0;
                if (diff > 255) diff = 255;
                a = (uint32_t) (255 - diff);
            }
            // Premultiplicado: rgb = c (= d0) tal cual, SIN dividir por a.
            dst[col] = (a << 24) | ((uint32_t) b0 << 16) | ((uint32_t) g0 << 8) | (uint32_t) r0;
        }
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba_tmp);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    unsigned long hash_val = gq_hash(data, pal, op, p8, w, h);
    gq_entry *e = gq_insert_slot(data, pal, op, p8, w, h, hash_val);
    e->data = data;
    e->pal = pal;
    e->op = op;
    e->p8 = p8;
    e->hash = hash_val;
    e->tex = tex;
    e->w = w;
    e->h = h;
    e->used = 1;
    e->ready = 1;

    if (gq_quads_count < GPU_QUADS_QUEUE_CAP) {
        gq_quads[gq_quads_count].e = e;
        gq_quads[gq_quads_count].x = (float) screen_x;
        gq_quads[gq_quads_count].y = (float) screen_y;
        gq_quads[gq_quads_count].w = (float) w;
        gq_quads[gq_quads_count].h = (float) h;
        gq_quads_count++;
    }
}

// Fase 59: discriminador de "op" fuera del rango real de enumDrawOP (0-19
// observados en consola hasta ahora) para que un glifo de texto nunca
// colisione en la cache con un sprite legitimo que por casualidad comparta
// puntero/ancho/alto (no deberia pasar -- distintos buffers -- pero es gratis
// evitarlo).
#define GQ_TEXT_OP 0x10000

void gpu_quads_enqueue_text(const unsigned char *alpha, int aw,
                             unsigned char r, unsigned char g, unsigned char b,
                             int screen_x, int screen_y, int w, int h) {
    if (!alpha || w <= 0 || h <= 0) return;
    if (w > GPU_QUADS_MAX_W || h > GPU_QUADS_MAX_H) return;
    if (gq_quads_count >= GPU_QUADS_QUEUE_CAP) return;

    int p8_color = ((int) r << 16) | ((int) g << 8) | (int) b;
    unsigned long hash_val = gq_hash(alpha, NULL, GQ_TEXT_OP, p8_color, w, h);
    gq_entry *e = gq_find(alpha, NULL, GQ_TEXT_OP, p8_color, w, h, hash_val);

    if (!e) {
        // Premultiplicado (rgb = color*alpha/255, a = alpha) para reusar el
        // mismo glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA) de gpu_quads_flush
        // sin agregar un segundo modo de blend solo para texto.
        for (int row = 0; row < h; row++) {
            const unsigned char *srow = alpha + (size_t) row * aw;
            uint32_t *drow = g_rgba_tmp + row * w;
            for (int col = 0; col < w; col++) {
                uint32_t a = srow[col];
                uint32_t pr = (uint32_t) r * a / 255;
                uint32_t pg = (uint32_t) g * a / 255;
                uint32_t pb = (uint32_t) b * a / 255;
                drow[col] = (a << 24) | (pb << 16) | (pg << 8) | pr;
            }
        }

        GLuint tex = 0;
        glGenTextures(1, &tex);
        if (!tex) return;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba_tmp);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        e = gq_insert_slot(alpha, NULL, GQ_TEXT_OP, p8_color, w, h, hash_val);
        e->data = alpha;
        e->pal = NULL;
        e->op = GQ_TEXT_OP;
        e->p8 = p8_color;
        e->hash = hash_val;
        e->tex = tex;
        e->w = w;
        e->h = h;
        e->used = 1;
        e->ready = 1;
    }

    if (e && e->tex) {
        gq_quads[gq_quads_count].e = e;
        gq_quads[gq_quads_count].x = (float) screen_x;
        gq_quads[gq_quads_count].y = (float) screen_y;
        gq_quads[gq_quads_count].w = (float) w;
        gq_quads[gq_quads_count].h = (float) h;
        gq_quads_count++;
    }
}

// Fase 61: relleno solido (CGsGraphics::DrawFillRect/DrawRoundRect/etc. via
// Screen_RGBBLEND16_Real, ver dynlib.c) -- mismo problema de fondo que el
// texto de la Fase 59 (escribe DIRECTO al framebuffer de software, que
// GPU_QUADS compone SIEMPRE debajo de los sprites/paneles desviados a GPU) y
// mismo fix (encolar como quad mas en la MISMA cola). A diferencia de un
// sprite/glifo, un relleno solido no tiene "datos" que decodificar -- una
// textura de 1x1 con el color premultiplicado, cacheada por (color,alpha),
// alcanza: GL estira ese unico texel sobre CUALQUIER tamano de quad via
// texture coordinates (0..1), sin necesidad de generar una textura del
// tamano real del rectangulo.
#define GQ_FILL_OP 0x10001

// Fase 66: diagnostico de orden (ver el comentario largo en
// gpu_quads_enqueue_fill) -- posiciones en la cola de los ultimos rellenos
// "de panel" (w*h>500) encolados este frame; gpu_quads_flush() los reporta
// y limpia la lista antes de resetear gq_quads_count.
#define GQ_WATCH_CAP 16
static int g_watch_idx[GQ_WATCH_CAP];
static int g_watch_x[GQ_WATCH_CAP];
static int g_watch_y[GQ_WATCH_CAP];
static int g_watch_w[GQ_WATCH_CAP];
static int g_watch_h[GQ_WATCH_CAP];
static int g_watch_count = 0;

void gpu_quads_enqueue_fill(unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                             int screen_x, int screen_y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (gq_quads_count >= GPU_QUADS_QUEUE_CAP) return;

    // No hay puntero de datos real -- se usa un sentinel fijo no-NULL como
    // "data" (gq_hash/gq_find lo tratan como cualquier otro puntero, nunca se
    // dereferencia) y se empaqueta (color,alpha) en `p8` como clave completa.
    static const int fill_sentinel = 0;
    int p8_color = ((int) r << 16) | ((int) g << 8) | (int) b;
    int key = (p8_color << 8) | a;

    // Fase 65 (2026-07-20): fast-path MRU -- confirmado en consola real
    // (Fase 63/64) que el minimapa dibuja CIENTOS de bloques de 4x4/4x20 por
    // frame, la enorme mayoria en corridas consecutivas del MISMO
    // (color,alpha) -- evita gq_hash/gq_find (O(1) esperado, pero no gratis
    // a este volumen) cuando la llamada actual usa la MISMA clave que la
    // inmediatamente anterior. Colaboro con el bajon de FPS reportado en un
    // area con mucho detalle de minimapa.
    static int last_key = -1;
    static gq_entry *last_entry = NULL;
    gq_entry *e;
    // Verificar que la entrada siga siendo REALMENTE la nuestra (no una
    // reusada por el anillo de desalojo, gq_insert_slot, para otra clave)
    // antes de confiar en el atajo -- barato (3 comparaciones) comparado con
    // el costo de servir la textura equivocada.
    if (last_entry && key == last_key && last_entry->used &&
        last_entry->op == GQ_FILL_OP && last_entry->p8 == key) {
        e = last_entry;
    } else {
        unsigned long hash_val = gq_hash(&fill_sentinel, NULL, GQ_FILL_OP, key, 1, 1);
        e = gq_find(&fill_sentinel, NULL, GQ_FILL_OP, key, 1, 1, hash_val);

        if (!e) {
            uint32_t pr = (uint32_t) r * a / 255;
            uint32_t pg = (uint32_t) g * a / 255;
            uint32_t pb = (uint32_t) b * a / 255;
            uint32_t texel = ((uint32_t) a << 24) | (pb << 16) | (pg << 8) | pr;

            GLuint tex = 0;
            glGenTextures(1, &tex);
            if (!tex) return;
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &texel);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            e = gq_insert_slot(&fill_sentinel, NULL, GQ_FILL_OP, key, 1, 1, hash_val);
            e->data = &fill_sentinel;
            e->pal = NULL;
            e->op = GQ_FILL_OP;
            e->p8 = key;
            e->hash = hash_val;
            e->tex = tex;
            e->w = 1;
            e->h = 1;
            e->used = 1;
            e->ready = 1;
        }
        last_key = key;
        last_entry = e;
    }

    if (e && e->tex) {
        // Fase 66 (port_progress.md): diagnostico de ORDEN -- hipotesis de
        // que los backdrops in-game (item/tip/guardado) SI pasan por este
        // mismo mecanismo (confirmado por analisis de out_ghidra.c:
        // CMvGameUI::DrawExplainBackPlane -> DrawFillRoundRect -> RGBBlend16,
        // el MISMO camino ya arreglado para el menu de pausa) pero quedan
        // TAPADOS por sprites encolados DESPUES en la MISMA cola de quads
        // (dibujados arriba, sin importar el orden real del motor si el
        // backdrop se encolo antes) -- a diferencia del menu de pausa, que
        // probablemente es lo ULTIMO que dibuja el motor en su frame (nada
        // queda encolado despues). Se filtra a rellenos "de panel" (w*h>500,
        // el minimapa usa bloques de 4x4/4x20 -- irrelevantes aca) para no
        // repetir el ruido de las Fases 63/64. gpu_quads_flush() lee/loguea
        // g_watch_* y los resetea cada frame.
        if (w * h > 500 && g_watch_count < GQ_WATCH_CAP) {
            g_watch_idx[g_watch_count] = gq_quads_count;
            g_watch_x[g_watch_count] = screen_x;
            g_watch_y[g_watch_count] = screen_y;
            g_watch_w[g_watch_count] = w;
            g_watch_h[g_watch_count] = h;
            g_watch_count++;
        }
        gq_quads[gq_quads_count].e = e;
        gq_quads[gq_quads_count].x = (float) screen_x;
        gq_quads[gq_quads_count].y = (float) screen_y;
        gq_quads[gq_quads_count].w = (float) w;
        gq_quads[gq_quads_count].h = (float) h;
        gq_quads_count++;
    }
}

int gpu_quads_flush(void) {
    // Fase 66/67: reportar diagnostico de orden ANTES de resetear
    // gq_quads_count. "residual" (Fase 66) solo contaba CUANTOS quads mas se
    // encolaron despues -- no decia si alguno realmente se SUPERPONE con el
    // panel en pantalla (podrian ser elementos chicos en otra zona, sin tapar
    // nada). Fase 67: ademas del residual, escanea esos quads posteriores y
    // reporta cuantos tienen un rectangulo que REALMENTE INTERSECTA al panel
    // (test AABB simple) y son lo bastante grandes para importar (w*h>100,
    // descarta puntitos de minimapa) -- evidencia directa de "algo lo tapa",
    // en vez de inferirlo solo del orden. Capado a 60 lineas totales.
    if (g_watch_count > 0) {
        // Fase 68: el cupo de 60 lineas (Fase 66/67) se agotaba re-logueando
        // el MISMO puñado de paneles fijos que se repiten cada frame (misma
        // falla que la Fase 63/64 con el otro diagnostico) -- confirmado en
        // consola: un elemento en (542,535,110,5) agoto el cupo entero antes
        // de llegar al evento de recoger el objeto, que el usuario reporto
        // que paso "casi al final" del log. Dedupe por CONJUNTO de
        // posiciones+tamanos ya vistos (ignora residual/overlap_count, que
        // varian de a poco cada frame y nunca "convergen" con un dedupe
        // exacto) -- cada panel DISTINTO (x,y,w,h) se reporta una sola vez,
        // dejando cupo libre para un panel nuevo (un popup real) que aparezca
        // despues de que el HUD fijo ya se vio.
        static int seen_x[96], seen_y[96], seen_w[96], seen_h[96];
        static int seen_count = 0;
        for (int i = 0; i < g_watch_count; i++) {
            int px0 = g_watch_x[i], py0 = g_watch_y[i];
            int pw = g_watch_w[i], ph = g_watch_h[i];
            int already_seen = 0;
            for (int k = 0; k < seen_count; k++) {
                if (seen_x[k] == px0 && seen_y[k] == py0 && seen_w[k] == pw && seen_h[k] == ph) {
                    already_seen = 1;
                    break;
                }
            }
            if (already_seen || seen_count >= 96) continue;
            seen_x[seen_count] = px0; seen_y[seen_count] = py0;
            seen_w[seen_count] = pw; seen_h[seen_count] = ph;
            seen_count++;

            int residual = gq_quads_count - g_watch_idx[i] - 1;
            int px1 = px0 + pw, py1 = py0 + ph;
            int overlap_count = 0;
            int overlap_max_area = 0;
            for (int j = g_watch_idx[i] + 1; j < gq_quads_count; j++) {
                gq_quad *q = &gq_quads[j];
                int qx0 = (int) q->x, qy0 = (int) q->y;
                int qx1 = qx0 + (int) q->w, qy1 = qy0 + (int) q->h;
                int area = (int) (q->w * q->h);
                if (area <= 100) continue; // descarta puntitos de minimapa (4x4/4x20)
                if (qx0 < px1 && qx1 > px0 && qy0 < py1 && qy1 > py0) {
                    overlap_count++;
                    if (area > overlap_max_area) overlap_max_area = area;
                }
            }
            game_log("[GpuQuadsDiag] panel x=%d y=%d w=%d h=%d idx=%d residual=%d overlap_count=%d overlap_max_area=%d\n",
                     px0, py0, pw, ph, g_watch_idx[i], residual, overlap_count, overlap_max_area);
        }
        g_watch_count = 0;
    }

    if (gq_quads_count == 0) return 0;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0.0f, (float) g_screen_w, (float) g_screen_h, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    // Blend UNICO para todos los ops: texturas premultiplicadas (ver
    // gpu_quads.h). out = src + dest*(1-alpha) reproduce copy/skip/blend/
    // multiply/colorize (afines) y additive (alpha=0) sin cambiar de estado.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    static const float tc[8] = { 0, 0,  1, 0,  0, 1,  1, 1 };

    // Fase 65: evita glBindTexture repetido cuando el quad actual usa la
    // MISMA textura que el anterior (corridas largas del mismo relleno --
    // el minimapa encola cientos de bloques consecutivos del mismo color,
    // Fase 63/64) -- una llamada a la API GL menos por quad en ese caso, sin
    // cambiar que se dibuja.
    GLuint bound_tex = 0;
    for (int i = 0; i < gq_quads_count; i++) {
        gq_quad *q = &gq_quads[i];
        gq_entry *e = q->e;
        float x0 = q->x, y0 = q->y, x1 = q->x + q->w, y1 = q->y + q->h;
        float verts[8] = { x0, y0,  x1, y0,  x0, y1,  x1, y1 };
        if (e->tex != bound_tex) {
            glBindTexture(GL_TEXTURE_2D, e->tex);
            bound_tex = e->tex;
        }
        glVertexPointer(2, GL_FLOAT, 0, verts);
        glTexCoordPointer(2, GL_FLOAT, 0, tc);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    int drawn = gq_quads_count;
    gq_quads_count = 0; // reset para el proximo frame
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    // El resto del estado pisado lo restaura el caller
    // (gpu_quads_restore_engine_state en dynlib.c) desde la sombra trackeada.
    return drawn;
}

#endif // ZENONIA_GPU_QUADS
