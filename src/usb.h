/*
 * usb.h — USB device enumeration and transfer layer
 *
 * This module sits on top of the xHCI driver and provides:
 *   - Standard USB descriptor structures (device, config, interface, endpoint)
 *   - Control transfers on EP0 (GET_DESCRIPTOR, SET_CONFIGURATION)
 *   - Bulk IN/OUT transfers (used for ANT+ data)
 *   - Device lookup by VID/PID
 */

#ifndef USB_H
#define USB_H

#include <stdint.h>
#include "xhci.h"

/* ── Standard USB descriptor structures (USB 2.0 spec chapter 9) ───── */

/* Device Descriptor (18 bytes) — identifies the device (VID, PID, class) */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;    /* 1 = Device */
    uint16_t bcdUSB;             /* USB version (e.g., 0x0200 = USB 2.0) */
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;    /* Max packet size for EP0 */
    uint16_t idVendor;           /* e.g., 0x0FCF = Garmin/Dynastream */
    uint16_t idProduct;          /* e.g., 0x1008 = ANTUSB2 Stick */
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

/* Configuration Descriptor (9 bytes) — describes one device configuration */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;    /* 2 = Configuration */
    uint16_t wTotalLength;       /* Total size including all sub-descriptors */
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue; /* Value to pass to SET_CONFIGURATION */
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

/* Interface Descriptor (9 bytes) — describes one USB interface */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;    /* 4 = Interface */
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_interface_desc_t;

/* Endpoint Descriptor (7 bytes) — describes one endpoint */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;    /* 5 = Endpoint */
    uint8_t  bEndpointAddress;   /* Bit 7: 1=IN, 0=OUT. Bits 3:0: EP number */
    uint8_t  bmAttributes;       /* Bits 1:0: 0=Control, 2=Bulk, 3=Interrupt */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed)) usb_endpoint_desc_t;

/* ── Endpoint tracking for bulk transfers ──────────────────────────────
 *
 * DCI (Device Context Index) maps USB endpoints to xHCI context slots:
 *   DCI for IN endpoint N  = N * 2 + 1
 *   DCI for OUT endpoint N = N * 2
 */
typedef struct {
    uint8_t  addr_in;       /* Bulk IN endpoint address (e.g., 0x81) */
    uint8_t  addr_out;      /* Bulk OUT endpoint address (e.g., 0x01) */
    uint16_t max_pkt_in;
    uint16_t max_pkt_out;
    int      dci_in;        /* xHCI Device Context Index for IN */
    int      dci_out;       /* xHCI Device Context Index for OUT */
} usb_endpoints_t;

/* ── API ───────────────────────────────────────────────────────────── */

int usb_control_transfer(xhci_t *hc, xhci_device_t *dev,
                         uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex,
                         uint16_t wLength, void *data);

int usb_get_device_descriptor(xhci_t *hc, xhci_device_t *dev,
                              usb_device_desc_t *desc);
int usb_get_config_descriptor(xhci_t *hc, xhci_device_t *dev,
                              uint8_t *buf, uint16_t buf_len);
int usb_set_configuration(xhci_t *hc, xhci_device_t *dev, uint8_t config);

int usb_find_bulk_endpoints(uint8_t *config_buf, uint16_t total_len,
                            usb_endpoints_t *eps);
int usb_configure_endpoints(xhci_t *hc, xhci_device_t *dev,
                            usb_endpoints_t *eps);

int usb_bulk_out(xhci_t *hc, xhci_device_t *dev,
                 usb_endpoints_t *eps, void *data, uint16_t len);
int usb_bulk_in(xhci_t *hc, xhci_device_t *dev,
                usb_endpoints_t *eps, void *buf, uint16_t buf_len);

int usb_find_device(xhci_t *hc, uint16_t vid, uint16_t pid);

#endif
