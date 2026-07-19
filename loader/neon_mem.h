#ifndef __NEON_MEM_H__
#define __NEON_MEM_H__

#include <stddef.h>

// Reemplazos NEON de memcpy/memset (optimization_ideas.md #4), detras de
// ZENONIA_NEON_LIBC -- ver dynlib.c (tabla default_dynlib, entradas "memcpy"/
// "memset") y CMakeLists.txt (opcion NEON_LIBC_INTERCEPT). Sin medir todavia
// en consola real contra la libc estandar de vitasdk (ver
// feedback_measure_before_patch en memoria): default OFF hasta confirmar con
// un log de FPS A/B, mismo criterio que RGB565_DIRECT_UPLOAD.
void *neon_memcpy(void *dst, const void *src, size_t n);
void *neon_memset(void *dst, int val, size_t n);

#endif
