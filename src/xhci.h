/*
 * xhci.h — xHCI (eXtensible Host Controller Interface) data structures
 *
 * The xHCI specification defines how software communicates with USB 3.0
 * host controllers. All communication happens through shared memory
 * structures called "rings" — circular buffers of 16-byte Transfer
 * Request Blocks (TRBs).
 *
 * Three types of rings exist:
 *   - Command Ring:  software -> controller (Enable Slot, Address Device, ...)
 *   - Transfer Ring: software -> controller (USB data transfers per endpoint)
 *   - Event Ring:    controller -> software (completion notifications)
 *
 * The controller also maintains per-device "contexts" — memory structures
 * describing each device's slot and endpoint configuration.
 *
 * Reference: xHCI Specification 1.2, Intel
 */

#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

/* ── TRB (Transfer Request Block) — the fundamental unit of xHCI I/O ──
 *
 * Every command, data transfer, and event is represented as a 16-byte TRB.
 * The layout is:
 *   [0:3]  param_lo  — parameter low  (address, command-specific data)
 *   [4:7]  param_hi  — parameter high (upper 32 bits of 64-bit address)
 *   [8:11] status    — transfer length, completion code, etc.
 *   [12:15] control  — TRB type (bits 15:10), cycle bit (bit 0), flags
 *
 * The "cycle bit" (bit 0 of control) is crucial: it tells the controller
 * which TRBs are "owned" by software vs. hardware. When software writes
 * a TRB, it sets the cycle bit to match the ring's current cycle state.
 * The controller only processes TRBs whose cycle bit matches its expectation.
 */
typedef struct {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

/* TRB types — encoded in bits 15:10 of the control field */
#define TRB_NORMAL              1   /* Normal data transfer */
#define TRB_SETUP_STAGE         2   /* USB control transfer: setup packet */
#define TRB_DATA_STAGE          3   /* USB control transfer: data phase */
#define TRB_STATUS_STAGE        4   /* USB control transfer: status handshake */
#define TRB_LINK                6   /* Link TRB: wraps ring back to start */
#define TRB_ENABLE_SLOT         9   /* Command: allocate a device slot */
#define TRB_SET_TR_DEQUEUE     10   /* Command: set transfer ring dequeue pointer */
#define TRB_ADDRESS_DEVICE     11   /* Command: assign USB address to device */
#define TRB_CONFIGURE_EP       12   /* Command: configure device endpoints */
#define TRB_RESET_EP           14   /* Command: reset a stalled endpoint */
#define TRB_TRANSFER_EVENT     32   /* Event: a transfer completed */
#define TRB_CMD_COMPLETION     33   /* Event: a command completed */
#define TRB_PORT_STATUS_CHANGE 34   /* Event: a port's status changed */

/* Completion codes — found in bits 31:24 of the event TRB's status field */
#define CC_SUCCESS              1   /* Operation completed successfully */
#define CC_SHORT_PACKET        13   /* Device sent less data than requested (OK) */

/* ── Ring — a circular buffer of TRBs ──────────────────────────────────
 *
 * The last TRB in the ring is always a Link TRB that points back to the
 * first entry, creating the circular structure. When the Link TRB is
 * reached, the cycle bit toggles (Toggle Cycle = TC flag).
 *
 * RING_SIZE of 256 means 255 usable entries + 1 Link TRB.
 */
#define RING_SIZE 256

typedef struct {
    xhci_trb_t *trbs;      /* Pointer to the TRB array (physically contiguous) */
    uint32_t    enqueue;    /* Next slot to write to (producer index) */
    uint32_t    cycle;      /* Current cycle state (0 or 1) */
} xhci_ring_t;

/* ── Event Ring Segment Table Entry ────────────────────────────────────
 *
 * The ERST tells the controller where the event ring lives in memory.
 * We use a single segment covering all EVENT_RING_SIZE entries.
 */
typedef struct {
    uint32_t base_lo;       /* Physical address of the TRB array (low 32 bits) */
    uint32_t base_hi;       /* Physical address (high 32 bits, 0 for 32-bit) */
    uint32_t size;          /* Number of TRBs in this segment */
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

#define EVENT_RING_SIZE 256

typedef struct {
    xhci_trb_t       *trbs;     /* TRB array for events */
    xhci_erst_entry_t *erst;    /* Event Ring Segment Table (1 entry) */
    uint32_t           dequeue;  /* Next event to read (consumer index) */
    uint32_t           cycle;    /* Expected cycle bit for next event */
} xhci_event_ring_t;

/* ── Device Contexts ───────────────────────────────────────────────────
 *
 * Each USB device gets a Slot Context (describing connection) and
 * Endpoint Contexts (one per active endpoint). Context size is either
 * 32 or 64 bytes depending on the controller (CSZ flag in HCCPARAMS1).
 */

/* Slot Context — describes a USB device's connection */
typedef struct {
    uint32_t field[8];
} __attribute__((packed)) xhci_slot_ctx_t;

/* Endpoint Context — describes one endpoint's transfer ring and parameters */
typedef struct {
    uint32_t field0;
    uint32_t field1;            /* CErr, EP Type, Max Packet Size */
    uint32_t tr_dequeue_lo;     /* Transfer Ring Dequeue Pointer + DCS */
    uint32_t tr_dequeue_hi;
    uint32_t field4;            /* Average TRB Length */
    uint32_t reserved[3];
} __attribute__((packed)) xhci_ep_ctx_t;

/* ── Per-device tracking ───────────────────────────────────────────── */

#define MAX_ENDPOINTS 4

typedef struct {
    int          slot_id;           /* xHCI slot ID (1-based) */
    int          port;              /* Physical port index (0-based) */
    int          speed;             /* USB speed (1=Full, 2=Low, 3=High, 4=Super) */
    uint16_t     vendor_id;         /* USB Vendor ID from device descriptor */
    uint16_t     product_id;        /* USB Product ID from device descriptor */
    uint8_t      num_configurations;
    uint8_t     *output_ctx;        /* Output Device Context (controller-managed) */
    uint8_t     *input_ctx;         /* Input Context (software-managed) */
    xhci_ring_t  ep_ring[MAX_ENDPOINTS];  /* [0]=EP0, [1]=bulk IN, [2]=bulk OUT */
} xhci_device_t;

/* ── Host Controller state ─────────────────────────────────────────────
 *
 * The xHCI register space is divided into:
 *   - Capability Registers (base): read-only, describe controller features
 *   - Operational Registers (base + caplength): control the controller
 *   - Runtime Registers (base + RTSOFF): event ring management
 *   - Doorbell Registers (base + DBOFF): notify controller of new work
 *   - Port Registers (op + 0x400): one set per physical port
 */
#define MAX_DEVICES 8

typedef struct {
    volatile uint8_t  *base;    /* MMIO base address (from PCI BAR0) */
    volatile uint8_t  *op;      /* Operational registers */
    volatile uint8_t  *rt;      /* Runtime registers */
    volatile uint32_t *db;      /* Doorbell register array */
    volatile uint8_t  *ports;   /* Port register sets */

    uint8_t  max_slots;         /* Maximum device slots supported */
    uint8_t  max_ports;         /* Number of root hub ports */
    uint8_t  context_size;      /* Context structure size: 32 or 64 bytes */

    xhci_ring_t       cmd_ring;  /* Command Ring (software -> controller) */
    xhci_event_ring_t evt_ring;  /* Event Ring (controller -> software) */
    uint64_t          *dcbaa;    /* Device Context Base Address Array */

    xhci_device_t     devices[MAX_DEVICES];
    int               num_devices;
} xhci_t;

/* ── API ───────────────────────────────────────────────────────────── */

int  xhci_init(xhci_t *hc, uint32_t bar0);
int  xhci_scan_ports(xhci_t *hc);
int  xhci_setup_device(xhci_t *hc, int port, int speed);

xhci_trb_t *xhci_poll_event(xhci_t *hc);
uint8_t      xhci_wait_event(xhci_t *hc, uint8_t expected_type, xhci_trb_t *out);

void ring_init(xhci_ring_t *ring);
xhci_trb_t *ring_enqueue(xhci_ring_t *ring, uint32_t param_lo,
                          uint32_t param_hi, uint32_t status,
                          uint32_t control);

#endif
