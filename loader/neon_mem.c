/*
 * neon_mem.c -- reemplazos NEON de memcpy/memset para el hot path de blit por
 * software del motor (optimization_ideas.md #4). Ver neon_mem.h para el
 * contrato y el estado de medicion (todavia sin confirmar en consola real).
 *
 * No manejan solapamiento (eso es memmove, que no se toca aca) ni
 * comportamiento de errno -- mismo contrato que memcpy/memset estandar.
 */

#include "neon_mem.h"

#if defined(__ARM_NEON__) || defined(__ARM_NEON)
#include <arm_neon.h>

void *neon_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *) dst;
    const uint8_t *s = (const uint8_t *) src;

    while (n >= 64) {
        uint8x16_t v0 = vld1q_u8(s);
        uint8x16_t v1 = vld1q_u8(s + 16);
        uint8x16_t v2 = vld1q_u8(s + 32);
        uint8x16_t v3 = vld1q_u8(s + 48);
        vst1q_u8(d, v0);
        vst1q_u8(d + 16, v1);
        vst1q_u8(d + 32, v2);
        vst1q_u8(d + 48, v3);
        s += 64;
        d += 64;
        n -= 64;
    }
    while (n >= 16) {
        vst1q_u8(d, vld1q_u8(s));
        s += 16;
        d += 16;
        n -= 16;
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *neon_memset(void *dst, int val, size_t n) {
    uint8_t *d = (uint8_t *) dst;
    uint8x16_t v = vdupq_n_u8((uint8_t) val);

    while (n >= 64) {
        vst1q_u8(d, v);
        vst1q_u8(d + 16, v);
        vst1q_u8(d + 32, v);
        vst1q_u8(d + 48, v);
        d += 64;
        n -= 64;
    }
    while (n >= 16) {
        vst1q_u8(d, v);
        d += 16;
        n -= 16;
    }
    while (n--) {
        *d++ = (uint8_t) val;
    }
    return dst;
}

#else
// Fallback escalar -- solo por si este archivo se compila fuera del target
// ARM del Vita (p.ej. un harness de test en host); el build real de Vita
// siempre entra por la rama NEON de arriba.
#include <string.h>

void *neon_memcpy(void *dst, const void *src, size_t n) {
    return memcpy(dst, src, n);
}

void *neon_memset(void *dst, int val, size_t n) {
    return memset(dst, val, n);
}

#endif
