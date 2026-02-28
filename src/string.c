/*
 * string.c — Minimal C library memory functions (bare-metal)
 *
 * These are byte-at-a-time implementations, simple but correct.
 * For a bare-metal system running on modern hardware with GCC -O2,
 * the compiler may auto-vectorize some of these loops, but the
 * primary goal is correctness, not speed.
 *
 * These functions follow the standard C library signatures so that
 * GCC can use them for implicit operations (struct copies, array
 * initialization) even with -fno-builtin.
 */

#include "string.h"

/*
 * memset — Fill a block of memory with a byte value.
 *
 * Used throughout the project to zero-initialize xHCI structures,
 * clear buffers, and reset state. The val parameter is an int
 * (per C standard) but only the lowest byte is used.
 */
void *memset(void *dst, int val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
    return dst;
}

/*
 * memcpy — Copy a block of memory from src to dst.
 *
 * Used for copying USB data between DMA buffers and caller buffers,
 * copying the ANT+ network key into message payloads, and transferring
 * descriptor data. Source and destination must not overlap.
 */
void *memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/*
 * memcmp — Compare two memory regions byte by byte.
 *
 * Returns 0 if all n bytes are identical, otherwise returns the
 * difference of the first differing byte (negative if a < b,
 * positive if a > b). Useful for comparing descriptors or data.
 */
int memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}
