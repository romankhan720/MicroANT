/*
 * pci.h — PCI bus enumeration for finding the xHCI controller
 *
 * PCI (Peripheral Component Interconnect) is the standard bus for
 * connecting hardware devices in a PC. Every device on the PCI bus
 * has a unique address identified by three numbers:
 *
 *   Bus    (0-255): which PCI bus the device is on
 *   Device (0-31):  which slot on that bus
 *   Function (0-7): which function of a multi-function device
 *
 * Each device has a 256-byte configuration space containing:
 *   - Vendor ID / Device ID (who made it, what model)
 *   - Class / Subclass / Prog-IF (what kind of device it is)
 *   - Base Address Registers (BARs): where its MMIO memory is mapped
 *   - Command register: enable/disable features like bus mastering
 *
 * On x86, PCI configuration space is accessed through two I/O ports:
 *   0xCF8 = CONFIG_ADDRESS (write the bus/dev/func/reg address)
 *   0xCFC = CONFIG_DATA    (read/write the 32-bit config value)
 *
 * We use PCI to find the xHCI (USB 3.0) controller, which is
 * identified by its class code: 0x0C (Serial Bus), 0x03 (USB),
 * 0x30 (xHCI).
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* PCI device descriptor — filled in by pci_find_xhci() */
typedef struct {
    uint8_t  bus, dev, func;   /* PCI address (bus/device/function) */
    uint16_t vendor_id;        /* Hardware vendor (e.g., 0x8086 = Intel) */
    uint16_t device_id;        /* Specific device model */
    uint32_t bar0;             /* Base Address Register 0: MMIO base address */
} pci_device_t;

/*
 * Read a 32-bit value from a PCI device's configuration space.
 *
 * The x86 PCI configuration mechanism works by:
 *   1. Writing a 32-bit address to I/O port 0xCF8:
 *      [31] Enable | [23:16] Bus | [15:11] Device | [10:8] Function | [7:2] Register
 *   2. Reading the 32-bit result from I/O port 0xCFC
 *
 * The register offset must be 4-byte aligned (bits 1:0 are masked off).
 */
uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg);

/* Write a 32-bit value to a PCI device's configuration space. */
void     pci_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t reg, uint32_t val);

/*
 * Scan the entire PCI bus to find the xHCI USB 3.0 controller.
 *
 * Iterates over all possible bus/device/function combinations and
 * checks the class code register (offset 0x08) for:
 *   Base Class = 0x0C (Serial Bus Controller)
 *   Sub Class  = 0x03 (USB Controller)
 *   Prog IF    = 0x30 (xHCI — USB 3.0)
 *
 * When found, also enables Bus Mastering and Memory Space in the
 * PCI command register, which are required for the xHCI controller
 * to perform DMA and respond to MMIO accesses.
 *
 * Returns 1 if found (result in *out), 0 if not found.
 */
int pci_find_xhci(pci_device_t *out);

/*
 * Enable bus mastering and memory space on a PCI device.
 *
 * Modifies the PCI Command register (offset 0x04):
 *   Bit 1: Memory Space — allows the device to respond to MMIO reads/writes
 *   Bit 2: Bus Master   — allows the device to initiate DMA transfers
 *
 * Both are required for xHCI: the controller uses DMA to read TRBs
 * from command/transfer rings and write events to the event ring.
 */
void pci_enable_bus_master(pci_device_t *dev);

#endif
