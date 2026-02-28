/*
 * linker_x86.ld — Linker script for ExigeOS (x86 / i386)
 *
 * WHAT IS A LINKER SCRIPT?
 * -------------------------
 * The compiler turns each .c/.asm source file into an object file (.o)
 * containing sections of code and data.  The linker combines those object
 * files into the final binary, and this script tells it:
 *   - Where in memory to place each section (the "Virtual Memory Address").
 *   - The order in which sections appear in the binary.
 *   - Which symbol marks the entry point (_start).
 *
 * WHY LOAD AT 1 MB (0x100000)?
 * ------------------------------
 * The IBM PC BIOS memory map reserves the first megabyte of physical memory:
 *   0x00000 – 0x003FF   Real-mode Interrupt Vector Table (IVT)
 *   0x00400 – 0x004FF   BIOS Data Area
 *   0x07C00 – 0x07DFF   MBR bootloader (loaded here by BIOS)
 *   0x0A000 – 0x0FFFF   VGA memory and BIOS ROM shadow
 *
 * Placing the kernel at 1 MB (0x100000) is the Multiboot standard choice:
 * it is above all BIOS-reserved areas and below the 4 GB limit of 32-bit
 * protected mode.
 *
 * SECTION OVERVIEW
 * -----------------
 *   .text   — executable code (read-only at runtime)
 *   .rodata — read-only data (string literals, const tables)
 *   .data   — initialised read-write data (global variables with values)
 *   .bss    — uninitialised / zero-initialised data (global variables
 *             without explicit values — not stored in the binary, just
 *             described by size so the loader knows how many bytes to zero)
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;     /* start virtual address at 1 MB */

    /* .multiboot must be first so the Multiboot header is in the
     * very first bytes of the binary — the bootloader scans only
     * the first 8 KB for the magic header. */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    /* .bss takes up no space in the binary file but the loader
     * (or boot stub) must zero these bytes before entering the kernel. */
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
}
