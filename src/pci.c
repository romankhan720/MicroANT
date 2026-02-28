/*
 * pci.c — PCI bus enumeration via I/O ports 0xCF8/0xCFC
 *
 * The x86 architecture provides a mechanism to access PCI configuration
 * space through two I/O ports:
 *
 *   Port 0xCF8 — CONFIG_ADDRESS:
 *     Write a 32-bit address specifying which PCI register to access.
 *     Format: [31] Enable bit (must be 1)
 *             [23:16] Bus number (0-255)
 *             [15:11] Device number (0-31)
 *             [10:8]  Function number (0-7)
 *             [7:2]   Register offset (must be 4-byte aligned)
 *             [1:0]   Always 00
 *
 *   Port 0xCFC — CONFIG_DATA:
 *     Read or write the 32-bit value at the address set in 0xCF8.
 *
 * This is called "Configuration Mechanism #1" and is the standard
 * method used by all modern PCs.
 *
 * To find the xHCI controller, we scan all 256 buses × 32 devices ×
 * 8 functions and check each device's class code register for the
 * USB 3.0 xHCI class code (0x0C0330).
 */

#include "pci.h"
#include "io.h"

/*
 * Read a 32-bit value from PCI configuration space.
 *
 * Step 1: Build the CONFIG_ADDRESS word
 *   - Bit 31 = 1 (enable configuration access)
 *   - Bits 23:16 = bus number
 *   - Bits 15:11 = device number
 *   - Bits 10:8  = function number
 *   - Bits 7:2   = register offset (aligned to 4 bytes)
 *
 * Step 2: Write CONFIG_ADDRESS to port 0xCF8
 * Step 3: Read the 32-bit result from port 0xCFC
 */
uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg) {
    uint32_t addr = (1u << 31)                 /* Enable bit */
                  | ((uint32_t)bus  << 16)     /* Bus number */
                  | ((uint32_t)dev  << 11)     /* Device number */
                  | ((uint32_t)func <<  8)     /* Function number */
                  | (reg & 0xFC);              /* Register (4-byte aligned) */
    outl(0xCF8, addr);      /* Set the address */
    return inl(0xCFC);      /* Read the data */
}

/* Write a 32-bit value to PCI configuration space.
 * Same address formation as pci_read, but writes to 0xCFC instead. */
void pci_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg,
               uint32_t val) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus  << 16)
                  | ((uint32_t)dev  << 11)
                  | ((uint32_t)func <<  8)
                  | (reg & 0xFC);
    outl(0xCF8, addr);      /* Set the address */
    outl(0xCFC, val);       /* Write the data */
}

/*
 * Enable bus mastering and memory space on a PCI device.
 *
 * The PCI Command Register (offset 0x04) controls how the device
 * interacts with the system:
 *
 *   Bit 1: Memory Space Enable
 *     When set, the device responds to MMIO accesses at its BAR addresses.
 *     Without this, writing to the xHCI registers would have no effect.
 *
 *   Bit 2: Bus Master Enable
 *     When set, the device can initiate DMA (Direct Memory Access) transfers.
 *     xHCI relies heavily on DMA: it reads TRBs from rings in system memory
 *     and writes completion events back. Without bus mastering, the
 *     controller would sit idle, unable to fetch any commands.
 *
 * We read the current command register value and OR in both bits to
 * preserve any other flags that may already be set.
 */
void pci_enable_bus_master(pci_device_t *dev) {
    uint32_t cmd = pci_read(dev->bus, dev->dev, dev->func, 0x04);
    cmd |= (1 << 1) | (1 << 2);  /* Memory Space + Bus Master */
    pci_write(dev->bus, dev->dev, dev->func, 0x04, cmd);
}

/*
 * Scan the PCI bus to find the xHCI (USB 3.0) host controller.
 *
 * PCI device identification uses the Class Code register (offset 0x08):
 *   Bits 31:24 = Base Class  (broad category)
 *   Bits 23:16 = Sub Class   (specific type within category)
 *   Bits 15:8  = Prog IF     (programming interface variant)
 *
 * USB controllers are categorized as:
 *   Base Class 0x0C = Serial Bus Controller
 *   Sub Class  0x03 = USB Controller
 *   Prog IF    0x00 = UHCI (USB 1.0)
 *   Prog IF    0x10 = OHCI (USB 1.0)
 *   Prog IF    0x20 = EHCI (USB 2.0)
 *   Prog IF    0x30 = xHCI (USB 3.0) ← what we're looking for
 *
 * When we find the xHCI controller:
 *   1. Record its PCI address (bus/device/function)
 *   2. Read its Vendor ID and Device ID
 *   3. Read BAR0 (Base Address Register 0) for the MMIO base address
 *      BAR0 bits 3:0 are flags, so we mask them off with & 0xFFFFFFF0
 *   4. Enable bus mastering and memory space (required for xHCI DMA)
 */
int pci_find_xhci(pci_device_t *out) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                /* Read Vendor ID + Device ID (register 0x00) */
                uint32_t id = pci_read(bus, dev, func, 0x00);

                /* 0xFFFFFFFF means no device at this address */
                if (id == 0xFFFFFFFF)
                    continue;

                /* Read Class Code register (offset 0x08) */
                uint32_t class_reg = pci_read(bus, dev, func, 0x08);
                uint8_t base_class = (class_reg >> 24) & 0xFF;
                uint8_t sub_class  = (class_reg >> 16) & 0xFF;
                uint8_t prog_if    = (class_reg >>  8) & 0xFF;

                /* Check for xHCI: class 0x0C (Serial Bus),
                 * subclass 0x03 (USB), prog-if 0x30 (xHCI) */
                if (base_class == 0x0C && sub_class == 0x03 && prog_if == 0x30) {
                    out->bus  = bus;
                    out->dev  = dev;
                    out->func = func;
                    out->vendor_id = id & 0xFFFF;
                    out->device_id = (id >> 16) & 0xFFFF;
                    /* BAR0 contains the MMIO base address.
                     * Bits 3:0 are type/prefetchable flags, not part of the address */
                    out->bar0 = pci_read(bus, dev, func, 0x10) & 0xFFFFFFF0;

                    /* Enable DMA and MMIO before returning */
                    pci_enable_bus_master(out);
                    return 1;
                }
            }
        }
    }
    return 0;  /* No xHCI controller found */
}
