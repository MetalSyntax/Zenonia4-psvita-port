#ifndef SPRITE_DECODE_H
#define SPRITE_DECODE_H

#include <stdint.h>
#include "so_util.h"

void DrawOP_COPY_Compress_16_Vita(uint16_t *dest, const uint8_t *src, const uint16_t *pal, int stride);
void DrawOP_ENLARGE_Compress_16_Vita(uint16_t *dest, uint8_t *src, uint16_t *pal, int stride, int scale);

void zenonia_install_sprite_decode_optimize(so_module *mod);

// Fase 83 (port_progress.md): cache de sprites COPY decodificados en CPU
// (SPRITE_DECODE_CACHE) -- ver comentario extenso junto a la implementacion
// en sprite_decode.c. `src`/`w`/`h` deben venir del mismo `PutCompressImg`
// que termina invocando el COPY nativo de mas arriba (llamada sincronica
// anidada, confirmada leyendo CGxPZxZero::Blt/PutCompressImg) -- se
// consume una sola vez y se descarta.
void sprite_cache_set_pending_dims(const void *src, int w, int h);
void sprite_cache_invalidate(void);

#endif
