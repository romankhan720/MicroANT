/*
 * vga.c — VGA text-mode driver (x86)
 *
 * HOW VGA TEXT MODE WORKS
 * ------------------------
 * VGA text mode 3 (80×25) is the default video mode set by the BIOS.
 * The video card exposes a 4000-byte memory-mapped buffer at physical
 * address 0xB8000.  Each character cell occupies two consecutive bytes:
 *
 *   cell[n * 2 + 0] = ASCII character code
 *   cell[n * 2 + 1] = attribute byte  (colours / blink)
 *
 * We access this as a uint16_t array where each entry is:
 *   entry = (attribute << 8) | character
 *
 * Cell index for row r, column c:
 *   index = r * VGA_WIDTH + c   (with VGA_WIDTH = 80)
 *
 * HARDWARE CURSOR
 * ----------------
 * The VGA card maintains a blinking cursor independently of the video
 * memory.  Its position is controlled via two I/O port registers:
 *
 *   Port 0x3D4 (CRT Controller Index): write the register index first.
 *   Port 0x3D5 (CRT Controller Data):  then write the value.
 *
 *   Register 0x0E = cursor position, high byte
 *   Register 0x0F = cursor position, low byte
 *
 * The cursor position is a linear index (row * 80 + col), split across
 * two 8-bit registers because the original hardware was 8-bit wide.
 *
 * SCROLLING
 * ----------
 * When the cursor reaches the last row (row 24), we scroll the screen
 * up by one line: copy rows 1–24 to rows 0–23, then clear row 24.
 */

#include "vga.h"
#include "io.h"
#include <stdint.h>

/* Pointer to VGA video memory. volatile prevents the compiler from
 * caching reads or eliminating writes — the hardware reads this memory
 * asynchronously to refresh the display. */
static volatile uint16_t *const VGA_MEM = (volatile uint16_t *)0xB8000;

static int     cursor_row   = 0;
static int     cursor_col   = 0;
static uint8_t current_color = 0x07;   /* light grey (7) on black (0) */

/* Pack a character and attribute byte into one VGA cell entry. */
static uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/*
 * vga_update_cursor() — Write the logical cursor position to the
 * VGA hardware cursor registers so the blinking cursor matches the
 * software position.  Must be called after every cursor movement.
 */
static void vga_update_cursor(void) {
    uint16_t pos = (uint16_t)(cursor_row * VGA_WIDTH + cursor_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));  /* low byte  */
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));    /* high byte */
}

/* vga_set_color() — Set the attribute byte for subsequent output.
 * Attribute = (bg & 0x07) << 4 | (fg & 0x0F) */
void vga_set_color(uint8_t fg, uint8_t bg) {
    current_color = (bg << 4) | (fg & 0x0F);
}

void vga_init(void) {
    current_color = 0x07;
    vga_clear();
}

/* vga_clear() — Fill all 2000 cells with a space and reset the cursor. */
void vga_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEM[i] = vga_entry(' ', current_color);
    cursor_row = 0;
    cursor_col = 0;
    vga_update_cursor();
}

/* vga_scroll() — Shift every row up by one and blank the last row. */
static void vga_scroll(void) {
    for (int row = 0; row < VGA_HEIGHT - 1; row++)
        for (int col = 0; col < VGA_WIDTH; col++)
            VGA_MEM[row * VGA_WIDTH + col] = VGA_MEM[(row + 1) * VGA_WIDTH + col];
    for (int col = 0; col < VGA_WIDTH; col++)
        VGA_MEM[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = vga_entry(' ', current_color);
    cursor_row = VGA_HEIGHT - 1;
}

void vga_putchar(char c) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            VGA_MEM[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(' ', current_color);
        }
    } else {
        VGA_MEM[cursor_row * VGA_WIDTH + cursor_col] = vga_entry(c, current_color);
        cursor_col++;
        if (cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    if (cursor_row >= VGA_HEIGHT)
        vga_scroll();
    vga_update_cursor();    /* always sync hardware cursor */
}

void vga_print(const char *str) {
    while (*str) vga_putchar(*str++);
}

void vga_newline(void) {
    vga_putchar('\n');
}

void vga_print_int(uint32_t n) {
    if (n == 0) { vga_putchar('0'); return; }
    char buf[12];
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i - 1; j >= 0; j--)
        vga_putchar(buf[j]);
}

void vga_print_int2(uint8_t n) {
    vga_putchar('0' + (n / 10));
    vga_putchar('0' + (n % 10));
}

/*
 * vga_flash() — Visual bell.
 * Swap foreground and background colours of every cell, pause, then
 * swap back.  The effect is a full-screen colour inversion flash.
 */
void vga_flash(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        uint16_t entry = VGA_MEM[i];
        uint8_t attr = (uint8_t)(entry >> 8);
        uint8_t inv  = (uint8_t)(((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F));
        VGA_MEM[i] = (entry & 0x00FF) | ((uint16_t)inv << 8);
    }
    for (volatile int i = 0; i < 5000000; i++);
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        uint16_t entry = VGA_MEM[i];
        uint8_t attr = (uint8_t)(entry >> 8);
        uint8_t inv  = (uint8_t)(((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F));
        VGA_MEM[i] = (entry & 0x00FF) | ((uint16_t)inv << 8);
    }
}
