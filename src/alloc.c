/*
 * alloc.c — Bump allocator for DMA-capable physical memory
 *
 * This is the simplest possible memory allocator: a "bump" allocator
 * that maintains a single offset into a large static array. Each
 * allocation advances the offset forward. Memory is never freed.
 *
 * How it works:
 *
 *   heap[0 ............... HEAP_SIZE-1]
 *         ^                ^
 *         |                |
 *   heap_offset        next alloc goes here
 *   (after align)
 *
 * For each allocation:
 *   1. Round heap_offset up to the requested alignment
 *   2. Check there's enough space remaining
 *   3. Return a pointer to heap[offset], advance offset by size
 *   4. Zero the allocated region (xHCI requires zeroed reserved fields)
 *
 * Why a static array?
 *   - Its address is determined at link time → known at compile time
 *   - With Multiboot identity mapping, virtual address == physical address
 *   - No need for a page table walk to find the physical address
 *   - Alignment is guaranteed by __attribute__((aligned(4096)))
 *
 * The 1 MB heap is placed in the .bss section (zero-initialized),
 * so it doesn't increase the binary size on disk.
 */

#include "alloc.h"
#include "string.h"

#define HEAP_SIZE (1024 * 1024)  /* 1 MB — enough for all xHCI structures */

/* The heap array: 1 MB of memory, page-aligned (4096 bytes).
 * Placed in .bss so it's zero-initialized without bloating the binary. */
static uint8_t heap[HEAP_SIZE] __attribute__((aligned(4096)));

/* Current allocation offset — only moves forward, never back. */
static uint32_t heap_offset = 0;

void *alloc_phys(uint32_t size, uint32_t align) {
    /* Round offset up to the requested alignment.
     * The formula (offset + align - 1) & ~(align - 1) works because:
     *   - (align - 1) creates a mask of the low bits (e.g., 63 for align=64)
     *   - Adding (align - 1) ensures we overshoot if not already aligned
     *   - AND with ~(align - 1) clears the low bits, rounding down
     * Net effect: round up to the next multiple of align. */
    heap_offset = (heap_offset + align - 1) & ~(align - 1);

    /* Check for heap exhaustion */
    if (heap_offset + size > HEAP_SIZE)
        return (void *)0;  /* Out of memory */

    void *ptr = &heap[heap_offset];
    heap_offset += size;

    /* Zero the allocated region. xHCI requires reserved fields to be 0,
     * and zeroing simplifies initialization of complex structures. */
    memset(ptr, 0, size);

    return ptr;
}
