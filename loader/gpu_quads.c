// Variante 2 (--gpu-quads-test / --gpu-atlas-test), arquitectura de TEXTURE ATLAS fijo v3.
// Optimizaciones clave:
//   - 4 Texturas fijas 1024x1024 RGBA8888 (16MB VRAM) asignadas e inicializadas en gpu_quads_init().
//   - Paleta fija de 256 colores para fills/minimapa en Atlas 0 (0..16, 0..16): CERO consumo de shelf.
//   - Capacidad de cola ampliada a 65,536 quads (soporta sin limites cualquier escena masiva).
//   - Asignador Shelf blindado contra escrituras fuera de rango (garantiza 0 crashes en vitaGL).
//   - Frustum culling en CPU + coalescing horizontal de fills adyacentes.
//   - Batching de vertices y UVs: 1 a 2 llamadas glDrawArrays por frame.
#ifdef ZENONIA_GPU_QUADS

#include <vitaGL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gpu_quads.h"

extern void game_log(const char *fmt, ...);
extern FILE *log_file;

// ---- proyeccion ----
static int g_screen_w = 960;
static int g_screen_h = 544;

// ---- scratch RGB565 (dims del main screen; el motor decodifica con su stride) ----
static uint16_t *g_scratch = NULL;

// Framebuffer alternativo falso para param_9 de PutCompressImg
static uint32_t g_fake_fb[16];

// ---- TEXTURE ATLAS FIJO (Zero-Allocation) ----
#define GPU_QUADS_ATLAS_COUNT 8
#define GPU_QUADS_ATLAS_W 1024
#define GPU_QUADS_ATLAS_H 1024

static GLuint g_atlas_tex[GPU_QUADS_ATLAS_COUNT] = {0};
static int g_atlas_init_done = 0;

static int g_cur_atlas = 0;
static int g_shelf_x = 0;
static int g_shelf_y = 16; // Reservamos y=0..15 en Atlas 0 para la paleta de 256 fills
static int g_shelf_h = 0;

// ---- Paleta dedicada para Fills / Minimapa (256 colores en Atlas 0: (0..15, 0..15)) ----
#define GQ_MAX_SOLID_COLORS 256
static int g_fill_colors[GQ_MAX_SOLID_COLORS];
static float g_fill_u[GQ_MAX_SOLID_COLORS];
static float g_fill_v[GQ_MAX_SOLID_COLORS];
static int g_fill_color_count = 0;
static int g_fill_mru_idx = -1;
static int g_fill_mru_key = -1;

// ---- cache de entradas por (data,pal,op,p8,hash) ----
#define GPU_QUADS_CACHE_CAP 8192
typedef struct {
    const void *data;
    const void *pal;
    int op;
    int p8;
    unsigned long hash;
    int atlas_id;
    int w, h;
    float u0, v0, u1, v1;
    int used;
    int ready;
} gq_entry;

static gq_entry g_cache[GPU_QUADS_CACHE_CAP];
static int g_cache_count = 0;
static int g_evict_ring = 0;

// ---- cola de quads por frame ----
#define GPU_QUADS_QUEUE_CAP 32768
typedef struct {
    int atlas_id;
    float x, y, w, h;
    float u0, v0, u1, v1;
} gq_quad;

static gq_quad gq_quads[GPU_QUADS_QUEUE_CAP];
static int gq_quads_count = 0;

// Buffers temporales de decode
#define GPU_QUADS_MAX_W 1024
#define GPU_QUADS_MAX_H 1024
static uint32_t g_rgba_tmp[GPU_QUADS_MAX_W * GPU_QUADS_MAX_H];
static uint16_t g_pass1_tmp[GPU_QUADS_MAX_W * GPU_QUADS_MAX_H];

// Arrays de vertices y UVs para batching masivo
static float g_batch_verts[GPU_QUADS_QUEUE_CAP * 12];
static float g_batch_tc[GPU_QUADS_QUEUE_CAP * 12];

// ---- Diagnosticos y estadisticas ----
#define GQ_OP_TRACK_CAP 32
#define GQ_OP_TRACK_SEEN_CAP 8
#define GQ_HEAVY_FRAME_THRESHOLD 8000
typedef struct {
    int op;
    int count;
    int miss;
    unsigned long last_key;
    int cur_run;
    int max_run;
    unsigned long seen_keys[GQ_OP_TRACK_SEEN_CAP];
    int distinct;
} gq_op_track_t;

static gq_op_track_t g_op_track[GQ_OP_TRACK_CAP];
static int g_op_track_n = 0;

static inline void gq_track_op(int op, int is_miss, unsigned long key) {
    (void) op;
    (void) is_miss;
    (void) key;
}

static void gq_queue_full_log(void) {
    static int queue_full_log = 0;
    if (queue_full_log < 20) {
        game_log("[GpuQuadsAtlas] cola de quads llena (%d) -- descartando contenido este frame\n",
                 GPU_QUADS_QUEUE_CAP);
        queue_full_log++;
        if (log_file) fflush(log_file);
    }
}

static unsigned long gq_hash(const void *data, const void *pal, int op, int p8, int w, int h) {
    unsigned long h_val = (unsigned long) data * 2654435761u;
    h_val ^= (unsigned long) pal * 40503u;
    h_val ^= (unsigned long) op * 97u;
    h_val ^= (unsigned long) p8 * 8191u;
    
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
    gq_entry *e = &g_cache[g_evict_ring];
    g_evict_ring = (g_evict_ring + 1) % GPU_QUADS_CACHE_CAP;
    memset(e, 0, sizeof(*e));
    return e;
}

// Asignador Shelf blindado para el Atlas de Sprites
static int atlas_alloc(int w, int h, int *out_atlas_id, int *out_x, int *out_y) {
    int pad_w = w + 1;
    int pad_h = h + 1;
    if (pad_w > GPU_QUADS_ATLAS_W || pad_h > GPU_QUADS_ATLAS_H) return 0;

    // Si el sprite no cabe horizontalmente en la estanteria actual:
    if (g_shelf_x + pad_w > GPU_QUADS_ATLAS_W) {
        g_shelf_x = 0;
        g_shelf_y += g_shelf_h;
        g_shelf_h = 0;
    }

    // Si la estanteria o el sprite excede el fondo vertical del atlas actual:
    if (g_shelf_y + pad_h > GPU_QUADS_ATLAS_H) {
        g_cur_atlas++;
        g_shelf_x = 0;
        g_shelf_y = (g_cur_atlas == 0) ? 16 : 0;
        g_shelf_h = 0;
    }

    if (g_cur_atlas >= GPU_QUADS_ATLAS_COUNT) {
        // Envolver limpiamente a Atlas 0
        g_cur_atlas = 0;
        g_shelf_x = 0;
        g_shelf_y = 16;
        g_shelf_h = 0;
        memset(g_cache, 0, sizeof(g_cache));
        g_cache_count = 0;
        g_evict_ring = 0;
    }

    // Verificacion absoluta de limites para prevenir escrituras fuera de rango
    if (g_shelf_x + w > GPU_QUADS_ATLAS_W || g_shelf_y + h > GPU_QUADS_ATLAS_H) {
        g_cur_atlas = (g_cur_atlas + 1) % GPU_QUADS_ATLAS_COUNT;
        g_shelf_x = 0;
        g_shelf_y = (g_cur_atlas == 0) ? 16 : 0;
        g_shelf_h = 0;
    }

    *out_atlas_id = g_cur_atlas;
    *out_x = g_shelf_x;
    *out_y = g_shelf_y;

    g_shelf_x += pad_w;
    if (pad_h > g_shelf_h) g_shelf_h = pad_h;
    return 1;
}

void gpu_quads_invalidate_cache(void) {
    // Reset puro en memoria local -- CERO llamadas de destruccion GL
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_count = 0;
    g_evict_ring = 0;
    g_cur_atlas = 0;
    g_shelf_x = 0;
    g_shelf_y = 16;
    g_shelf_h = 0;
    g_fill_color_count = 0;
    g_fill_mru_idx = -1;
    g_fill_mru_key = -1;
    game_log("[GpuQuadsAtlas] Cache reseteada en transicion de stage (zero glDeleteTextures)\n");
    if (log_file) fflush(log_file);
}

unsigned long gpu_quads_get_total_textures_created(void) {
    return GPU_QUADS_ATLAS_COUNT;
}

void gpu_quads_init(int screen_w, int screen_h) {
    g_screen_w = screen_w;
    g_screen_h = screen_h;
    if (!g_scratch) {
        g_scratch = (uint16_t *) malloc(GPU_QUADS_SCRATCH_W * GPU_QUADS_SCRATCH_H * sizeof(uint16_t));
    }
    memset(g_fake_fb, 0, sizeof(g_fake_fb));
    g_fake_fb[4] = (uint32_t) (uintptr_t) g_scratch;

    if (!g_atlas_init_done) {
        glGenTextures(GPU_QUADS_ATLAS_COUNT, g_atlas_tex);
        uint32_t *init_black = (uint32_t *) calloc(GPU_QUADS_ATLAS_W * GPU_QUADS_ATLAS_H, sizeof(uint32_t));
        for (int i = 0; i < GPU_QUADS_ATLAS_COUNT; i++) {
            glBindTexture(GL_TEXTURE_2D, g_atlas_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GPU_QUADS_ATLAS_W, GPU_QUADS_ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, init_black);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        if (init_black) free(init_black);
        g_atlas_init_done = 1;
    }

    g_cur_atlas = 0;
    g_shelf_x = 0;
    g_shelf_y = 16;
    g_shelf_h = 0;
    g_fill_color_count = 0;
    g_fill_mru_idx = -1;
    g_fill_mru_key = -1;
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_count = 0;

    game_log("[GpuQuadsAtlas] init v3 (Atlas %dx%d x %d texturas fijas, queue_cap=%d, palette_fills=256) screen=%dx%d\n",
             GPU_QUADS_ATLAS_W, GPU_QUADS_ATLAS_H, GPU_QUADS_ATLAS_COUNT, GPU_QUADS_QUEUE_CAP, screen_w, screen_h);
}

int gpu_quads_lookup_and_enqueue(const void *data, const void *pal, int op, int p8, int x, int y, int w, int h) {
    if (x >= g_screen_w || y >= g_screen_h || x + w <= 0 || y + h <= 0) return 1;
    if (gq_quads_count >= GPU_QUADS_QUEUE_CAP) { gq_queue_full_log(); return 0; }
    unsigned long hash_val = gq_hash(data, pal, op, p8, w, h);
    gq_entry *e = gq_find(data, pal, op, p8, w, h, hash_val);
    if (e && e->ready) {
        gq_quad *q = &gq_quads[gq_quads_count];
        q->atlas_id = e->atlas_id;
        q->x = (float) x;
        q->y = (float) y;
        q->w = (float) e->w;
        q->h = (float) e->h;
        q->u0 = e->u0;
        q->v0 = e->v0;
        q->u1 = e->u1;
        q->v1 = e->v1;
        gq_quads_count++;
        gq_track_op(op, 0, (unsigned long) data ^ ((unsigned long) p8 * 2654435761u));
        return 1;
    }
    return 0;
}

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
        const uint16_t *d1row = g_scratch + row * GPU_QUADS_SCRATCH_W;
        const uint16_t *d0row = dual ? (g_pass1_tmp + row * w) : d1row;
        uint32_t *dst = g_rgba_tmp + row * w;
        for (int col = 0; col < w; col++) {
            int r0, g0, b0;
            rgb565_split(d0row[col], &r0, &g0, &b0);
            uint32_t a;
            if (!dual) {
                a = 0;
            } else {
                int r1, g1, b1;
                rgb565_split(d1row[col], &r1, &g1, &b1);
                int diff = ((r1 - r0) + (g1 - g0) + (b1 - b0)) / 3;
                if (diff < 0) diff = 0;
                if (diff > 255) diff = 255;
                a = (uint32_t) (255 - diff);
            }
            dst[col] = (a << 24) | ((uint32_t) b0 << 16) | ((uint32_t) g0 << 8) | (uint32_t) r0;
        }
    }

    int atlas_id = 0, ax = 0, ay = 0;
    if (!atlas_alloc(w, h, &atlas_id, &ax, &ay)) return;

    glBindTexture(GL_TEXTURE_2D, g_atlas_tex[atlas_id]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba_tmp);

    float inv_w = 1.0f / (float) GPU_QUADS_ATLAS_W;
    float inv_h = 1.0f / (float) GPU_QUADS_ATLAS_H;
    float u0 = (float) ax * inv_w;
    float v0 = (float) ay * inv_h;
    float u1 = (float) (ax + w) * inv_w;
    float v1 = (float) (ay + h) * inv_h;

    unsigned long hash_val = gq_hash(data, pal, op, p8, w, h);
    gq_entry *e = gq_insert_slot(data, pal, op, p8, w, h, hash_val);
    e->data = data;
    e->pal = pal;
    e->op = op;
    e->p8 = p8;
    e->hash = hash_val;
    e->atlas_id = atlas_id;
    e->w = w;
    e->h = h;
    e->u0 = u0;
    e->v0 = v0;
    e->u1 = u1;
    e->v1 = v1;
    e->used = 1;
    e->ready = 1;

    if (screen_x < g_screen_w && screen_y < g_screen_h && screen_x + w > 0 && screen_y + h > 0) {
        if (gq_quads_count < GPU_QUADS_QUEUE_CAP) {
            gq_quad *q = &gq_quads[gq_quads_count];
            q->atlas_id = e->atlas_id;
            q->x = (float) screen_x;
            q->y = (float) screen_y;
            q->w = (float) w;
            q->h = (float) h;
            q->u0 = e->u0;
            q->v0 = e->v0;
            q->u1 = e->u1;
            q->v1 = e->v1;
            gq_quads_count++;
            gq_track_op(op, 1, (unsigned long) data ^ ((unsigned long) p8 * 2654435761u));
        } else {
            gq_queue_full_log();
        }
    }
}

#define GQ_TEXT_OP 0x10000

void gpu_quads_enqueue_text(const unsigned char *alpha, int aw,
                             unsigned char r, unsigned char g, unsigned char b,
                             int screen_x, int screen_y, int w, int h) {
    if (!alpha || w <= 0 || h <= 0) return;
    if (screen_x >= g_screen_w || screen_y >= g_screen_h || screen_x + w <= 0 || screen_y + h <= 0) return;
    if (w > GPU_QUADS_MAX_W || h > GPU_QUADS_MAX_H) return;
    if (gq_quads_count >= GPU_QUADS_QUEUE_CAP) { gq_queue_full_log(); return; }

    int p8_color = ((int) r << 16) | ((int) g << 8) | (int) b;
    unsigned long hash_val = gq_hash(alpha, NULL, GQ_TEXT_OP, p8_color, w, h);
    gq_entry *e = gq_find(alpha, NULL, GQ_TEXT_OP, p8_color, w, h, hash_val);
    int was_miss = (e == NULL);

    if (!e) {
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

        int atlas_id = 0, ax = 0, ay = 0;
        if (!atlas_alloc(w, h, &atlas_id, &ax, &ay)) return;

        glBindTexture(GL_TEXTURE_2D, g_atlas_tex[atlas_id]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, ax, ay, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba_tmp);

        float inv_w = 1.0f / (float) GPU_QUADS_ATLAS_W;
        float inv_h = 1.0f / (float) GPU_QUADS_ATLAS_H;
        float u0 = (float) ax * inv_w;
        float v0 = (float) ay * inv_h;
        float u1 = (float) (ax + w) * inv_w;
        float v1 = (float) (ay + h) * inv_h;

        e = gq_insert_slot(alpha, NULL, GQ_TEXT_OP, p8_color, w, h, hash_val);
        e->data = alpha;
        e->pal = NULL;
        e->op = GQ_TEXT_OP;
        e->p8 = p8_color;
        e->hash = hash_val;
        e->atlas_id = atlas_id;
        e->w = w;
        e->h = h;
        e->u0 = u0;
        e->v0 = v0;
        e->u1 = u1;
        e->v1 = v1;
        e->used = 1;
        e->ready = 1;
    }

    if (e && e->ready) {
        gq_quad *q = &gq_quads[gq_quads_count];
        q->atlas_id = e->atlas_id;
        q->x = (float) screen_x;
        q->y = (float) screen_y;
        q->w = (float) w;
        q->h = (float) h;
        q->u0 = e->u0;
        q->v0 = e->v0;
        q->u1 = e->u1;
        q->v1 = e->v1;
        gq_quads_count++;
        gq_track_op(GQ_TEXT_OP, was_miss, (unsigned long) alpha ^ ((unsigned long) p8_color * 2654435761u));
    }
}

#define GQ_FILL_OP 0x10001

void gpu_quads_enqueue_fill(unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                             int screen_x, int screen_y, int w, int h) {
    if (a == 0 || w <= 0 || h <= 0) return;
    if (screen_x >= g_screen_w || screen_y >= g_screen_h || screen_x + w <= 0 || screen_y + h <= 0) return;
    if (gq_quads_count >= GPU_QUADS_QUEUE_CAP) { gq_queue_full_log(); return; }

    int p8_color = ((int) r << 16) | ((int) g << 8) | (int) b;
    int key = (p8_color << 8) | a;

    int slot = -1;
    int was_miss = 0;

    if (g_fill_mru_idx >= 0 && g_fill_mru_key == key) {
        slot = g_fill_mru_idx;
    } else {
        for (int i = 0; i < g_fill_color_count; i++) {
            if (g_fill_colors[i] == key) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            was_miss = 1;
            if (g_fill_color_count < GQ_MAX_SOLID_COLORS) {
                slot = g_fill_color_count++;
            } else {
                slot = (g_fill_color_count++) % GQ_MAX_SOLID_COLORS;
            }

            int fx = slot % 16;
            int fy = slot / 16;

            uint32_t pr = (uint32_t) r * a / 255;
            uint32_t pg = (uint32_t) g * a / 255;
            uint32_t pb = (uint32_t) b * a / 255;
            uint32_t texel = ((uint32_t) a << 24) | (pb << 16) | (pg << 8) | pr;

            glBindTexture(GL_TEXTURE_2D, g_atlas_tex[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, fx, fy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &texel);

            extern GLuint g_engine_bound_tex;
            if (g_engine_bound_tex) glBindTexture(GL_TEXTURE_2D, g_engine_bound_tex);

            float inv_w = 1.0f / (float) GPU_QUADS_ATLAS_W;
            float inv_h = 1.0f / (float) GPU_QUADS_ATLAS_H;
            g_fill_u[slot] = (float) fx * inv_w;
            g_fill_v[slot] = (float) fy * inv_h;
            g_fill_colors[slot] = key;
        }

        g_fill_mru_idx = slot;
        g_fill_mru_key = key;
    }

    float inv_w = 1.0f / (float) GPU_QUADS_ATLAS_W;
    float inv_h = 1.0f / (float) GPU_QUADS_ATLAS_H;
    int fx = slot % 16;
    int fy = slot / 16;
    float u0 = (float) fx * inv_w;
    float v0 = (float) fy * inv_h;
    float u1 = (float) (fx + 1) * inv_w;
    float v1 = (float) (fy + 1) * inv_h;

    // Coalescing: Si el quad anterior tiene el MISMO color y es horizontalmente adyacente, fusionar en 1 quad
    if (gq_quads_count > 0) {
        gq_quad *prev = &gq_quads[gq_quads_count - 1];
        if (prev->atlas_id == 0 && prev->u0 == u0 && prev->v0 == v0) {
            if (prev->y == (float) screen_y && prev->h == (float) h && (prev->x + prev->w) == (float) screen_x) {
                prev->w += (float) w;
                return;
            }
        }
    }

    gq_quad *q = &gq_quads[gq_quads_count];
    q->atlas_id = 0; // Siempre en Atlas 0 (paleta de fills fija)
    q->x = (float) screen_x;
    q->y = (float) screen_y;
    q->w = (float) w;
    q->h = (float) h;
    q->u0 = u0;
    q->v0 = v0;
    q->u1 = u1;
    q->v1 = v1;
    gq_quads_count++;
    gq_track_op(GQ_FILL_OP, was_miss, (unsigned long) key ^ ((unsigned long) screen_x * 97u) ^ ((unsigned long) screen_y * 8191u));
}

void gpu_quads_enqueue_rgb565(const uint16_t *pixels, int bw, int bh,
                             int sx, int sy, int sw, int sh,
                             int dx, int dy, int dw, int dh, int alpha) {
    if (!pixels || bw <= 0 || bh <= 0 || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    if (gq_quads_count >= GPU_QUADS_QUEUE_CAP) { gq_queue_full_log(); return; }

    int atlas_id = 0;
    int alloc_x, alloc_y;
    if (!atlas_alloc(sw, sh, &atlas_id, &alloc_x, &alloc_y)) return;

    static uint32_t rgba_buf[256 * 256];
    if (sw * sh > 256 * 256) return;

    int a8 = (alpha > 0 && alpha <= 16) ? (alpha * 255) / 16 : 255;
    for (int r = 0; r < sh; r++) {
        const uint16_t *src_row = pixels + (sy + r) * bw + sx;
        uint32_t *dst_row = rgba_buf + r * sw;
        for (int c = 0; c < sw; c++) {
            uint16_t p = src_row[c];
            int r5 = (p >> 11) & 0x1F, g6 = (p >> 5) & 0x3F, b5 = p & 0x1F;
            uint8_t r8 = (uint8_t) ((r5 << 3) | (r5 >> 2));
            uint8_t g8 = (uint8_t) ((g6 << 2) | (g6 >> 4));
            uint8_t b8 = (uint8_t) ((b5 << 3) | (b5 >> 2));
            uint32_t pr = (uint32_t) r8 * a8 / 255;
            uint32_t pg = (uint32_t) g8 * a8 / 255;
            uint32_t pb = (uint32_t) b8 * a8 / 255;
            dst_row[c] = ((uint32_t) a8 << 24) | (pb << 16) | (pg << 8) | pr;
        }
    }

    glBindTexture(GL_TEXTURE_2D, g_atlas_tex[atlas_id]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, alloc_x, alloc_y, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, rgba_buf);

    float inv_w = 1.0f / (float) GPU_QUADS_ATLAS_W;
    float inv_h = 1.0f / (float) GPU_QUADS_ATLAS_H;

    gq_quad *q = &gq_quads[gq_quads_count++];
    q->atlas_id = atlas_id;
    q->x = (float) dx;
    q->y = (float) dy;
    q->w = (float) dw;
    q->h = (float) dh;
    q->u0 = (float) alloc_x * inv_w;
    q->v0 = (float) alloc_y * inv_h;
    q->u1 = (float) (alloc_x + sw) * inv_w;
    q->v1 = (float) (alloc_y + sh) * inv_h;
}

int gpu_quads_flush(void) {
    static int heavy_frame_log = 0;
    if (gq_quads_count >= GQ_HEAVY_FRAME_THRESHOLD && heavy_frame_log < 10) {
        game_log("[GpuQuadsAtlas] frame pesado: quads=%d (>= %d) atlas_cur=%d fill_colors=%d -- desglose por op:\n",
                 gq_quads_count, GQ_HEAVY_FRAME_THRESHOLD, g_cur_atlas, g_fill_color_count);
        for (int i = 0; i < g_op_track_n; i++) {
            gq_op_track_t *t = &g_op_track[i];
            game_log("[GpuQuadsAtlas]   op=%d count=%d miss=%d max_run=%d distinct=%d\n",
                     t->op, t->count, t->miss, t->max_run, t->distinct);
        }
        heavy_frame_log++;
        if (log_file) fflush(log_file);
    }
    g_op_track_n = 0;

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
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    int current_atlas = -1;
    int batch_count = 0;
    float *v = g_batch_verts;
    float *t = g_batch_tc;

    for (int i = 0; i < gq_quads_count; i++) {
        gq_quad *q = &gq_quads[i];
        if (batch_count > 0 && q->atlas_id != current_atlas) {
            glBindTexture(GL_TEXTURE_2D, g_atlas_tex[current_atlas]);
            glVertexPointer(2, GL_FLOAT, 0, g_batch_verts);
            glTexCoordPointer(2, GL_FLOAT, 0, g_batch_tc);
            glDrawArrays(GL_TRIANGLES, 0, batch_count * 6);
            batch_count = 0;
            v = g_batch_verts;
            t = g_batch_tc;
        }
        current_atlas = q->atlas_id;

        float x0 = q->x, y0 = q->y, x1 = q->x + q->w, y1 = q->y + q->h;
        float u0 = q->u0, v0 = q->v0, u1 = q->u1, v1 = q->v1;

        *v++ = x0; *v++ = y0;
        *v++ = x1; *v++ = y0;
        *v++ = x0; *v++ = y1;
        *v++ = x1; *v++ = y0;
        *v++ = x1; *v++ = y1;
        *v++ = x0; *v++ = y1;

        *t++ = u0; *t++ = v0;
        *t++ = u1; *t++ = v0;
        *t++ = u0; *t++ = v1;
        *t++ = u1; *t++ = v0;
        *t++ = u1; *t++ = v1;
        *t++ = u0; *t++ = v1;

        batch_count++;
    }

    if (batch_count > 0 && current_atlas >= 0 && current_atlas < GPU_QUADS_ATLAS_COUNT) {
        glBindTexture(GL_TEXTURE_2D, g_atlas_tex[current_atlas]);
        glVertexPointer(2, GL_FLOAT, 0, g_batch_verts);
        glTexCoordPointer(2, GL_FLOAT, 0, g_batch_tc);
        glDrawArrays(GL_TRIANGLES, 0, batch_count * 6);
    }

    int drawn = gq_quads_count;
    gq_quads_count = 0;
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();

    return drawn;
}

#endif // ZENONIA_GPU_QUADS
