/*
 * ant.h — ANT+ wireless protocol definitions and heart rate driver
 *
 * ANT+ is a wireless sensor network protocol by Garmin/Dynastream,
 * commonly used for sports sensors (heart rate monitors, speed/cadence
 * sensors, power meters). It operates at 2.4 GHz.
 *
 * The protocol is accessed through a Garmin USB stick that translates
 * USB bulk transfers into wireless ANT+ messages. This module defines:
 *
 *   - USB identifiers for Garmin ANT+ dongles (VID/PID)
 *   - ANT+ protocol constants (message types, channel parameters)
 *   - Heart rate device profile settings (from the ANT+ standard)
 *   - Driver state and API functions
 *
 * The constants and initialization sequence are ported from tapinoma,
 * a Node.js ANT+ heart rate reader: https://github.com/loicguillois/tapinoma
 *
 * ANT+ message format:
 *   [SYNC=0xA4] [LEN] [MSG_ID] [PAYLOAD...] [CHECKSUM]
 *   where CHECKSUM = XOR of all preceding bytes
 */

#ifndef ANT_H
#define ANT_H

#include <stdint.h>
#include "xhci.h"
#include "usb.h"

/* ── Garmin ANT+ USB stick identifiers ─────────────────────────────── *
 *
 * Garmin makes several USB ANT+ sticks. They all share the same
 * vendor ID (Dynastream Innovations, a Garmin subsidiary) but have
 * different product IDs:
 *
 *   0x1008 = USB-m ANT Stick (ANTUSB2, older model)
 *   0x1009 = USB ANT Stick 3 (newer, more common)
 *
 * We try both PIDs during device discovery.
 */
#define GARMIN_VID          0x0FCF
#define GARMIN_STICK2_PID   0x1008
#define GARMIN_STICK3_PID   0x1009

/* ── ANT+ protocol constants (from tapinoma/src/constants.js) ──────── *
 *
 * ANT+ messages always start with the sync byte 0xA4.
 * The message type (MSG_ID) tells the ANT+ radio what to do.
 */
#define ANT_SYNC                  0xA4

/* Message types — sent from host to ANT+ stick (commands) */
#define ANT_MSG_SYSTEM_RESET      0x4A  /* Reset the ANT+ radio */
#define ANT_MSG_NETWORK_KEY       0x46  /* Set the 8-byte network key */
#define ANT_MSG_ASSIGN_CHANNEL    0x42  /* Assign a channel type */
#define ANT_MSG_CHANNEL_ID        0x51  /* Set device type to search for */
#define ANT_MSG_SEARCH_TIMEOUT    0x44  /* How long to search (x 2.5s) */
#define ANT_MSG_CHANNEL_PERIOD    0x43  /* Message period (in 32768Hz ticks) */
#define ANT_MSG_CHANNEL_FREQUENCY 0x45  /* RF frequency offset from 2400 MHz */
#define ANT_MSG_CHANNEL_OPEN      0x4B  /* Start receiving on this channel */
#define ANT_MSG_CHANNEL_CLOSE     0x4C  /* Stop receiving */

/* Message types — received from ANT+ stick (data/events) */
#define ANT_MSG_BROADCAST_DATA    0x4E  /* Standard broadcast data page */
#define ANT_MSG_ACKNOWLEDGED_DATA 0x4F  /* Acknowledged data transfer */
#define ANT_MSG_BURST_DATA        0x50  /* Burst data transfer */
#define ANT_MSG_CHANNEL_REQUEST   0x4D  /* Request specific channel info */
#define ANT_MSG_CHANNEL_EVENT     0x40  /* Channel event/response */
#define ANT_MSG_CAPABILITIES      0x54  /* Device capabilities report */

/* Channel types */
#define ANT_CH_RECEIVE            0x00  /* Receive-only (slave) channel */

/* ── Heart rate device profile ─────────────────────────────────────── *
 *
 * ANT+ defines standard "device profiles" for each sensor type.
 * The heart rate monitor profile specifies:
 *
 *   Device Type: 120
 *     Identifies this as a heart rate monitor in the ANT+ network.
 *
 *   Channel Period: 8070 (in 32768 Hz ticks)
 *     = 32768 / 8070 ≈ 4.06 Hz → one message every ~246 ms
 *
 *   RF Frequency: 57
 *     Actual frequency = 2400 + 57 = 2457 MHz
 *
 * These values come from the ANT+ Heart Rate Monitor Device Profile
 * and are also used in tapinoma/src/constants.js.
 */
#define ANT_DEVICE_HR             120
#define ANT_HR_PERIOD             8070
#define ANT_HR_FREQUENCY          57

/* ANT+ public network key — required to join the ANT+ network.
 * This key is published by Garmin for ANT+ device interoperability.
 * (from tapinoma/src/messages.js setNetworkKey) */
extern const uint8_t ANT_NETWORK_KEY[8];

/* ── ANT+ driver state ─────────────────────────────────────────────── *
 *
 * Tracks the USB connection to the ANT+ stick and the last received
 * heart rate value. A single driver instance handles one stick.
 */
typedef struct {
    xhci_t        *hc;         /* xHCI host controller */
    xhci_device_t *dev;        /* USB device (the ANT+ stick) */
    usb_endpoints_t eps;       /* Bulk IN/OUT endpoint addresses */
    uint8_t        last_hr;    /* Last received heart rate (BPM) */
    int            connected;  /* 1 = actively receiving HR data */
} ant_state_t;

/* ── API ───────────────────────────────────────────────────────────── */

/*
 * Initialize the ANT+ stick and configure a heart rate channel.
 *
 * This replicates the tapinoma attach() sequence:
 *   1. Read USB descriptors, find bulk endpoints
 *   2. SET_CONFIGURATION to activate the USB interface
 *   3. Configure xHCI endpoint contexts for bulk IN/OUT
 *   4. Send ANT+ init commands: reset → network key → channel setup → open
 *
 * Returns 0 on success, negative error code on failure.
 */
int ant_init(ant_state_t *ant, xhci_t *hc, xhci_device_t *dev);

/*
 * Poll for incoming ANT+ heart rate data. Non-blocking.
 *
 * Reads one USB bulk IN transfer and checks if it contains a heart
 * rate broadcast message. ANT+ HR data is in broadcast messages
 * (type 0x4E) with the computed heart rate at byte offset 7 of
 * the ANT+ channel data (byte 11 of the raw USB buffer).
 *
 * Returns the heart rate in BPM if available, 0 otherwise.
 */
uint8_t ant_poll_heart_rate(ant_state_t *ant);

#endif
