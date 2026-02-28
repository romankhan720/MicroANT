/*
 * usb.c — USB enumeration and data transfer layer
 *
 * This module provides the standard USB operations needed to talk to the
 * ANT+ stick: reading descriptors, activating a configuration, and
 * performing bulk IN/OUT transfers.
 *
 * All USB communication happens through the xHCI controller via TRBs:
 *
 *   Control transfer (EP0):  [Setup TRB] -> [Data TRB] -> [Status TRB]
 *   Bulk transfer:           [Normal TRB with data buffer pointer]
 *
 * After each transfer, we ring the device's doorbell and wait for a
 * Transfer Event on the event ring confirming completion.
 */

#include "usb.h"
#include "alloc.h"
#include "string.h"

/* ── Control transfer (Setup -> Data -> Status) on EP0 ─────────────────
 *
 * USB control transfers have three phases:
 *
 * 1. Setup Stage: an 8-byte packet telling the device what we want.
 *    Packed into two 32-bit words and sent as Immediate Data (IDT flag).
 *
 * 2. Data Stage (optional): the actual data payload, read from or
 *    written to a DMA-accessible buffer.
 *
 * 3. Status Stage: a zero-length handshake confirming the transfer.
 *    Direction is opposite to the data stage. IOC flag triggers the event.
 */
int usb_control_transfer(xhci_t *hc, xhci_device_t *dev,
                         uint8_t bmRequestType, uint8_t bRequest,
                         uint16_t wValue, uint16_t wIndex,
                         uint16_t wLength, void *data) {
    xhci_ring_t *ring = &dev->ep_ring[0];

    /* Pack the 8-byte USB setup packet into two 32-bit words */
    uint32_t setup_lo = (uint32_t)bmRequestType
                      | ((uint32_t)bRequest << 8)
                      | ((uint32_t)wValue << 16);
    uint32_t setup_hi = (uint32_t)wIndex
                      | ((uint32_t)wLength << 16);

    /* Transfer Type: 0=No Data, 2=OUT Data, 3=IN Data */
    uint8_t dir_in = (bmRequestType & 0x80) ? 1 : 0;
    uint32_t trt = (wLength == 0) ? 0 : (dir_in ? 3 : 2);

    /* Setup Stage TRB */
    ring_enqueue(ring, setup_lo, setup_hi,
                 8,  /* Transfer Length = 8 (setup packet is always 8 bytes) */
                 (TRB_SETUP_STAGE << 10)
               | (trt << 16)
               | (1 << 6));  /* IDT = Immediate Data */

    /* Data Stage TRB (only if there's data to transfer) */
    uint8_t *buf = (void *)0;
    if (wLength > 0) {
        buf = alloc_phys(wLength, 64);
        if (!dir_in && data)
            memcpy(buf, data, wLength);

        ring_enqueue(ring,
                     (uint32_t)(uintptr_t)buf, 0,
                     (uint32_t)wLength,
                     (TRB_DATA_STAGE << 10)
                   | (dir_in ? (1 << 16) : 0));  /* DIR = 1 for IN */
    }

    /* Status Stage TRB — direction opposite to data stage */
    uint32_t status_dir = (wLength > 0 && dir_in) ? 0 : (1 << 16);
    ring_enqueue(ring, 0, 0, 0,
                 (TRB_STATUS_STAGE << 10)
               | status_dir
               | (1 << 5));  /* IOC = Interrupt On Completion */

    /* Ring doorbell: EP0 DCI = 1 */
    hc->db[dev->slot_id] = 1;

    /* Wait for transfer completion */
    xhci_trb_t ev;
    uint8_t cc = xhci_wait_event(hc, TRB_TRANSFER_EVENT, &ev);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET)
        return -1;

    /* Copy received data back to caller */
    if (wLength > 0 && dir_in && data)
        memcpy(data, buf, wLength);

    return 0;
}

/* ── Descriptor requests ───────────────────────────────────────────── */

/*
 * Read the 18-byte device descriptor. Caches VID/PID in the device struct.
 * USB request: GET_DESCRIPTOR (bmRequestType=0x80, bRequest=6)
 */
int usb_get_device_descriptor(xhci_t *hc, xhci_device_t *dev,
                              usb_device_desc_t *desc) {
    int ret = usb_control_transfer(hc, dev,
                                   0x80, 6,       /* GET_DESCRIPTOR */
                                   (1 << 8) | 0,  /* type=Device, index=0 */
                                   0, sizeof(usb_device_desc_t), desc);
    if (ret == 0) {
        dev->vendor_id = desc->idVendor;
        dev->product_id = desc->idProduct;
        dev->num_configurations = desc->bNumConfigurations;
    }
    return ret;
}

/*
 * Read the full configuration descriptor tree (config + interfaces + endpoints).
 * First reads the 9-byte header to learn wTotalLength, then reads the full tree.
 */
int usb_get_config_descriptor(xhci_t *hc, xhci_device_t *dev,
                              uint8_t *buf, uint16_t buf_len) {
    usb_config_desc_t hdr;
    int ret = usb_control_transfer(hc, dev,
                                   0x80, 6,
                                   (2 << 8) | 0,  /* type=Configuration */
                                   0, sizeof(hdr), &hdr);
    if (ret != 0) return ret;

    uint16_t total = hdr.wTotalLength;
    if (total > buf_len) total = buf_len;

    return usb_control_transfer(hc, dev,
                                0x80, 6,
                                (2 << 8) | 0,
                                0, total, buf);
}

/* Send SET_CONFIGURATION to activate a USB configuration */
int usb_set_configuration(xhci_t *hc, xhci_device_t *dev, uint8_t config) {
    return usb_control_transfer(hc, dev,
                                0x00, 9,   /* SET_CONFIGURATION */
                                config, 0, 0, (void *)0);
}

/* ── Endpoint discovery ────────────────────────────────────────────────
 *
 * Walk the configuration descriptor byte stream to find bulk IN/OUT
 * endpoints. The ANT+ stick has one of each.
 *
 * Endpoint direction is in bit 7 of bEndpointAddress: 1=IN, 0=OUT.
 * Transfer type is in bits 1:0 of bmAttributes: 2=Bulk.
 */
int usb_find_bulk_endpoints(uint8_t *config_buf, uint16_t total_len,
                            usb_endpoints_t *eps) {
    memset(eps, 0, sizeof(usb_endpoints_t));
    uint16_t offset = 0;

    while (offset < total_len) {
        uint8_t len  = config_buf[offset];
        uint8_t type = config_buf[offset + 1];

        if (len == 0) break;

        /* Endpoint descriptor: type=5, min length=7 */
        if (type == 5 && len >= 7) {
            usb_endpoint_desc_t *ep = (usb_endpoint_desc_t *)&config_buf[offset];
            uint8_t xfer_type = ep->bmAttributes & 0x03;

            if (xfer_type == 2) {  /* Bulk */
                if (ep->bEndpointAddress & 0x80) {
                    eps->addr_in = ep->bEndpointAddress;
                    eps->max_pkt_in = ep->wMaxPacketSize;
                } else {
                    eps->addr_out = ep->bEndpointAddress;
                    eps->max_pkt_out = ep->wMaxPacketSize;
                }
            }
        }

        offset += len;
    }

    if (eps->addr_in && eps->addr_out)
        return 0;
    return -1;
}

/* ── Endpoint configuration on xHCI ────────────────────────────────────
 *
 * Tell the xHCI controller about the bulk endpoints by sending a
 * Configure Endpoint command with an Input Context.
 *
 * DCI mapping: IN endpoint N -> DCI = N*2+1, OUT endpoint N -> DCI = N*2
 */
int usb_configure_endpoints(xhci_t *hc, xhci_device_t *dev,
                            usb_endpoints_t *eps) {
    uint32_t csz = hc->context_size;

    uint8_t ep_num_in  = eps->addr_in & 0x0F;
    uint8_t ep_num_out = eps->addr_out & 0x0F;
    eps->dci_in  = ep_num_in * 2 + 1;
    eps->dci_out = ep_num_out * 2;

    int max_dci = eps->dci_in > eps->dci_out ? eps->dci_in : eps->dci_out;

    /* Init transfer rings: [1]=bulk IN, [2]=bulk OUT */
    ring_init(&dev->ep_ring[1]);
    ring_init(&dev->ep_ring[2]);

    /* Build Input Context */
    uint8_t *input_ctx = alloc_phys(33 * csz, 64);
    uint32_t *icc = (uint32_t *)input_ctx;
    icc[1] = (1 << 0)               /* Add: Slot */
           | (1 << eps->dci_in)      /* Add: bulk IN */
           | (1 << eps->dci_out);    /* Add: bulk OUT */

    /* Slot Context: Context Entries = max_dci */
    xhci_slot_ctx_t *slot_ctx = (xhci_slot_ctx_t *)(input_ctx + csz);
    slot_ctx->field[0] = ((uint32_t)max_dci << 27) | ((uint32_t)dev->speed << 20);
    slot_ctx->field[1] = ((uint32_t)(dev->port + 1)) << 16;

    /* Bulk IN Endpoint Context: EP Type=6 (Bulk IN), CErr=3 */
    xhci_ep_ctx_t *ep_in = (xhci_ep_ctx_t *)(input_ctx + (eps->dci_in + 1) * csz);
    ep_in->field1 = (3 << 1) | (6 << 3) | ((uint32_t)eps->max_pkt_in << 16);
    ep_in->tr_dequeue_lo = (uint32_t)(uintptr_t)dev->ep_ring[1].trbs | 1;
    ep_in->tr_dequeue_hi = 0;
    ep_in->field4 = eps->max_pkt_in;

    /* Bulk OUT Endpoint Context: EP Type=2 (Bulk OUT), CErr=3 */
    xhci_ep_ctx_t *ep_out = (xhci_ep_ctx_t *)(input_ctx + (eps->dci_out + 1) * csz);
    ep_out->field1 = (3 << 1) | (2 << 3) | ((uint32_t)eps->max_pkt_out << 16);
    ep_out->tr_dequeue_lo = (uint32_t)(uintptr_t)dev->ep_ring[2].trbs | 1;
    ep_out->tr_dequeue_hi = 0;
    ep_out->field4 = eps->max_pkt_out;

    /* Send Configure Endpoint command */
    xhci_trb_t ev;
    ring_enqueue(&hc->cmd_ring,
                 (uint32_t)(uintptr_t)input_ctx, 0, 0,
                 (TRB_CONFIGURE_EP << 10) | ((uint32_t)dev->slot_id << 24));
    hc->db[0] = 0;

    uint8_t cc = xhci_wait_event(hc, TRB_CMD_COMPLETION, &ev);
    return (cc == CC_SUCCESS) ? 0 : -1;
}

/* ── Bulk transfers ────────────────────────────────────────────────────
 *
 * Bulk transfers: enqueue a Normal TRB, ring the doorbell, wait for event.
 * The Transfer Event's status field contains the "residual" count
 * (bytes NOT transferred), so actual = requested - residual.
 */

int usb_bulk_out(xhci_t *hc, xhci_device_t *dev,
                 usb_endpoints_t *eps, void *data, uint16_t len) {
    uint8_t *buf = alloc_phys(len, 64);
    memcpy(buf, data, len);

    ring_enqueue(&dev->ep_ring[2],  /* bulk OUT ring */
                 (uint32_t)(uintptr_t)buf, 0,
                 (uint32_t)len,
                 (TRB_NORMAL << 10) | (1 << 5));  /* IOC */

    hc->db[dev->slot_id] = eps->dci_out;

    xhci_trb_t ev;
    uint8_t cc = xhci_wait_event(hc, TRB_TRANSFER_EVENT, &ev);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 0 : -1;
}

int usb_bulk_in(xhci_t *hc, xhci_device_t *dev,
                usb_endpoints_t *eps, void *buf, uint16_t buf_len) {
    uint8_t *rxbuf = alloc_phys(buf_len, 64);

    ring_enqueue(&dev->ep_ring[1],  /* bulk IN ring */
                 (uint32_t)(uintptr_t)rxbuf, 0,
                 (uint32_t)buf_len,
                 (TRB_NORMAL << 10) | (1 << 5));  /* IOC */

    hc->db[dev->slot_id] = eps->dci_in;

    xhci_trb_t ev;
    uint8_t cc = xhci_wait_event(hc, TRB_TRANSFER_EVENT, &ev);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET)
        return -1;

    uint32_t residual = ev.status & 0x00FFFFFF;
    uint32_t received = buf_len - residual;

    memcpy(buf, rxbuf, received);
    return (int)received;
}

/* ── Device lookup by VID/PID ──────────────────────────────────────── */

int usb_find_device(xhci_t *hc, uint16_t vid, uint16_t pid) {
    for (int i = 0; i < hc->num_devices; i++) {
        xhci_device_t *dev = &hc->devices[i];
        if (dev->vendor_id == 0) {
            usb_device_desc_t desc;
            if (usb_get_device_descriptor(hc, dev, &desc) != 0)
                continue;
        }
        if (dev->vendor_id == vid && dev->product_id == pid)
            return i;
    }
    return -1;
}
