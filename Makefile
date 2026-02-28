# MicroANT — Build system
#
# Builds a flat binary kernel image for x86 (32-bit protected mode).
# The binary is Multiboot-compliant and can be loaded directly by
# QEMU's -kernel option or by any Multiboot bootloader (GRUB).
#
# Toolchain: GCC (32-bit cross-compilation), NASM (x86 assembler), LD (linker)
# On Debian/Ubuntu: sudo apt install gcc libc6-dev-i386 nasm binutils

CC      = gcc
LD      = ld
NASM    = nasm

# Compiler flags for freestanding (bare-metal) x86 code:
#   -m32                  Generate 32-bit code (Multiboot runs in protected mode)
#   -std=gnu99            Use C99 with GNU extensions (inline asm, attributes)
#   -ffreestanding        No standard library; don't assume libc exists
#   -O2                   Optimize for speed (important for busy-wait loops)
#   -Wall -Wextra         Enable all common warnings
#   -nostdlib             Don't link against libc
#   -fno-builtin          Don't replace our memset/memcpy with builtins
#   -fno-stack-protector  No stack canaries (we have no __stack_chk_fail)
#   -fno-pic              No position-independent code (fixed address at 1 MB)
#   -Isrc                 Look for headers in src/
CFLAGS  = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra \
          -nostdlib -fno-builtin -fno-stack-protector \
          -fno-pic -Isrc

# Linker flags:
#   -m elf_i386           Output 32-bit ELF format
#   -T src/linker_x86.ld  Use our linker script (places kernel at 1 MB)
#   -nostdlib              Don't link standard libraries
LDFLAGS = -m elf_i386 -T src/linker_x86.ld -nostdlib

BUILD   = build
TARGET  = microant.bin

# Object files in link order:
#   boot_x86.o must be first (contains the Multiboot header and entry point)
OBJS = $(BUILD)/boot_x86.o \
       $(BUILD)/string.o    \
       $(BUILD)/alloc.o     \
       $(BUILD)/vga.o       \
       $(BUILD)/pci.o       \
       $(BUILD)/xhci.o      \
       $(BUILD)/usb.o       \
       $(BUILD)/ant.o       \
       $(BUILD)/display.o   \
       $(BUILD)/kernel.o

.PHONY: all clean run

all: $(TARGET)

# Link all object files into the final flat binary
$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# Create build directory if it doesn't exist
$(BUILD):
	mkdir -p $(BUILD)

# Assemble the Multiboot boot stub (NASM, ELF32 format)
$(BUILD)/boot_x86.o: src/boot_x86.asm | $(BUILD)
	$(NASM) -f elf32 -o $@ $<

# Compile C source files
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# Run in QEMU with an emulated xHCI controller (no real USB device).
# Useful for testing boot sequence, PCI scan, and xHCI initialization.
# Will stop at "No ANT+ stick found" since no physical device is attached.
run: $(TARGET)
	qemu-system-i386 -kernel $(TARGET) \
	    -device qemu-xhci,id=xhci

# Run with USB passthrough: passes a real Garmin ANT+ stick to the VM.
# Requires root (sudo) for raw USB device access.
# The vendorid/productid match the Garmin USB-m ANT Stick (0x0FCF:0x1008).
run-passthrough: $(TARGET)
	sudo qemu-system-i386 -kernel $(TARGET) \
	    -device qemu-xhci,id=xhci \
	    -device usb-host,vendorid=0x0fcf,productid=0x1008

clean:
	rm -rf $(BUILD) $(TARGET)
