#ifndef SPRITE_DECODE_H
#define SPRITE_DECODE_H

#include <stdint.h>
#include "so_util.h"

void DrawOP_COPY_Compress_16_Vita(uint16_t *dest, uint8_t *src, uint16_t *pal, int stride);
void DrawOP_ENLARGE_Compress_16_Vita(uint16_t *dest, uint8_t *src, uint16_t *pal, int stride, int scale);

void zenonia_install_sprite_decode_optimize(so_module *mod);

#endif
