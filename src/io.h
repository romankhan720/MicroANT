/*
 * io.h — x86 port-mapped I/O primitives
 *
 * The x86 architecture has two separate address spaces:
 *   1. Memory space  — accessed with normal load/store instructions.
 *   2. I/O port space — a 64 KB address space (ports 0x0000–0xFFFF)
 *      accessed with the special IN and OUT instructions.
 *
 * Most legacy PC hardware (keyboard controller, timer, RTC, VGA
 * cursor, PC speaker …) lives in I/O port space rather than in
 * regular memory. These three inline functions are the only
 * abstraction you need to talk to all of that hardware.
 *
 * NOTE: This header is NOT included on Raspberry Pi (PLATFORM_RPI3).
 * ARM uses memory-mapped I/O exclusively — all peripherals are
 * reached via normal pointer dereferences to physical addresses.
 */

#ifndef IO_H
#define IO_H

#include <stdint.h>

/*
 * inb() — Read one byte from an I/O port.
 *
 * The "inb %1, %0" instruction reads the byte at I/O address
 * `port` into the AL register (constraint "=a").
 * "Nd" allows the port to be encoded as an 8-bit immediate when
 * it is a compile-time constant (< 256), or placed in DX otherwise.
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/*
 * outb() — Write one byte to an I/O port.
 *
 * "outb %0, %1" writes AL (constraint "a") to the port in DX / imm8.
 * The "volatile" keyword prevents the compiler from reordering or
 * eliminating the instruction — hardware register writes must
 * happen exactly as written.
 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * io_wait() — Insert a small delay after an I/O write.
 *
 * Some old ISA devices (8259 PIC, 8042 PS/2 controller …) need a
 * brief pause between consecutive port accesses to process the
 * previous command.  Writing any value to port 0x80 (POST diagnostic
 * port — unused by modern hardware) takes ≈ 1–4 µs on a real PC,
 * which is enough of a delay for any legacy device.
 */
/* inw() / outw() — 16-bit I/O port access. */
static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* inl() / outl() — 32-bit I/O port access (used for PCI config). */
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif
