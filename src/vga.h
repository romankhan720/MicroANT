/*
 * vga.h — VGA text-mode driver interface
 *
 * VGA text mode 3 provides an 80×25 grid of character cells.
 * Each cell is two bytes stored in memory starting at 0xB8000:
 *
 *   Byte 0 (low):  ASCII character code
 *   Byte 1 (high): attribute byte
 *
 * Attribute byte layout:
 *   bit  7   : blink (or bright background, depending on BIOS setting)
 *   bits 6–4 : background colour (3 bits → 8 choices)
 *   bits 3–0 : foreground colour (4 bits → 16 choices)
 *
 * Example: attribute 0x07 = light grey (7) on black (0) = default.
 *          attribute 0x1F = white (15) on blue (1).
 *
 * On Raspberry Pi 3 this header is still included, but the
 * implementation (vga_rpi3.c) sends ANSI escape sequences over
 * UART instead of writing to VGA memory.  The interface is
 * identical so shell.c compiles unchanged on both platforms.
 */

#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#define VGA_WIDTH  80   /* columns */
#define VGA_HEIGHT 25   /* rows    */

/*
 * VGA colour palette (4-bit index used in the attribute byte).
 * Only colours 0–7 are available as background colours because
 * the background field is only 3 bits wide.
 */
typedef enum {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN   = 14,   /* often rendered as yellow */
    VGA_COLOR_WHITE         = 15,
} vga_color_t;

/* vga_init()    — Clear screen, reset cursor and colour. */
void vga_init(void);

/* vga_clear()   — Fill the screen with spaces in the current colour. */
void vga_clear(void);

/* vga_putchar() — Write one character at the cursor and advance it.
 *   '\n' moves to the next line (with scroll if needed).
 *   '\b' moves the cursor one position left and erases the cell. */
void vga_putchar(char c);

/* vga_print()   — Write a null-terminated string. */
void vga_print(const char *str);

/* vga_newline() — Shorthand for vga_putchar('\n'). */
void vga_newline(void);

/* vga_print_int()  — Print an unsigned 32-bit integer in decimal. */
void vga_print_int(uint32_t n);

/* vga_print_int2() — Print a uint8 as exactly two decimal digits,
 *                    zero-padded (e.g. 7 → "07", 23 → "23"). */
void vga_print_int2(uint8_t n);

/* vga_set_color() — Change the foreground/background colour for
 *                   all subsequent output.
 *   fg, bg: colour index (bg only uses 0–7). */
void vga_set_color(uint8_t fg, uint8_t bg);

/* vga_flash() — Visual bell: briefly invert the entire screen. */
void vga_flash(void);

#endif
