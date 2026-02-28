/*
 * xhci.c — xHCI USB host controller driver
 *
 * This driver implements the minimal subset of the xHCI specification
 * needed to enumerate USB devices and perform bulk transfers (for ANT+).
 *
 * The xHCI controller communicates with software entirely through shared
 * memory. There are no I/O ports involved — everything is memory-mapped
 * I/O (MMIO) at the address specified by PCI BAR0.
 *
 * Key concepts:
 *   - Rings: circular buffers of TRBs, used for commands, transfers, and events
 *   - Contexts: per-device memory structures describing slot and endpoint config
 *   - Doorbells: MMIO registers that notify the controller of new work
 *   - DCBAA: an array of pointers to device output contexts
 *
 * Initialization sequence (xHCI spec section 4.2):
 *   1. Wait for Controller Not Ready (CNR) to clear
 *   2. Stop the controller (clear Run/Stop bit)
 *   3. Reset the controller (set HCRST bit)
 *   4. Configure MaxSlotsEn, DCBAAP, CRCR
 *   5. Set up the Event Ring (ERST, ERDP, ERSTBA)
 *   6. Start the controller (set Run/Stop + INTE bits)
 *
 * Reference: xHCI Specification 1.2, Intel
 */

#include "xhci.h"
#include "alloc.h"
#include "string.h"
#include "vga.h"

/* ── MMIO helpers ──────────────────────────────────────────────────────
 *
 * The xHCI controller's registers are accessed through memory-mapped I/O.
 * We must use volatile pointers to prevent the compiler from caching reads
 * or reordering writes — hardware registers can change at any time.
 *
 * WR64 writes a 64-bit value as two 32-bit writes (low then high), which
 * is required on 32-bit systems where 64-bit stores aren't atomic.
 */

#define RD32(base, off)       (*(volatile uint32_t *)((base) + (off)))
#define WR32(base, off, val)  (*(volatile uint32_t *)((base) + (off)) = (val))
#define WR64(base, off, val)  do {                                    \
    *(volatile uint32_t *)((base) + (off))     = (uint32_t)(val);     \
    *(volatile uint32_t *)((base) + (off) + 4) = (uint32_t)((val) >> 32); \
} while (0)

/* ── Operational register offsets (from the start of the Operational space) ──
 *
 * These are added to hc->op (which is base + caplength).
 * See xHCI spec Table 5-18.
 */
#define USBCMD   0x00   /* USB Command Register: Run/Stop, Reset, INTE */
#define USBSTS   0x04   /* USB Status Register: HCHalted, CNR, errors */
#define CRCR     0x18   /* Command Ring Control Register (64-bit): ring pointer */
#define DCBAAP   0x30   /* Device Context Base Address Array Pointer (64-bit) */
#define CONFIG   0x38   /* Configure Register: MaxSlotsEn */

/* ── PORTSC (Port Status and Control) bit definitions ──────────────────
 *
 * Each port has a 32-bit PORTSC register at (op + 0x400 + port * 0x10).
 * Key bits:
 *   Bit  0: CCS  (Current Connect Status) — 1 if a device is attached
 *   Bit  1: PED  (Port Enabled/Disabled) — 1 if port is operational
 *   Bit  4: PR   (Port Reset) — write 1 to initiate a port reset
 *   Bits 5-8: PLS (Port Link State) — current link state
 *   Bits 10-13: Speed — negotiated device speed after reset
 *   Bit 17: CSC  (Connect Status Change) — RW1C, clear by writing 1
 *   Bit 21: PRC  (Port Reset Change) — RW1C, set when reset completes
 *
 * PORTSC_PRESERVE masks the Read-Write bits so we don't accidentally
 * clear the RW1C (Read-Write-1-to-Clear) status bits when modifying
 * other fields.
 */
#define PORTSC_CCS     (1 <<  0)
#define PORTSC_PED     (1 <<  1)
#define PORTSC_PR      (1 <<  4)
#define PORTSC_PLS_MASK (0xF << 5)
#define PORTSC_SPEED(x) (((x) >> 10) & 0xF)
#define PORTSC_CSC     (1 << 17)
#define PORTSC_PRC     (1 << 21)
#define PORTSC_PRESERVE 0x0E01C3E0u

/* ── Ring management ───────────────────────────────────────────────────
 *
 * A ring is a circular array of TRBs. The last entry is always a
 * Link TRB that points back to the first entry. When the hardware
 * (or software) reaches the Link TRB, it follows the pointer back
 * to index 0 and toggles the cycle bit.
 *
 * The cycle bit is the synchronization mechanism:
 *   - Software sets the cycle bit to the ring's current cycle state
 *   - Hardware only processes TRBs whose cycle bit matches its expectation
 *   - When the ring wraps, both sides toggle their cycle state
 */

void ring_init(xhci_ring_t *ring) {
    /* Allocate a physically contiguous, 64-byte aligned TRB array */
    ring->trbs = alloc_phys(RING_SIZE * sizeof(xhci_trb_t), 64);
    ring->enqueue = 0;
    ring->cycle = 1;  /* Cycle bit starts at 1 (xHCI convention) */

    /* Set up the Link TRB at the last slot.
     * It points back to trbs[0] and has the Toggle Cycle (TC) flag set,
     * which means the cycle bit flips when the controller follows this link. */
    xhci_trb_t *link = &ring->trbs[RING_SIZE - 1];
    link->param_lo = (uint32_t)(uintptr_t)ring->trbs;  /* Target address */
    link->param_hi = 0;
    link->status   = 0;
    link->control  = (TRB_LINK << 10) | (1 << 1);  /* Type=Link, TC=1 */
}

/*
 * ring_enqueue() — Add a TRB to the ring at the current enqueue position.
 *
 * The caller provides the TRB fields, and we stamp the correct cycle bit
 * into bit 0 of the control field. If we've filled all usable slots
 * (RING_SIZE - 1, since the last one is the Link TRB), we update the
 * Link TRB's cycle bit and wrap back to index 0, toggling the cycle.
 */
xhci_trb_t *ring_enqueue(xhci_ring_t *ring, uint32_t param_lo,
                          uint32_t param_hi, uint32_t status,
                          uint32_t control) {
    xhci_trb_t *trb = &ring->trbs[ring->enqueue];

    trb->param_lo = param_lo;
    trb->param_hi = param_hi;
    trb->status   = status;
    /* Clear bit 0 from the caller's control value, then OR in our cycle bit */
    trb->control  = (control & ~1u) | (ring->cycle & 1);

    ring->enqueue++;
    if (ring->enqueue >= RING_SIZE - 1) {
        /* We've reached the Link TRB slot — update its cycle bit so the
         * controller will follow it, then wrap our index back to 0. */
        ring->trbs[RING_SIZE - 1].control =
            (ring->trbs[RING_SIZE - 1].control & ~1u) | (ring->cycle & 1);
        ring->enqueue = 0;
        ring->cycle ^= 1;  /* Toggle: 1→0 or 0→1 */
    }

    return trb;
}

/* ── Event Ring ────────────────────────────────────────────────────────
 *
 * The Event Ring is different from command/transfer rings:
 *   - The CONTROLLER is the producer (writes events)
 *   - SOFTWARE is the consumer (reads events)
 *   - There is no Link TRB — wrapping is handled by the segment table
 *
 * The Event Ring Segment Table (ERST) tells the controller where the
 * event TRB array is and how large it is. After reading an event,
 * software must update the Event Ring Dequeue Pointer (ERDP) register
 * to tell the controller it has consumed the event.
 *
 * Interrupter 0 registers (at runtime base + 0x20):
 *   +0x00: IMAN  — Interrupt Management (enable/pending)
 *   +0x08: ERSTSZ — Event Ring Segment Table Size
 *   +0x10: ERSTBA — Event Ring Segment Table Base Address
 *   +0x18: ERDP   — Event Ring Dequeue Pointer
 */

static void event_ring_init(xhci_t *hc) {
    xhci_event_ring_t *er = &hc->evt_ring;

    /* Allocate the event TRB array and a single-entry segment table */
    er->trbs = alloc_phys(EVENT_RING_SIZE * sizeof(xhci_trb_t), 64);
    er->erst = alloc_phys(sizeof(xhci_erst_entry_t), 64);
    er->dequeue = 0;
    er->cycle = 1;

    /* Fill in the segment table: one segment covering all entries */
    er->erst[0].base_lo = (uint32_t)(uintptr_t)er->trbs;
    er->erst[0].base_hi = 0;
    er->erst[0].size    = EVENT_RING_SIZE;

    /* Program Interrupter 0 registers */
    volatile uint8_t *ir0 = hc->rt + 0x20;

    WR32(ir0, 0x08, 1);  /* ERSTSZ = 1 segment */

    /* ERDP: initial dequeue pointer, with EHB (Event Handler Busy) bit 3 set
     * to acknowledge any pending events */
    WR32(ir0, 0x18, (uint32_t)(uintptr_t)er->trbs | (1 << 3));
    WR32(ir0, 0x1C, 0);  /* High 32 bits */

    /* ERSTBA: must be written AFTER ERSTSZ (xHCI spec requirement) */
    WR32(ir0, 0x10, (uint32_t)(uintptr_t)er->erst);
    WR32(ir0, 0x14, 0);

    /* IMAN: set IE (Interrupt Enable) bit to allow event generation */
    RD32(ir0, 0x00);
    WR32(ir0, 0x00, RD32(ir0, 0x00) | (1 << 1));
}

/*
 * xhci_poll_event() — Check if a new event is available on the event ring.
 *
 * Returns a pointer to the event TRB if one is ready, NULL otherwise.
 * An event is "ready" when its cycle bit matches our expected cycle state.
 *
 * After consuming an event, we advance the dequeue pointer and write
 * the new ERDP to the controller, with the EHB bit set.
 */
xhci_trb_t *xhci_poll_event(xhci_t *hc) {
    xhci_event_ring_t *er = &hc->evt_ring;
    xhci_trb_t *trb = &er->trbs[er->dequeue];

    /* Check cycle bit: if it doesn't match, no new event yet */
    if ((trb->control & 1) != er->cycle)
        return (void *)0;

    /* Advance dequeue index, toggle cycle on wrap */
    er->dequeue++;
    if (er->dequeue >= EVENT_RING_SIZE) {
        er->dequeue = 0;
        er->cycle ^= 1;
    }

    /* Tell the controller we've consumed this event by updating ERDP */
    volatile uint8_t *ir0 = hc->rt + 0x20;
    uint32_t new_erdp = (uint32_t)(uintptr_t)&er->trbs[er->dequeue];
    WR32(ir0, 0x18, new_erdp | (1 << 3));  /* EHB = 1 */
    WR32(ir0, 0x1C, 0);

    return trb;
}

/*
 * xhci_wait_event() — Busy-wait for a specific event type.
 *
 * Polls the event ring until an event matching expected_type arrives,
 * or until the timeout counter expires (roughly a few seconds).
 *
 * Non-matching events (e.g., Port Status Change events while waiting
 * for a Command Completion) are consumed and discarded.
 *
 * Returns the completion code (CC) from the event, or 0xFF on timeout.
 */
uint8_t xhci_wait_event(xhci_t *hc, uint8_t expected_type, xhci_trb_t *out) {
    xhci_trb_t *ev;
    for (int timeout = 0; timeout < 5000000; timeout++) {
        ev = xhci_poll_event(hc);
        if (ev) {
            uint8_t type = (ev->control >> 10) & 0x3F;  /* TRB type field */
            uint8_t cc = (ev->status >> 24) & 0xFF;      /* Completion code */
            if (out) *out = *ev;   /* Copy event to caller's buffer */
            if (type == expected_type)
                return cc;
            /* Otherwise: wrong event type, discard and keep polling */
        }
    }
    return 0xFF;  /* Timeout — no matching event received */
}

/* ── Command Ring helpers ──────────────────────────────────────────────
 *
 * To send a command to the controller:
 *   1. Enqueue a TRB on the command ring
 *   2. Ring doorbell 0 with target 0 (Host Controller Command)
 *
 * The controller processes the command and posts a Command Completion
 * Event on the event ring with the result.
 */

static void xhci_command(xhci_t *hc, uint32_t param_lo, uint32_t param_hi,
                          uint32_t status, uint32_t control) {
    ring_enqueue(&hc->cmd_ring, param_lo, param_hi, status, control);
    hc->db[0] = 0;  /* Ring doorbell 0, target 0 */
}

/* ── Controller initialization (xHCI spec section 4.2) ─────────────── */

int xhci_init(xhci_t *hc, uint32_t bar0) {
    memset(hc, 0, sizeof(xhci_t));
    hc->base = (volatile uint8_t *)(uintptr_t)bar0;

    /* ── Read Capability Registers ─────────────────────────────────────
     *
     * The first byte (CAPLENGTH) tells us the size of the capability area,
     * which varies between controllers. Operational registers start right
     * after at base + caplength.
     *
     * HCSPARAMS1 contains the number of ports and device slots.
     * HCCPARAMS1 tells us the context size (32 or 64 bytes).
     * DBOFF and RTSOFF give the offsets to doorbell and runtime registers.
     */
    uint8_t  caplength  = *(volatile uint8_t *)(hc->base + 0x00);
    uint32_t hcsparams1 = RD32(hc->base, 0x04);
    uint32_t hccparams1 = RD32(hc->base, 0x10);
    uint32_t dboff      = RD32(hc->base, 0x14);
    uint32_t rtsoff     = RD32(hc->base, 0x18);

    /* Compute register base addresses */
    hc->op    = hc->base + caplength;
    hc->rt    = hc->base + (rtsoff & ~0x1Fu);       /* 32-byte aligned */
    hc->db    = (volatile uint32_t *)(hc->base + (dboff & ~0x3u));  /* 4-byte aligned */
    hc->ports = hc->op + 0x400;  /* Port registers start at operational + 0x400 */

    /* Extract controller capabilities */
    hc->max_slots    = hcsparams1 & 0xFF;          /* Bits 7:0 */
    hc->max_ports    = (hcsparams1 >> 24) & 0xFF;  /* Bits 31:24 */
    hc->context_size = (hccparams1 & (1 << 2)) ? 64 : 32;  /* CSZ flag */

    /* ── Step 1: Wait for Controller Not Ready (CNR) to clear ──────── */
    while (RD32(hc->op, USBSTS) & (1 << 11))
        ;

    /* ── Step 2: Stop the controller ───────────────────────────────── */
    uint32_t cmd = RD32(hc->op, USBCMD);
    cmd &= ~1u;  /* Clear Run/Stop bit */
    WR32(hc->op, USBCMD, cmd);
    while (!(RD32(hc->op, USBSTS) & 1))  /* Wait for HCHalted = 1 */
        ;

    /* ── Step 3: Reset the controller ──────────────────────────────── */
    WR32(hc->op, USBCMD, (1 << 1));  /* Set HCRST (Host Controller Reset) */
    while (RD32(hc->op, USBCMD) & (1 << 1))  /* Wait for HCRST to self-clear */
        ;
    while (RD32(hc->op, USBSTS) & (1 << 11))  /* Wait for CNR to clear */
        ;

    /* ── Step 4: Configure MaxSlotsEn ──────────────────────────────── */
    uint8_t slots = hc->max_slots;
    if (slots > MAX_DEVICES) slots = MAX_DEVICES;
    WR32(hc->op, CONFIG, slots);

    /* ── Step 5: Set up the Device Context Base Address Array ──────── *
     *
     * The DCBAA is an array of 64-bit pointers, one per device slot
     * (plus slot 0 for the scratchpad). When we address a device,
     * we store its output context pointer in dcbaa[slot_id].
     */
    hc->dcbaa = alloc_phys((slots + 1) * sizeof(uint64_t), 64);
    WR64(hc->op, DCBAAP, (uint64_t)(uintptr_t)hc->dcbaa);

    /* ── Step 6: Set up the Command Ring ───────────────────────────── *
     *
     * The CRCR register tells the controller where the command ring is.
     * Bit 0 is the Ring Cycle State (RCS) — must match the ring's initial cycle.
     */
    ring_init(&hc->cmd_ring);
    uint64_t crcr = (uint64_t)(uintptr_t)hc->cmd_ring.trbs | hc->cmd_ring.cycle;
    WR64(hc->op, CRCR, crcr);

    /* ── Step 7: Set up the Event Ring ─────────────────────────────── */
    event_ring_init(hc);

    /* ── Step 8: Start the controller ──────────────────────────────── *
     *
     * Set Run/Stop (bit 0) and Interrupter Enable (INTE, bit 2).
     * Then wait for HCHalted (USBSTS bit 0) to clear, confirming
     * the controller is running.
     */
    cmd = RD32(hc->op, USBCMD);
    cmd |= (1 << 0) | (1 << 2);  /* Run + INTE */
    WR32(hc->op, USBCMD, cmd);
    while (RD32(hc->op, USBSTS) & 1)  /* Wait for HCHalted = 0 */
        ;

    return 0;
}

/* ── Port scanning ─────────────────────────────────────────────────── */

/* Get a pointer to the PORTSC register for a given port.
 * Each port's register set is 0x10 bytes apart. */
static volatile uint32_t *portsc_reg(xhci_t *hc, int port) {
    return (volatile uint32_t *)(hc->ports + port * 0x10);
}

/* Busy-wait delay for port operations (reset settling time) */
static void port_delay(void) {
    for (volatile int i = 0; i < 2000000; i++)
        ;
}

/*
 * reset_port() — Perform a USB port reset and return the negotiated speed.
 *
 * The port reset sequence:
 *   1. Verify CCS (device present)
 *   2. Clear any pending status change bits (CSC, PRC) — these are RW1C
 *   3. Write PR=1 (Port Reset) to initiate reset signaling
 *   4. Wait for PRC (Port Reset Change) — set by hardware when reset completes
 *   5. Clear PRC
 *   6. Verify PED (Port Enabled) — confirms device is ready
 *   7. Read the negotiated speed from bits 13:10
 *
 * Returns speed (1=Full, 2=Low, 3=High, 4=Super) or -1 on failure.
 */
static int reset_port(xhci_t *hc, int port) {
    volatile uint32_t *portsc = portsc_reg(hc, port);
    uint32_t val = *portsc;

    if (!(val & PORTSC_CCS))
        return -1;  /* No device connected */

    /* Clear pending status change bits (write 1 to clear RW1C bits) */
    *portsc = (val & PORTSC_PRESERVE) | PORTSC_CSC | PORTSC_PRC;
    port_delay();

    /* Initiate port reset */
    val = *portsc;
    *portsc = (val & PORTSC_PRESERVE) | PORTSC_PR;

    /* Wait for reset to complete (PRC bit gets set by hardware) */
    for (int i = 0; i < 5000000; i++) {
        val = *portsc;
        if (val & PORTSC_PRC) break;
    }

    if (!(val & PORTSC_PRC))
        return -1;  /* Reset never completed (timeout) */

    /* Acknowledge the reset completion by clearing PRC */
    *portsc = (*portsc & PORTSC_PRESERVE) | PORTSC_PRC;
    port_delay();

    /* After reset, the port should be enabled */
    val = *portsc;
    if (!(val & PORTSC_PED))
        return -1;  /* Port not enabled after reset */

    return PORTSC_SPEED(val);
}

/*
 * xhci_scan_ports() — Detect and set up all connected USB devices.
 *
 * Uses a two-pass strategy:
 *
 *   Pass 1: Handle ports that are already enabled (PED=1).
 *           This is common with QEMU USB passthrough, where the virtual
 *           controller may have already completed link training.
 *           No reset is needed — we read the speed directly from PORTSC.
 *
 *   Pass 2: Only if Pass 1 found nothing, try resetting ports that have
 *           CCS=1 (device present) but PED=0 (not yet enabled).
 *           Some of these may be "phantom" ports (e.g., PLS=7 Polling state)
 *           that will fail reset — this is expected and harmless.
 *
 * Before sending any commands, we drain pending events from the event ring
 * to avoid confusion from stale Port Status Change events.
 */
int xhci_scan_ports(xhci_t *hc) {
    int found = 0;

    if (hc->num_devices > 0)
        return hc->num_devices;

    /* Drain any pending events from previous port activity */
    while (xhci_poll_event(hc))
        ;

    /* Pass 1: ports already enabled (PED=1) — no reset needed */
    for (int i = 0; i < hc->max_ports && hc->num_devices < MAX_DEVICES; i++) {
        uint32_t val = *portsc_reg(hc, i);
        if ((val & PORTSC_CCS) && (val & PORTSC_PED)) {
            int speed = PORTSC_SPEED(val);
            vga_print("P"); vga_print_int(i);
            vga_print(":PED spd="); vga_print_int(speed);
            if (speed > 0) {
                vga_print(" SETUP");
                int ret = xhci_setup_device(hc, i, speed);
                vga_print("="); vga_print_int(ret);
                if (ret == 0)
                    found++;
            }
            vga_newline();
        }
    }

    if (found > 0)
        return found;

    /* Pass 2: ports with CCS but not enabled — attempt reset */
    for (int i = 0; i < hc->max_ports && hc->num_devices < MAX_DEVICES; i++) {
        uint32_t val = *portsc_reg(hc, i);
        if ((val & PORTSC_CCS) && !(val & PORTSC_PED)) {
            vga_print("P"); vga_print_int(i);
            vga_print(":RST");
            int speed = reset_port(hc, i);
            vga_print(" spd="); vga_print_int(speed);
            if (speed > 0) {
                vga_print(" SETUP");
                int ret = xhci_setup_device(hc, i, speed);
                vga_print("="); vga_print_int(ret);
                if (ret == 0)
                    found++;
            }
            vga_newline();
        }
    }

    return found;
}

/* ── Device setup (Enable Slot → Address Device) ──────────────────────
 *
 * Setting up a USB device on xHCI requires two commands:
 *
 * 1. Enable Slot: asks the controller to allocate a "slot" (numbered 1..N).
 *    The controller returns the slot ID in the Command Completion Event.
 *
 * 2. Address Device: assigns a USB address to the device. We must first
 *    prepare an Input Context with:
 *    - Input Control Context: which contexts we're adding (Slot + EP0)
 *    - Slot Context: speed, port number, context entries count
 *    - EP0 Context: max packet size, transfer ring pointer, EP type
 *
 *    The controller reads our input context, creates the output context,
 *    and sends a SET_ADDRESS request to the physical USB device.
 */

int xhci_setup_device(xhci_t *hc, int port, int speed) {
    xhci_trb_t ev;

    /* Verify the controller is still running (not halted due to error) */
    uint32_t sts = RD32(hc->op, USBSTS);
    vga_print("[STS="); vga_print_int(sts); vga_print("]");
    if (sts & 1) return -1;  /* HCHalted — controller stopped */

    /* Drain stale events to ensure clean command/response pairing */
    while (xhci_poll_event(hc))
        ;

    /* ── Enable Slot command ───────────────────────────────────────── */
    xhci_command(hc, 0, 0, 0, (TRB_ENABLE_SLOT << 10));
    uint8_t cc = xhci_wait_event(hc, TRB_CMD_COMPLETION, &ev);
    vga_print("[ES cc="); vga_print_int(cc); vga_print("]");
    if (cc != CC_SUCCESS)
        return -1;

    /* The slot ID is returned in bits 31:24 of the event's control field */
    int slot_id = (ev.control >> 24) & 0xFF;
    vga_print("[slot="); vga_print_int(slot_id); vga_print("]");
    if (slot_id == 0 || slot_id > hc->max_slots)
        return -1;

    uint32_t csz = hc->context_size;  /* 32 or 64 bytes per context */
    xhci_device_t *dev = &hc->devices[hc->num_devices];
    memset(dev, 0, sizeof(xhci_device_t));
    dev->slot_id = slot_id;
    dev->port = port;
    dev->speed = speed;

    /* Allocate Output Device Context (32 contexts × csz bytes each).
     * Register its address in the DCBAA so the controller can find it. */
    dev->output_ctx = alloc_phys(32 * csz, 64);
    hc->dcbaa[slot_id] = (uint64_t)(uintptr_t)dev->output_ctx;

    /* Allocate Input Context (33 entries: 1 Input Control + 32 contexts).
     * The first entry (Input Control Context) tells the controller which
     * contexts we're configuring. */
    dev->input_ctx = alloc_phys(33 * csz, 64);
    uint32_t *icc = (uint32_t *)dev->input_ctx;
    icc[1] = (1 << 0) | (1 << 1);  /* Add Context flags: Slot (0) + EP0 (1) */

    /* ── Slot Context setup ────────────────────────────────────────── *
     *
     * field[0] bits 31:27 = Context Entries (1 = only EP0)
     *          bits 23:20 = Speed (1=Full, 2=Low, 3=High, 4=Super)
     * field[1] bits 23:16 = Root Hub Port Number (1-based!)
     */
    xhci_slot_ctx_t *slot_ctx = (xhci_slot_ctx_t *)(dev->input_ctx + csz);
    slot_ctx->field[0] = (1u << 27) | ((uint32_t)speed << 20);
    slot_ctx->field[1] = ((uint32_t)(port + 1)) << 16;

    /* ── EP0 (Default Control Endpoint) setup ──────────────────────── *
     *
     * Every USB device has EP0 for control transfers (GET_DESCRIPTOR, etc.).
     * We need a transfer ring for EP0 and an endpoint context describing it.
     *
     * Max packet size depends on USB speed:
     *   Full Speed (1):  8 bytes
     *   Low Speed (2):   8 bytes
     *   High Speed (3):  64 bytes
     *   Super Speed (4): 512 bytes
     */
    ring_init(&dev->ep_ring[0]);

    xhci_ep_ctx_t *ep0 = (xhci_ep_ctx_t *)(dev->input_ctx + 2 * csz);

    uint16_t max_pkt;
    switch (speed) {
        case 2:  max_pkt = 8;   break;  /* Low Speed   */
        case 1:  max_pkt = 8;   break;  /* Full Speed  */
        case 3:  max_pkt = 64;  break;  /* High Speed  */
        case 4:  max_pkt = 512; break;  /* Super Speed */
        default: max_pkt = 8;   break;
    }

    /* EP0 context field1:
     *   Bits 2:1  = CErr (Completion Error retry count) = 3
     *   Bits 5:3  = EP Type = 4 (Control, bidirectional)
     *   Bits 31:16 = Max Packet Size */
    ep0->field1 = (3 << 1)
                | (4 << 3)
                | ((uint32_t)max_pkt << 16);
    /* Transfer ring dequeue pointer, with DCS (Dequeue Cycle State) = 1 */
    ep0->tr_dequeue_lo = (uint32_t)(uintptr_t)dev->ep_ring[0].trbs | 1;
    ep0->tr_dequeue_hi = 0;
    ep0->field4 = 8;  /* Average TRB Length (hint for controller scheduling) */

    /* ── Address Device command ────────────────────────────────────── *
     *
     * This command tells the controller to:
     *   1. Read our input context
     *   2. Send a SET_ADDRESS request to the physical USB device
     *   3. Transition the device slot from Enabled to Addressed state
     *
     * param_lo = pointer to Input Context
     * control bits 31:24 = Slot ID
     */
    xhci_command(hc, (uint32_t)(uintptr_t)dev->input_ctx, 0, 0,
                 (TRB_ADDRESS_DEVICE << 10) | ((uint32_t)slot_id << 24));
    cc = xhci_wait_event(hc, TRB_CMD_COMPLETION, &ev);
    vga_print("[AD cc="); vga_print_int(cc); vga_print("]");
    if (cc != CC_SUCCESS)
        return -1;

    hc->num_devices++;
    return 0;
}
