/*
 * display.h — BPM display with large digits on VGA text mode
 *
 * This module handles the visual presentation of the heart rate
 * on a standard VGA 80x25 text mode screen. It provides:
 *
 *   - A splash screen with the MicroANT title bar
 *   - Large digit rendering (5x5 character block font)
 *   - Heart symbol animation
 *   - Status line at the bottom of the screen
 *
 * Screen layout:
 *   Row 0:      Title bar ("MicroANT v0.1") on blue background
 *   Rows 8-14:  Heart icon + large BPM digits + "BPM" label
 *   Row 24:     Status line (green text, shows current state)
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

/* Initialize the display: clear screen, draw title bar, show placeholder. */
void display_init(void);

/* Update the heart rate display with large centered digits.
 * Draws a heart icon, the BPM value in 5x5 block characters,
 * and a vertical "BPM" label next to the number. */
void display_bpm(uint8_t bpm);

/* Update the status line at the bottom of the screen (row 24).
 * Shows a "> " prompt followed by the message in green text. */
void display_status(const char *msg);

#endif
