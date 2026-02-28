/*
 * string.h — Minimal C library memory functions
 *
 * In a freestanding (bare-metal) environment, there is no standard C
 * library. We must provide our own implementations of the basic memory
 * manipulation functions that GCC may generate calls to.
 *
 * These three functions cover all the memory operations needed by
 * the xHCI driver, USB stack, and ANT+ protocol:
 *
 *   memset  — Zero-initialize structures (xHCI contexts, buffers)
 *   memcpy  — Copy data between USB buffers and protocol structures
 *   memcmp  — Compare memory regions (descriptor matching, etc.)
 *
 * Note: GCC with -ffreestanding may still emit implicit calls to
 * memset/memcpy for structure assignments and initialization,
 * so these must be available even if not called explicitly.
 */

#ifndef STRING_H
#define STRING_H

#include <stdint.h>

/* Fill n bytes of memory at dst with the value val (cast to uint8_t). */
void *memset(void *dst, int val, uint32_t n);

/* Copy n bytes from src to dst. Regions must not overlap. */
void *memcpy(void *dst, const void *src, uint32_t n);

/* Compare n bytes of memory. Returns 0 if equal, <0 or >0 otherwise. */
int   memcmp(const void *a, const void *b, uint32_t n);

#endif
