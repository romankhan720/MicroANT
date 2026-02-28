/*
 * alloc.h — Simple bump allocator for physically contiguous memory
 *
 * The xHCI controller accesses all its data structures (TRBs, device
 * contexts, data buffers) via DMA — it reads physical memory addresses
 * directly. This means we need to allocate memory where:
 *
 *   1. The physical address is known (for writing into TRB pointers)
 *   2. The memory is aligned (xHCI requires 64-byte alignment for most
 *      structures, and some need page alignment)
 *   3. The memory is zeroed (xHCI expects reserved fields to be 0)
 *
 * Since we boot with Multiboot in 32-bit protected mode with identity
 * mapping (virtual address == physical address), a simple bump allocator
 * satisfies all three requirements: we allocate from a static array
 * whose address is known at link time.
 *
 * The allocator never frees memory — it only moves forward. This is
 * fine for our single-purpose system that allocates at startup and
 * then runs forever.
 */

#ifndef ALLOC_H
#define ALLOC_H

#include <stdint.h>

/*
 * Allocate a block of physically contiguous, zeroed memory.
 *
 * Parameters:
 *   size  — number of bytes to allocate
 *   align — required alignment (must be a power of 2)
 *           Common values: 16 (TRBs), 64 (contexts, buffers), 4096 (page)
 *
 * Returns a pointer to the allocated block, or NULL if the heap is full.
 * The returned virtual address equals the physical address (identity mapping).
 */
void *alloc_phys(uint32_t size, uint32_t align);

#endif
