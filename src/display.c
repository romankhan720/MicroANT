/*
 * display.c — BPM display with large digits on VGA text mode
 *
 * This module renders the heart rate on a standard VGA 80x25 text
 * mode display. VGA text mode stores characters in a memory-mapped
 * buffer at physical address 0xB8000:
 *
 *   Each cell is 2 bytes: [character byte] [attribute byte]
 *   Attribute format: [7:4] = background color, [3:0] = foreground color
 *   Screen size: 80 columns × 25 rows = 2000 cells = 4000 bytes
 *
 * The display has three zones:
 *
 *   1. Title bar (row 0): "MicroANT v0.1" on a blue background
 *
 *   2. Heart rate area (rows 8-14): Shows a heart icon (♥) followed
 *      by the BPM value rendered in large 5×5 block characters, with
 *      a vertical "BPM" label beside it.
 *
 *   3. Status line (row 24): Shows the current system state in green
 *      text (e.g., "Initializing...", "Receiving heart rate data").
 *
 * Large digits are drawn using a simple 5-wide × 5-tall ASCII art font
 * where '#' characters form the digit shape. Each digit occupies a
 * 5×5 grid with 1 column of spacing between digits.
 */

#include "display.h"
#include "vga.h"

/* ── Direct VGA memory access for positioned drawing ───────────────── *
 *
 * While vga.c provides a sequential text output API (vga_putchar,
 * vga_print), we need to place characters at arbitrary screen positions
 * for the large digit display. We access VGA memory directly.
 */

/* VGA text mode framebuffer at physical address 0xB8000 */
static volatile uint16_t *const VMEM = (volatile uint16_t *)0xB8000;

/* Create a VGA character+attribute entry.
 * Low byte = ASCII character, high byte = color attribute. */
static uint16_t make_entry(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/* Place a single character at a specific row/column with color.
 * Bounds-checked to prevent writing outside the 80×25 screen. */
static void put_at(int row, int col, char c, uint8_t color) {
    if (row >= 0 && row < VGA_HEIGHT && col >= 0 && col < VGA_WIDTH)
        VMEM[row * VGA_WIDTH + col] = make_entry(c, color);
}

/* Print a string starting at a specific row/column. */
static void print_at(int row, int col, const char *str, uint8_t color) {
    while (*str && col < VGA_WIDTH) {
        put_at(row, col, *str, color);
        str++;
        col++;
    }
}

/* Fill an entire row with spaces in the given color (clears the row). */
static void clear_row(int row, uint8_t color) {
    for (int col = 0; col < VGA_WIDTH; col++)
        VMEM[row * VGA_WIDTH + col] = make_entry(' ', color);
}

/* ── 5-row tall digit font (5 wide × 5 tall) ──────────────────────── *
 *
 * Each digit 0-9 is defined as 5 strings of 5 characters each.
 * '#' characters form the visible shape, spaces are background.
 * This gives large, readable digits visible from a distance. */

static const char *DIGITS[10][5] = {
    /* 0 */
    { " ### ",
      "#   #",
      "#   #",
      "#   #",
      " ### " },
    /* 1 */
    { "  #  ",
      " ##  ",
      "  #  ",
      "  #  ",
      " ### " },
    /* 2 */
    { " ### ",
      "#   #",
      "  ## ",
      " #   ",
      "#####" },
    /* 3 */
    { " ### ",
      "#   #",
      "  ## ",
      "#   #",
      " ### " },
    /* 4 */
    { "#   #",
      "#   #",
      "#####",
      "    #",
      "    #" },
    /* 5 */
    { "#####",
      "#    ",
      " ### ",
      "    #",
      " ### " },
    /* 6 */
    { " ### ",
      "#    ",
      "#### ",
      "#   #",
      " ### " },
    /* 7 */
    { "#####",
      "    #",
      "   # ",
      "  #  ",
      "  #  " },
    /* 8 */
    { " ### ",
      "#   #",
      " ### ",
      "#   #",
      " ### " },
    /* 9 */
    { " ### ",
      "#   #",
      " ####",
      "    #",
      " ### " },
};

/* Heart symbol (5 lines × 7 columns).
 * Rendered using ASCII character 3 (♥ in the PC character set). */
static const char *HEART[5] = {
    " ## ## ",
    "#######",
    "#######",
    " ##### ",
    "  ###  ",
};

/* Draw a large digit (5×5) at the given screen position. */
static void draw_big_digit(int digit, int row, int col, uint8_t color) {
    if (digit < 0 || digit > 9) return;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 5; c++)
            if (DIGITS[digit][r][c] == '#')
                put_at(row + r, col + c, '#', color);
            else
                put_at(row + r, col + c, ' ', 0x00);
}

/* Draw the heart symbol at the given screen position.
 * Uses ASCII character 3 (♥) for a filled heart appearance. */
static void draw_heart(int row, int col, uint8_t color) {
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 7; c++)
            if (HEART[r][c] == '#')
                put_at(row + r, col + c, 3, color);  /* ASCII 3 = ♥ */
            else
                put_at(row + r, col + c, ' ', 0x00);
}

/* ── Public API ────────────────────────────────────────────────────── */

/*
 * Initialize the display: clear screen, draw the title bar, and show
 * a placeholder in the heart rate area.
 *
 * Layout after init:
 *   Row 0:  [=========  MicroANT v0.1  =========] (blue background)
 *   Row 10: "--- BPM" (dim grey placeholder)
 *   Row 24: "> Initializing..." (green status)
 */
void display_init(void) {
    vga_init();
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_clear();

    /* Title bar: white text on blue background */
    uint8_t title_color = (VGA_COLOR_BLUE << 4) | VGA_COLOR_WHITE;
    clear_row(0, title_color);
    print_at(0, 30, "  MicroANT v0.1  ", title_color);

    /* Heart rate area placeholder (shown before first data arrives) */
    uint8_t dim = (VGA_COLOR_BLACK << 4) | VGA_COLOR_DARK_GREY;
    print_at(10, 30, "--- BPM", dim);

    /* Status bar */
    display_status("Initializing...");
}

/*
 * Update the heart rate display with the current BPM value.
 *
 * Draws three elements centered on the screen:
 *   1. Heart icon (♥) in red, at column 22
 *   2. BPM digits in large 5×5 font, starting at column 32
 *   3. Vertical "B P M" label next to the last digit
 *
 * The digit rendering handles 1, 2, or 3-digit numbers:
 *   - Single digit (0-9): just the ones digit
 *   - Two digits (10-99): tens + ones
 *   - Three digits (100-255): hundreds + tens + ones
 */
void display_bpm(uint8_t bpm) {
    int base_row = 8;

    /* Color scheme */
    uint8_t heart_color = (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_RED;
    uint8_t digit_color = (VGA_COLOR_BLACK << 4) | VGA_COLOR_WHITE;
    uint8_t bpm_color   = (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_GREY;

    /* Clear the display area (rows 8-14) */
    for (int r = base_row; r < base_row + 7; r++)
        clear_row(r, 0x00);

    /* Draw the heart icon (♥) on the left */
    draw_heart(base_row + 1, 22, heart_color);

    /* Decompose BPM into individual digits */
    int hundreds = bpm / 100;
    int tens     = (bpm / 10) % 10;
    int ones     = bpm % 10;

    /* Draw each digit, advancing 6 columns between them (5 for digit + 1 gap) */
    int col = 32;

    if (hundreds > 0) {
        draw_big_digit(hundreds, base_row + 1, col, digit_color);
        col += 6;
    }
    if (hundreds > 0 || tens > 0) {
        draw_big_digit(tens, base_row + 1, col, digit_color);
        col += 6;
    }
    draw_big_digit(ones, base_row + 1, col, digit_color);
    col += 6;

    /* "BPM" label displayed vertically next to the digits */
    print_at(base_row + 2, col + 1, "B", bpm_color);
    print_at(base_row + 3, col + 1, "P", bpm_color);
    print_at(base_row + 4, col + 1, "M", bpm_color);
}

/*
 * Update the status line at the bottom of the screen.
 * Shows a green "> " prompt followed by the status message.
 * Used to indicate the current boot stage or runtime state.
 */
void display_status(const char *msg) {
    uint8_t status_color = (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_GREEN;
    clear_row(24, 0x00);
    print_at(24, 1, "> ", status_color);
    print_at(24, 3, msg, status_color);
}
