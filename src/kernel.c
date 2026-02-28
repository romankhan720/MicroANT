/*
 * kernel.c — Main entry point for MicroANT
 *
 * This is the heart of MicroANT. After the bootloader hands control to
 * kernel_main(), we walk through each layer of the hardware stack:
 *
 *   1. VGA display     — set up the screen with a title bar and status line
 *   2. PCI bus scan     — find the xHCI (USB 3.0) controller on the PCI bus
 *   3. xHCI init        — reset the controller and set up its ring buffers
 *   4. USB enumeration  — detect connected devices, read their descriptors
 *   5. ANT+ setup       — configure the Garmin stick for heart rate reception
 *   6. Main loop        — continuously poll ANT+ data and display BPM
 *
 * If any step fails, we display an error message and halt the CPU.
 * There is no recovery mechanism — this is a single-purpose OS.
 */

#include "vga.h"
#include "display.h"
#include "pci.h"
#include "xhci.h"
#include "usb.h"
#include "ant.h"

/*
 * halt() — Stop the CPU permanently.
 *
 * CLI disables interrupts so nothing can wake us up.
 * HLT puts the CPU in a low-power state. The infinite loop is a safety
 * net in case a Non-Maskable Interrupt (NMI) wakes the CPU from HLT.
 */
static void halt(void) {
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/*
 * delay_ms() — Busy-wait delay (approximate).
 *
 * Without a proper timer driver, we use a calibrated busy loop.
 * The actual duration depends on CPU speed and is only approximate,
 * but it's good enough for waiting on USB device attachment.
 */
static void delay_ms(int ms) {
    for (volatile int i = 0; i < ms * 10000; i++)
        ;
}

/*
 * debug_hex() — Print a label followed by a 32-bit value in hexadecimal.
 *
 * Useful for debugging hardware registers (BAR addresses, PORTSC values, etc.).
 * Converts the value nibble-by-nibble from least significant to most significant.
 */
static void debug_hex(const char *label, uint32_t val) {
    vga_print(label);
    const char hex[] = "0123456789ABCDEF";
    char buf[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    vga_print(buf);
    vga_newline();
}

/*
 * kernel_main() — Entry point called by boot_x86.asm after stack setup.
 *
 * This function never returns. It either enters the infinite BPM polling
 * loop or halts on error.
 */
void kernel_main(void) {

    /* ── Step 1: Initialize the VGA display ────────────────────────────
     *
     * Clear the screen, draw the "MicroANT v0.1" title bar, and show
     * the initial status message. All subsequent output goes through
     * the VGA text mode driver (80 columns x 25 rows at 0xB8000).
     */
    display_init();

    /* ── Step 2: Scan the PCI bus for an xHCI controller ───────────────
     *
     * The PCI bus connects all hardware devices on the motherboard.
     * We scan all 256 buses, 32 devices per bus, 8 functions per device,
     * looking for a device with:
     *   - Class 0x0C (Serial Bus Controller)
     *   - Subclass 0x03 (USB Controller)
     *   - Programming Interface 0x30 (xHCI / USB 3.0)
     *
     * Once found, we read its BAR0 (Base Address Register 0) which tells
     * us where the controller's MMIO registers are mapped in memory.
     */
    display_status("Scanning PCI bus...");
    pci_device_t xhci_pci;
    if (!pci_find_xhci(&xhci_pci)) {
        display_status("ERROR: No xHCI controller found");
        halt();
    }

    /* ── Step 3: Initialize the xHCI USB host controller ───────────────
     *
     * This resets the controller and sets up:
     *   - DCBAA (Device Context Base Address Array) — pointers to device contexts
     *   - Command Ring — where we send commands to the controller
     *   - Event Ring — where the controller sends us completion notifications
     *
     * After initialization, the controller is running and ready to accept
     * commands like Enable Slot, Address Device, etc.
     */
    display_status("Initializing USB controller...");
    xhci_t hc;
    if (xhci_init(&hc, xhci_pci.bar0) != 0) {
        display_status("ERROR: xHCI initialization failed");
        halt();
    }

    /* Debug: display controller capabilities */
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    debug_hex("BAR0=", xhci_pci.bar0);
    vga_print("Ports="); vga_print_int(hc.max_ports);
    vga_print(" Slots="); vga_print_int(hc.max_slots);
    vga_print(" CtxSz="); vga_print_int(hc.context_size);
    vga_newline();
    debug_hex("USBSTS=", *(volatile uint32_t *)(hc.op + 0x04));

    /* ── Step 4: Wait for USB device attachment ────────────────────────
     *
     * With QEMU USB passthrough, the virtual xHCI controller needs a
     * moment to recognize the physical device. We wait 1 second, then
     * check each port's PORTSC (Port Status and Control) register.
     *
     * PORTSC bit 0 (CCS = Current Connect Status) indicates a device
     * is physically present on that port.
     */
    display_status("Waiting for USB device attachment...");
    delay_ms(1000);

    /* Debug: dump each port's PORTSC register */
    for (int i = 0; i < hc.max_ports && i < 8; i++) {
        uint32_t portsc = *(volatile uint32_t *)(hc.ports + i * 0x10);
        vga_print("P"); vga_print_int(i); vga_print("=");
        debug_hex("", portsc);
    }

    /* ── Step 5: Enumerate USB devices ─────────────────────────────────
     *
     * We retry up to 20 times (with 500ms delays) because the device
     * may need time to complete link training after attachment.
     *
     * xhci_scan_ports() does two passes:
     *   Pass 1: Handle ports already enabled (PED=1) — common with QEMU
     *           passthrough where the host OS already enabled the port.
     *   Pass 2: Reset ports that have CCS=1 but aren't enabled yet.
     *
     * For each detected device, the driver:
     *   1. Sends an Enable Slot command to reserve a device slot
     *   2. Allocates input/output device contexts in memory
     *   3. Sends an Address Device command to assign a USB address
     */
    int devices = 0;
    for (int attempt = 0; attempt < 20 && devices == 0; attempt++) {
        int any_connected = 0;
        for (int i = 0; i < hc.max_ports; i++) {
            uint32_t portsc = *(volatile uint32_t *)(hc.ports + i * 0x10);
            if (portsc & 0x1) any_connected = 1;  /* CCS = bit 0 */
        }

        if (any_connected) {
            display_status("Device detected, enumerating...");
            devices = xhci_scan_ports(&hc);
        } else {
            display_status("Scanning USB ports...");
        }
        delay_ms(500);
    }
    if (devices == 0) {
        vga_newline();
        vga_print("After retries:");
        vga_newline();
        for (int i = 0; i < hc.max_ports && i < 8; i++) {
            uint32_t portsc = *(volatile uint32_t *)(hc.ports + i * 0x10);
            if (portsc != 0) {
                vga_print("P"); vga_print_int(i); vga_print("=");
                debug_hex("", portsc);
            }
        }
        display_status("ERROR: No USB devices found");
        halt();
    }

    /* ── Step 6: Find the Garmin ANT+ stick ────────────────────────────
     *
     * Among the enumerated USB devices, we look for one matching
     * Garmin's Vendor ID (0x0FCF) and either:
     *   - Product ID 0x1009 (ANT USB Stick 3)
     *   - Product ID 0x1008 (ANTUSB2 Stick)
     *
     * usb_find_device() reads each device's descriptor (GET_DESCRIPTOR)
     * to retrieve its VID/PID and compares them.
     */
    display_status("Looking for ANT+ stick...");
    int dev_idx = usb_find_device(&hc, GARMIN_VID, GARMIN_STICK3_PID);
    if (dev_idx < 0)
        dev_idx = usb_find_device(&hc, GARMIN_VID, GARMIN_STICK2_PID);
    if (dev_idx < 0) {
        display_status("ERROR: No ANT+ stick found (Garmin USB)");
        halt();
    }

    /* ── Step 7: Configure the ANT+ heart rate channel ─────────────────
     *
     * ant_init() performs the full ANT+ setup sequence (ported from
     * tapinoma's driver.js attach() function):
     *
     *   1. Read USB config descriptor → find bulk IN/OUT endpoints
     *   2. SET_CONFIGURATION → activate the USB interface
     *   3. Configure bulk endpoint contexts on the xHCI controller
     *   4. Send ANT+ commands via bulk OUT:
     *      - System Reset
     *      - Set Network Key (ANT+ public key)
     *      - Assign Channel (receive mode)
     *      - Set Channel ID (device type = Heart Rate = 120)
     *      - Set Search Timeout (infinite)
     *      - Set Channel Period (8070 = ~4.06 Hz)
     *      - Set Channel RF Frequency (2457 MHz = base 2400 + 57)
     *      - Open Channel
     */
    display_status("Configuring ANT+ heart rate channel...");
    ant_state_t ant;
    int ret = ant_init(&ant, &hc, &hc.devices[dev_idx]);
    if (ret != 0) {
        display_status("ERROR: ANT+ initialization failed");
        halt();
    }

    /* ── Step 8: Main loop — poll ANT+ and display BPM ─────────────────
     *
     * ant_poll_heart_rate() reads bulk IN data from the ANT+ stick.
     * When a heart rate broadcast message (type 0x4E) arrives, the
     * Computed Heart Rate is extracted from byte offset 11 in the
     * raw USB data (see ant.c for the exact byte layout).
     *
     * The BPM value (1–249) is rendered as large ASCII-art digits
     * alongside a heart icon on the VGA screen.
     */
    display_status("Waiting for heart rate sensor...");

    for (;;) {
        uint8_t bpm = ant_poll_heart_rate(&ant);
        if (bpm > 0) {
            display_bpm(bpm);
            display_status("Receiving heart rate data");
        }
    }
}
