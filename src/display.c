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

/* ── Direct VGA memory access for positioned drawing ───────────────── */

static volatile uint16_t *const VMEM = (volatile uint16_t *)0xB8000;

#define BPM_AREA_TOP       8
#define BPM_DRAW_ROW       (BPM_AREA_TOP + 1)
#define GRAPH_PANEL_TOP    16
#define GRAPH_PANEL_BOTTOM 22
#define GRAPH_PANEL_LEFT   22
#define GRAPH_PANEL_RIGHT  57
#define GRAPH_LABEL_ROW    GRAPH_PANEL_TOP
#define GRAPH_INNER_TOP    (GRAPH_PANEL_TOP + 1)
#define GRAPH_INNER_BOTTOM (GRAPH_PANEL_BOTTOM - 1)
#define GRAPH_HEIGHT       (GRAPH_INNER_BOTTOM - GRAPH_INNER_TOP + 1)
#define GRAPH_LEFT         25

/* ── NEW: BPM history buffer (no dynamic allocation, fixed size) ───── */

#define HISTORY_SIZE 30
static uint8_t history[HISTORY_SIZE];
static int hist_idx = 0;

/* Create a VGA character+attribute entry. */
static uint16_t make_entry(char c, uint8_t color) {
    return (uint16_t)(uint8_t)c | ((uint16_t)color << 8);
}

/* Place a character at row/column. */
static void put_at(int row, int col, char c, uint8_t color) {
    if (row >= 0 && row < VGA_HEIGHT && col >= 0 && col < VGA_WIDTH)
        VMEM[row * VGA_WIDTH + col] = make_entry(c, color);
}

/* Print string at position. */
static void print_at(int row, int col, const char *str, uint8_t color) {
    while (*str && col < VGA_WIDTH) {
        put_at(row, col, *str, color);
        str++;
        col++;
    }
}

/* Clear row */
static void clear_row(int row, uint8_t color) {
    for (int col = 0; col < VGA_WIDTH; col++)
        VMEM[row * VGA_WIDTH + col] = make_entry(' ', color);
}

/* ── Digit font ───────────────── */

static const char *DIGITS[10][5] = {
    { " ### ", "#   #", "#   #", "#   #", " ### " },
    { "  #  ", " ##  ", "  #  ", "  #  ", " ### " },
    { " ### ", "#   #", "  ## ", " #   ", "#####" },
    { " ### ", "#   #", "  ## ", "#   #", " ### " },
    { "#   #", "#   #", "#####", "    #", "    #" },
    { "#####", "#    ", " ### ", "    #", " ### " },
    { " ### ", "#    ", "#### ", "#   #", " ### " },
    { "#####", "    #", "   # ", "  #  ", "  #  " },
    { " ### ", "#   #", " ### ", "#   #", " ### " },
    { " ### ", "#   #", " ####", "    #", " ### " },
};

static const char *HEART[5] = {
    " ## ## ",
    "#######",
    "#######",
    " ##### ",
    "  ###  ",
};

/* ── NEW: Color selection based on BPM ───────────────── */

static uint8_t get_bpm_color(uint8_t bpm) {
    if (bpm < 90)
        return (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_GREEN;
    else if (bpm < 120)
        return (VGA_COLOR_BLACK << 4) | VGA_COLOR_BROWN;
    else
        return (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_RED;
}

/* ── NEW: Small integer to string (no libc) ─────────── */

static void u8_to_str(uint8_t val, char *buf) {
    buf[0] = '0' + (val / 100);
    buf[1] = '0' + ((val / 10) % 10);
    buf[2] = '0' + (val % 10);
    buf[3] = '\0';
}

static uint8_t get_history_color(uint8_t bpm, int is_latest) {
    if (is_latest)
        return (VGA_COLOR_BLACK << 4) | VGA_COLOR_WHITE;
    if (bpm < 90)
        return (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_CYAN;
    if (bpm < 120)
        return (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_GREEN;
    return (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_RED;
}

/* Draw a large digit */
static void draw_big_digit(int digit, int row, int col, uint8_t color) {
    if (digit < 0 || digit > 9) return;

    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 5; c++)
            if (DIGITS[digit][r][c] == '#')
                put_at(row + r, col + c, '#', color);
            else
                put_at(row + r, col + c, ' ', 0x00);
}

/* Draw heart */
static void draw_heart(int row, int col, uint8_t color) {
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 7; c++)
            if (HEART[r][c] == '#')
                put_at(row + r, col + c, 3, color);
            else
                put_at(row + r, col + c, ' ', 0x00);
}

/* ── NEW: ASCII graph for BPM history ──────────────── */

static void draw_history_graph(void) {
    uint8_t border_color = (VGA_COLOR_BLACK << 4) | VGA_COLOR_DARK_GREY;
    uint8_t label_color  = (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_GREY;
    uint8_t grid_color   = (VGA_COLOR_BLACK << 4) | VGA_COLOR_DARK_GREY;

    for (int row = GRAPH_PANEL_TOP; row <= GRAPH_PANEL_BOTTOM; row++) {
        for (int col = GRAPH_PANEL_LEFT; col <= GRAPH_PANEL_RIGHT; col++)
            put_at(row, col, ' ', 0x00);
    }

    for (int col = GRAPH_PANEL_LEFT; col <= GRAPH_PANEL_RIGHT; col++) {
        put_at(GRAPH_PANEL_TOP, col, '-', border_color);
        put_at(GRAPH_PANEL_BOTTOM, col, '-', border_color);
    }

    for (int row = GRAPH_PANEL_TOP; row <= GRAPH_PANEL_BOTTOM; row++) {
        put_at(row, GRAPH_PANEL_LEFT, '|', border_color);
        put_at(row, GRAPH_PANEL_RIGHT, '|', border_color);
    }

    put_at(GRAPH_PANEL_TOP, GRAPH_PANEL_LEFT, '+', border_color);
    put_at(GRAPH_PANEL_TOP, GRAPH_PANEL_RIGHT, '+', border_color);
    put_at(GRAPH_PANEL_BOTTOM, GRAPH_PANEL_LEFT, '+', border_color);
    put_at(GRAPH_PANEL_BOTTOM, GRAPH_PANEL_RIGHT, '+', border_color);

    print_at(GRAPH_LABEL_ROW, GRAPH_PANEL_LEFT + 2, "TREND", label_color);
    print_at(GRAPH_LABEL_ROW, GRAPH_PANEL_RIGHT - 4, "LIVE", label_color);

    for (int row = GRAPH_INNER_TOP; row <= GRAPH_INNER_BOTTOM; row++) {
        for (int col = GRAPH_LEFT; col < GRAPH_LEFT + HISTORY_SIZE; col++) {
            char grid = ((GRAPH_INNER_BOTTOM - row) % 2 == 0) ? '.' : ' ';
            put_at(row, col, grid, grid_color);
        }
    }

    for (int i = 0; i < HISTORY_SIZE; i++) {
        int hist_pos = (hist_idx + i) % HISTORY_SIZE;
        int val = history[hist_pos];
        int height = (val - 50) / 20;
        uint8_t color = get_history_color(val, i == HISTORY_SIZE - 1);

        if (val > 0 && height == 0)
            height = 1;
        if (height > GRAPH_HEIGHT)
            height = GRAPH_HEIGHT;

        for (int h = 0; h < height; h++) {
            char cell = (h == height - 1) ? '^' : (char)219;
            put_at(GRAPH_INNER_BOTTOM - h, GRAPH_LEFT + i, cell, color);
        }
    }
}

/* ── Public API ───────────────── */

void display_init(void) {
    vga_init();
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_clear();

    /* Title bar */
    uint8_t title_color = (VGA_COLOR_BLUE << 4) | VGA_COLOR_WHITE;
    clear_row(0, title_color);

    /* Centered title (improvement) */
    const char *title = "  MicroANT v0.1  ";
    int col = (VGA_WIDTH - 18) / 2;
    print_at(0, col, title, title_color);

    /* Placeholder */
    uint8_t dim = (VGA_COLOR_BLACK << 4) | VGA_COLOR_DARK_GREY;
    print_at(10, 30, "--- BPM", dim);

    display_status("Initializing...");
}

void display_bpm(uint8_t bpm) {
    uint8_t heart_color = (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_RED;
    uint8_t digit_color = get_bpm_color(bpm);
    uint8_t bpm_color   = (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_GREY;

    /* Store history */
    history[hist_idx++] = bpm;
    if (hist_idx >= HISTORY_SIZE) hist_idx = 0;

    /* Clear area */
    for (int r = BPM_AREA_TOP; r < BPM_AREA_TOP + 7; r++)
        clear_row(r, 0x00);

    draw_heart(BPM_DRAW_ROW, 22, heart_color);

    int hundreds = bpm / 100;
    int tens     = (bpm / 10) % 10;
    int ones     = bpm % 10;

    int col = 32;

    if (hundreds > 0) {
        draw_big_digit(hundreds, BPM_DRAW_ROW, col, digit_color);
        col += 6;
    }
    if (hundreds > 0 || tens > 0) {
        draw_big_digit(tens, BPM_DRAW_ROW, col, digit_color);
        col += 6;
    }
    draw_big_digit(ones, BPM_DRAW_ROW, col, digit_color);
    col += 6;

    /* Vertical BPM */
    print_at(BPM_DRAW_ROW + 1, col + 1, "B", bpm_color);
    print_at(BPM_DRAW_ROW + 2, col + 1, "P", bpm_color);
    print_at(BPM_DRAW_ROW + 3, col + 1, "M", bpm_color);

    /* Numeric BPM (new) */
    char buf[4];
    u8_to_str(bpm, buf);
    print_at(BPM_AREA_TOP + 6, 36, buf, bpm_color);

    draw_history_graph();
}

void display_status(const char *msg) {
    uint8_t status_color = (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_GREEN;
    clear_row(24, 0x00);
    print_at(24, 1, "> ", status_color);
    print_at(24, 3, msg, status_color);
}
