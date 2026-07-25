// Variante 2 (--gpu-quads-test), rediseno Fase 50: offload de TODOS los
// sprites de PutCompressImg a quads texturizados en la GPU via vitaGL.
//
// Arquitectura (Fase 50, reemplaza el split "FB base encima con color-key" de
// la Fase 49, que era estructuralmente incapaz de ordenar sombras/efectos
// software respecto de los quads y ademas obligaba a convertir el FB por CPU
// cada frame -- la regresion de FPS del log_1784517442.txt):
//
//   1. El FB base del motor se sube DIRECTO en RGB565 (rapido, sin conversion)
//      y se dibuja OPACO DEBAJO, igual que en los builds RGB565_DIRECT. Con
//      todos los ops desviados, el FB solo contiene primitivas CGsGraphics
//      (fondos/fills, ~0 por frame en gameplay) -- la capa de fondo legitima.
//   2. TODOS los enumDrawOP de PutCompressImg (no solo COPY) se desvian a
//      quads, dibujados ENCIMA del FB en el ORDEN DE LLAMADA original -- el
//      z-order del motor se preserva entre sprites.
//   3. La transparencia/blending por pixel se recupera con DECODE DUAL: cada
//      sprite se decodifica DOS veces con el blitter REAL del motor (mismo op)
//      sobre un scratch negro (d0) y uno blanco (d1). Cualquier operador
//      afin en el dest (result = c + (1-a)*dest: copy, skip, blend, multiply/
//      sombra, colorize) queda EXACTAMENTE caracterizado: c = d0,
//      a = 255 - (d1 - d0). La textura se guarda PREMULTIPLICADA (rgb = c,
//      alpha = a) y se dibuja con glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA),
//      que reproduce el operador sin divisiones ni color-keys (el color-key
//      0xF81F/0x0001 y sus falsos positivos desaparecen por completo).
//      op==2 (ADDITIVE) es el unico no-afin por el clamp: una sola pasada
//      sobre negro con alpha=0 -- el mismo blend func da src + dest exacto.
//   4. El p8 (alpha global / color de colorize segun el op) se pasa TAL CUAL
//      al blitter real, que lo hornea en el resultado -- por eso (op,p8) son
//      parte de la clave de cache junto con (data,pal).
#ifndef GPU_QUADS_H
#define GPU_QUADS_H

#include <stdint.h>

// Ancho/alto del scratch = dims del main screen buffer (el motor decodifica
// con el stride del main screen, no con uno propio).
#define GPU_QUADS_SCRATCH_W 960
#define GPU_QUADS_SCRATCH_H 544

// Guarda las dims logicas de pantalla usadas para la proyeccion de los quads.
// Llamar una vez.
void gpu_quads_init(int screen_w, int screen_h);

// Cache-hit: si el sprite (data,pal,op,p8) ya tiene textura, encola el quad en
// (x,y) y devuelve 1 (el caller debe SALTAR el blit por software). Si no, 0.
int gpu_quads_lookup_and_enqueue(const void *data, const void *pal, int op, int p8, int x, int y, int w, int h);

// Prepara el scratch para una pasada de decode de un sprite w x h:
// llena esa region con `fill` (0x0000 pasada negra, 0xFFFF pasada blanca) y
// devuelve el puntero al struct de framebuffer alternativo (parametro 9 de
// PutCompressImg). NULL si (w,h) se sale del scratch -> blit por software.
void *gpu_quads_begin_pass(int w, int h, uint16_t fill);

// Copia el resultado de la pasada negra (d0) a un buffer aparte, antes de
// re-llenar el scratch a blanco y correr la segunda pasada.
void gpu_quads_capture_pass1(int w, int h);

// Combina la(s) pasada(s), sube la textura RGBA premultiplicada, la cachea por
// (data,pal,op,p8) y encola el quad en (screen_x,screen_y). dual=1 usa pass1(negro) +
// scratch(blanco); dual=0 (op additive) usa solo scratch(negro) con alpha=0.
void gpu_quads_finish_decode(const void *data, const void *pal, int op, int p8,
                             int screen_x, int screen_y, int w, int h, int dual);

// Dibuja todos los quads encolados este frame ENCIMA del FB base (se llama en
// glDrawArrays_wrapper JUSTO DESPUES del glDrawArrays real del motor) y vacia
// la cola. Devuelve cuantos quads dibujo; si devuelve >0 el caller DEBE
// restaurar el estado GL del motor (gpu_quads_restore_engine_state) porque el
// flush lo pisa.
int gpu_quads_flush(void);

// Fase 59 (2026-07-20): encola un glifo de texto (CGxFACharCache::
// DrawToFrameBuffer, ver dynlib.c) en la MISMA cola que los sprites de
// PutCompressImg -- confirmado en out_ghidra.c que el texto NUNCA pasa por
// PutCompressImg (escribe directo al framebuffer de software via su propio
// loop de blend), asi que sin esto su orden relativo contra los sprites
// desviados a GPU se pierde: el framebuffer de software siempre se compone
// COMPLETO como capa base, DEBAJO de TODOS los quads del frame (ver
// gpu_quads_flush), sin importar que el texto se haya escrito ahi DESPUES
// (en tiempo real de llamada) que un sprite/panel que deberia quedar detras
// suyo -- el panel (ahora un quad) termina tapando el texto (quedo en el FB)
// sistematicamente. Encolar el texto como quad tambien resuelve esto: su
// posicion en la cola pasa a reflejar el momento real en que el motor lo
// dibujo, igual que cualquier sprite.
// `alpha` es un buffer de cobertura 0-255 (el char-cache SOLO guarda alfa,
// confirmado en CopyPixelsToCharCacheBuffer/out_ghidra.c), `aw` es su stride
// real en bytes (el ancho ORIGINAL del glifo, no necesariamente igual a `w`
// si el caller ya recorto contra el clip rect de GxFont_GetCommonInfo antes
// de llamar). (r,g,b) es el color de texto actual (CGxFACharCache::
// setColor/setColor565, offset this+0x48).
void gpu_quads_enqueue_text(const unsigned char *alpha, int aw,
                             unsigned char r, unsigned char g, unsigned char b,
                             int screen_x, int screen_y, int w, int h);

// Fase 61 (2026-07-20): encola un relleno solido (CGsGraphics::DrawFillRect/
// DrawRoundRect/DrawFillEllipse/etc, via Screen_RGBBLEND16_Real -- ver
// dynlib.c) en la MISMA cola que sprites y texto -- mismo problema que la
// Fase 59 (escribe directo al framebuffer de software, siempre debajo de
// todos los quads) y mismo fix. (r,g,b,a) es el color+alpha YA resueltos por
// el caller (a 0-255, no el alpha de 4 bits que usa RGBBlend16 internamente).
void gpu_quads_enqueue_fill(unsigned char r, unsigned char g, unsigned char b, unsigned char a,
                             int screen_x, int screen_y, int w, int h);

// Fase 69 (2026-07-23): borra TODAS las entradas de la cache de texturas
// (glDeleteTextures + reset de metadata) y el atajo MRU de
// gpu_quads_enqueue_fill. Llamar en una transicion de stage (ver
// Zenonia_OnUIStatusChange en java.c, ui_status==19): la cache indexa por
// puntero de datos + 4 bytes muestreados (gq_hash, Fase 55/56), y el motor
// reutiliza direcciones de heap recien liberadas para los assets del stage
// nuevo -- si el contenido nuevo coincide por casualidad en esos 4 bytes con
// el viejo, gq_find serviría la textura del stage ANTERIOR para sprites
// completamente distintos. Invalidar en cada transicion elimina esa ventana
// de colision al costo de un re-decode (barato, ya amortizado en 1-2 frames)
// de los primeros sprites del stage nuevo.
void gpu_quads_invalidate_cache(void);

// Fase 77 (2026-07-24): total ACUMULADO de texturas creadas via glGenTextures
// desde el boot (nunca se resetea, ni siquiera en gpu_quads_invalidate_cache).
// Expuesto para que main.c lo incluya en su linea periodica de estadisticas
// (cada 120 frames) -- si el crash determinista de las Fases 72-76 (mismo
// `.psp2dmp` 5 veces, sobrevivio a batching y a re-bind de textura) resulta
// correlacionar con un valor sospechoso (ej. cerca de 65536, un limite tipico
// de handle de 16 bits), seria evidencia real de agotamiento de un recurso
// interno de vitaGL en vez de otra hipotesis sin confirmar.
unsigned long gpu_quads_get_total_textures_created(void);

#endif // GPU_QUADS_H
