; =============================================================================
; boot_x86.asm — Multiboot entry point for ExigeOS (x86 / i386)
;
; HOW THE x86 BOOT PROCESS WORKS
; --------------------------------
; 1. Power-on: the CPU starts in 16-bit REAL MODE and jumps to the BIOS ROM
;    at physical address 0xFFFF0.
;
; 2. BIOS POST: the BIOS initialises RAM, storage devices, and basic hardware,
;    then searches the first sector (512 bytes) of each bootable device for a
;    valid boot signature (0xAA55 at offset 510).
;
; 3. Bootloader: the BIOS loads that first sector (the MBR / bootloader) at
;    physical address 0x7C00 and transfers control to it.  For ExigeOS we
;    bypass this step entirely by using a bootloader that already supports
;    the Multiboot specification (GRUB, or QEMU's built-in -kernel flag).
;
; 4. Multiboot: GRUB / QEMU reads the kernel ELF binary, loads it at the
;    address specified in the linker script (1 MB), switches the CPU into
;    32-bit protected mode (flat memory model, paging disabled), and jumps
;    to _start.  EAX contains 0x2BADB002 (proof of Multiboot compliance)
;    and EBX points to a Multiboot information structure.
;
; 5. This file: _start sets up a stack and calls our C function kernel_main().
;
; MULTIBOOT SPECIFICATION (version 1)
; ------------------------------------
; The bootloader identifies a Multiboot-compliant kernel by scanning the first
; 8 KB of the kernel image for a 4-byte-aligned magic header:
;
;   [magic]    = 0x1BADB002   ← the bootloader searches for this value
;   [flags]    = 0x00000000   ← no optional features requested here
;   [checksum] = -(magic + flags)  ← the 32-bit sum of all three must be 0
;
; The linker script guarantees .multiboot is the very first section in the
; binary, so the header is always within the first 8 KB.
; =============================================================================

MULTIBOOT_MAGIC    equ 0x1BADB002
MULTIBOOT_FLAGS    equ 0x00
MULTIBOOT_CHECKSUM equ -(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)

; ── Multiboot header ─────────────────────────────────────────────────────────
section .multiboot
align 4
    dd MULTIBOOT_MAGIC
    dd MULTIBOOT_FLAGS
    dd MULTIBOOT_CHECKSUM

; ── Stack allocation (BSS — zeroed by the ELF loader) ────────────────────────
;
; The x86 stack grows downward: pushing a value decrements ESP then writes.
; We reserve 16 KB and hand the CPU the *top* address (stack_top).
; Without a valid stack, no function call or local variable is possible.
section .bss
align 16
stack_bottom:
    resb 16384          ; 16 KB stack
stack_top:

; ── Kernel entry point ───────────────────────────────────────────────────────
section .text
global _start           ; exported so the linker can find the entry point
extern kernel_main      ; defined in kernel.c

_start:
    ; Load the stack pointer with the top of our reserved stack area.
    mov esp, stack_top

    ; The System V i386 ABI requires ESP to be 16-byte aligned *before* CALL.
    ; Push two dummy zero words to satisfy this requirement.
    push 0
    push 0

    ; Jump into the C kernel.
    call kernel_main

    ; kernel_main() should never return (the shell loop is infinite).
    ; If it does return, disable interrupts and halt the CPU.
    cli
.hang:
    hlt                 ; low-power halt (wakes on NMI)
    jmp .hang           ; loop back to halt in case of NMI wakeup

; Silence linker warning about executable stack
section .note.GNU-stack noalloc noexec nowrite progbits
