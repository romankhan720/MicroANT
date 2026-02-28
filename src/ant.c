/*
 * ant.c — ANT+ wireless protocol implementation for heart rate monitoring
 *
 * This module drives a Garmin USB ANT+ stick to receive heart rate
 * data from a wireless chest strap or arm band sensor.
 *
 * The implementation is a bare-metal C port of tapinoma, a Node.js
 * ANT+ heart rate reader: https://github.com/loicguillois/tapinoma
 *
 * Communication flow:
 *   1. USB bulk OUT: host sends ANT+ command messages to the stick
 *   2. The stick transmits/receives over the 2.4 GHz ANT+ radio
 *   3. USB bulk IN: stick sends received ANT+ data back to the host
 *
 * ANT+ message format (from tapinoma/src/utils.js):
 *   [0xA4] [LEN] [MSG_ID] [PAYLOAD...] [CHECKSUM]
 *   CHECKSUM = XOR of all bytes including sync, length, and msg_id
 *
 * Initialization sequence (from tapinoma/src/driver.js attach()):
 *   1. Reset the ANT+ radio
 *   2. Set the ANT+ public network key
 *   3. Assign channel 0 as a receive (slave) channel
 *   4. Set channel ID: search for any HR device (device type 120)
 *   5. Set search timeout to maximum (255 × 2.5s ≈ 10 minutes)
 *   6. Set messaging period (8070 ticks → ~4 Hz)
 *   7. Set RF frequency (2457 MHz)
 *   8. Open channel → start receiving broadcasts
 */

#include "ant.h"
#include "string.h"

/* ANT+ public network key (from tapinoma/src/messages.js setNetworkKey).
 * This well-known key allows any ANT+ device to join the public network.
 * It's published by Garmin/Dynastream for interoperability. */
const uint8_t ANT_NETWORK_KEY[8] = {
    0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
};

/* ── ANT+ message construction ─────────────────────────────────────── *
 *
 * Every ANT+ message follows the same structure:
 *   Byte 0:     SYNC (always 0xA4)
 *   Byte 1:     LEN  (payload length, NOT including sync/len/id/checksum)
 *   Byte 2:     MSG_ID (message type)
 *   Bytes 3..N: PAYLOAD (LEN bytes)
 *   Byte N+1:   CHECKSUM (XOR of ALL preceding bytes)
 *
 * The checksum is simple: XOR every byte from sync through the last
 * payload byte. This catches single-bit errors in transmission.
 */

/* Build an ANT+ message in the provided buffer.
 * Returns the total message size (header + payload + checksum). */
static uint8_t ant_build_msg(uint8_t *buf, uint8_t msg_id,
                              const uint8_t *payload, uint8_t len) {
    buf[0] = ANT_SYNC;    /* Sync byte — marks the start of a message */
    buf[1] = len;          /* Payload length */
    buf[2] = msg_id;       /* Message type identifier */

    /* Compute checksum: start with header bytes, then XOR each payload byte */
    uint8_t checksum = buf[0] ^ buf[1] ^ buf[2];
    for (uint8_t i = 0; i < len; i++) {
        buf[3 + i] = payload[i];
        checksum ^= payload[i];
    }
    buf[3 + len] = checksum;

    return 4 + len;  /* total: sync(1) + len(1) + id(1) + payload(len) + checksum(1) */
}

/* Send an ANT+ message to the stick via USB bulk OUT transfer. */
static int ant_send(ant_state_t *ant, uint8_t msg_id,
                    const uint8_t *payload, uint8_t len) {
    uint8_t buf[32];
    uint8_t total = ant_build_msg(buf, msg_id, payload, len);
    return usb_bulk_out(ant->hc, ant->dev, &ant->eps, buf, total);
}

/* Small busy-wait delay between ANT+ commands.
 *
 * The ANT+ radio needs time to process each command before receiving
 * the next one. Without this delay, commands can be lost or the stick
 * may respond with errors. This is a simple busy loop since we have
 * no timer infrastructure in our bare-metal environment. */
static void ant_delay(void) {
    for (volatile int i = 0; i < 500000; i++)
        ;
}

/* ── ANT+ commands (mirroring tapinoma/src/messages.js) ────────────── *
 *
 * Each function below sends one ANT+ command message. They mirror
 * the message construction functions in tapinoma's messages.js module.
 */

/* Reset the ANT+ radio to its default state.
 * After reset, all channels are closed and all settings are cleared.
 * The stick needs extra time (~500ms) to reinitialize after a reset. */
static int ant_reset_system(ant_state_t *ant) {
    uint8_t payload[] = { 0x00 };   /* Filler byte (required by protocol) */
    int ret = ant_send(ant, ANT_MSG_SYSTEM_RESET, payload, 1);
    /* Extra long delay: the radio needs time to reinitialize after reset */
    for (volatile int i = 0; i < 2000000; i++)
        ;
    return ret;
}

/* Set the 8-byte network key for a network number.
 * ANT+ uses a public network key that allows all ANT+ devices to
 * communicate. The key must be set before any channel can operate.
 *   Payload: [network_number] [key_byte_0..7] = 9 bytes */
static int ant_set_network_key(ant_state_t *ant, uint8_t network) {
    uint8_t payload[9];
    payload[0] = network;                      /* Network number (0 = default) */
    memcpy(&payload[1], ANT_NETWORK_KEY, 8);   /* 8-byte network key */
    return ant_send(ant, ANT_MSG_NETWORK_KEY, payload, 9);
}

/* Assign a channel number and type (receive/transmit).
 * For heart rate monitoring, we use a receive-only (slave) channel.
 *   Payload: [channel] [type] [network] = 3 bytes */
static int ant_assign_channel(ant_state_t *ant, uint8_t channel, uint8_t type) {
    uint8_t payload[] = { channel, type, 0x00 };  /* network 0 */
    return ant_send(ant, ANT_MSG_ASSIGN_CHANNEL, payload, 3);
}

/* Set the channel ID: which device to search for.
 *   device_id = 0 means "any device" (wildcard search)
 *   device_type = 120 for heart rate monitors
 *   transmission_type = 0 means "any" (pairing mode)
 *   Payload: [channel] [device_id_lo] [device_id_hi] [device_type] [trans_type] = 5 bytes */
static int ant_set_device(ant_state_t *ant, uint8_t channel,
                           uint16_t device_id, uint8_t device_type,
                           uint8_t transmission_type) {
    uint8_t payload[] = {
        channel,
        (uint8_t)(device_id & 0xFF),    /* Device number low byte */
        (uint8_t)(device_id >> 8),      /* Device number high byte */
        device_type,                     /* 120 = heart rate monitor */
        transmission_type                /* 0 = wildcard (accept any) */
    };
    return ant_send(ant, ANT_MSG_CHANNEL_ID, payload, 5);
}

/* Set the search timeout: how long to look for a sensor.
 *   timeout is in units of 2.5 seconds. 255 = ~10 minutes (maximum).
 *   Payload: [channel] [timeout] = 2 bytes */
static int ant_search_timeout(ant_state_t *ant, uint8_t channel, uint8_t timeout) {
    uint8_t payload[] = { channel, timeout };
    return ant_send(ant, ANT_MSG_SEARCH_TIMEOUT, payload, 2);
}

/* Set the channel messaging period in units of 32768 Hz ticks.
 *   For heart rate: 8070 ticks → 32768/8070 ≈ 4.06 messages/second.
 *   Payload: [channel] [period_lo] [period_hi] = 3 bytes */
static int ant_set_period(ant_state_t *ant, uint8_t channel, uint16_t period) {
    uint8_t payload[] = {
        channel,
        (uint8_t)(period & 0xFF),     /* Period low byte */
        (uint8_t)(period >> 8)        /* Period high byte */
    };
    return ant_send(ant, ANT_MSG_CHANNEL_PERIOD, payload, 3);
}

/* Set the RF frequency as an offset from 2400 MHz.
 *   For heart rate: freq=57 → 2400+57 = 2457 MHz.
 *   Payload: [channel] [frequency] = 2 bytes */
static int ant_set_frequency(ant_state_t *ant, uint8_t channel, uint8_t freq) {
    uint8_t payload[] = { channel, freq };
    return ant_send(ant, ANT_MSG_CHANNEL_FREQUENCY, payload, 2);
}

/* Open the channel: start searching for and receiving data from sensors.
 *   After opening, the stick begins transmitting search requests at the
 *   configured frequency and period. Once a sensor is found, broadcast
 *   data messages (0x4E) will arrive on the USB bulk IN endpoint.
 *   Payload: [channel] = 1 byte */
static int ant_open_channel(ant_state_t *ant, uint8_t channel) {
    uint8_t payload[] = { channel };
    return ant_send(ant, ANT_MSG_CHANNEL_OPEN, payload, 1);
}

/* ── Init: replicate tapinoma attach() sequence ────────────────────── *
 *
 * This function performs the complete initialization of the ANT+ stick:
 *
 *   Phase 1 — USB setup:
 *     Read the configuration descriptor to discover bulk endpoints,
 *     activate the USB configuration, and tell the xHCI controller
 *     about the new endpoints.
 *
 *   Phase 2 — ANT+ channel configuration:
 *     Send the sequence of ANT+ commands that configures channel 0
 *     to receive heart rate broadcasts from any nearby sensor.
 *     This mirrors tapinoma/src/driver.js attach().
 */
int ant_init(ant_state_t *ant, xhci_t *hc, xhci_device_t *dev) {
    memset(ant, 0, sizeof(ant_state_t));
    ant->hc = hc;
    ant->dev = dev;

    /* ── Phase 1: USB endpoint setup ─────────────────────────────── */

    /* Read the full configuration descriptor tree to discover endpoints */
    uint8_t config_buf[256];
    if (usb_get_config_descriptor(hc, dev, config_buf, sizeof(config_buf)) != 0)
        return -1;

    /* Walk the descriptor tree to find the bulk IN and bulk OUT endpoints */
    if (usb_find_bulk_endpoints(config_buf,
            ((usb_config_desc_t *)config_buf)->wTotalLength, &ant->eps) != 0)
        return -2;

    /* Activate the USB configuration (SET_CONFIGURATION request) */
    usb_config_desc_t *cfg = (usb_config_desc_t *)config_buf;
    if (usb_set_configuration(hc, dev, cfg->bConfigurationValue) != 0)
        return -3;

    /* Tell the xHCI controller about the bulk endpoints:
     * allocate transfer rings and send a Configure Endpoint command */
    if (usb_configure_endpoints(hc, dev, &ant->eps) != 0)
        return -4;

    /* ── Phase 2: ANT+ channel configuration ─────────────────────── *
     *
     * The exact sequence from tapinoma/src/driver.js attach():
     *   1. Reset the radio (clears any previous state)
     *   2. Set the ANT+ public network key on network 0
     *   3. Assign channel 0 as a receive (slave) channel
     *   4. Set device search: any HR monitor (type 120, ID=0)
     *   5. Set search timeout to maximum (~10 minutes)
     *   6. Set channel period (8070 = ~4 Hz for HR profile)
     *   7. Set RF frequency (57 → 2457 MHz for HR profile)
     *   8. Open the channel → start searching for sensors
     *
     * A delay between each command gives the radio time to process. */

    ant_reset_system(ant);
    ant_delay();

    ant_set_network_key(ant, 0);
    ant_delay();

    ant_assign_channel(ant, 0, ANT_CH_RECEIVE);
    ant_delay();

    ant_set_device(ant, 0, 0, ANT_DEVICE_HR, 0);
    ant_delay();

    ant_search_timeout(ant, 0, 255);
    ant_delay();

    ant_set_period(ant, 0, ANT_HR_PERIOD);
    ant_delay();

    ant_set_frequency(ant, 0, ANT_HR_FREQUENCY);
    ant_delay();

    ant_open_channel(ant, 0);
    ant_delay();

    return 0;
}

/* ── Poll for heart rate data ──────────────────────────────────────── *
 *
 * Reads one ANT+ message from the USB bulk IN endpoint and checks
 * if it contains heart rate data.
 *
 * ANT+ heart rate broadcast message layout in the USB buffer:
 *
 *   Byte  0: SYNC (0xA4)
 *   Byte  1: LEN  (usually 9 for broadcast data)
 *   Byte  2: MSG_TYPE (0x4E = broadcast, 0x4F = acknowledged, 0x50 = burst)
 *   Byte  3: Channel number
 *   Bytes 4-11: ANT+ channel data (8 bytes of heart rate page data)
 *   Byte 12: Checksum
 *
 * Within the 8-byte channel data (bytes 4-11):
 *   Byte  4: Data page number
 *   Bytes 5-10: Page-specific data
 *   Byte 11: Computed Heart Rate (BPM) ← this is what we extract
 *
 * The computed heart rate is always at the last byte of the channel
 * data, regardless of which data page is being sent. This makes
 * parsing simple — we just read byte 11 of the USB buffer.
 *
 * (from tapinoma/src/driver.js: heartRate = hrmPayload.readUInt8(3)
 *  where hrmPayload = data.slice(BUFFER_INDEX_MSG_DATA + 4))
 */
uint8_t ant_poll_heart_rate(ant_state_t *ant) {
    uint8_t buf[64];
    int received = usb_bulk_in(ant->hc, ant->dev, &ant->eps, buf, sizeof(buf));

    /* Need at least 4 bytes for a minimal ANT+ message header */
    if (received < 4)
        return 0;

    uint8_t msg_type = buf[2];

    /* Check for data-carrying message types */
    if (msg_type == ANT_MSG_BROADCAST_DATA ||
        msg_type == ANT_MSG_ACKNOWLEDGED_DATA ||
        msg_type == ANT_MSG_BURST_DATA) {
        /* Computed Heart Rate is at byte offset 11 in the raw USB data */
        if (received >= 12) {
            uint8_t hr = buf[11];
            /* Sanity check: valid heart rates are between 1 and 249 BPM */
            if (hr > 0 && hr < 250) {
                ant->last_hr = hr;
                ant->connected = 1;
                return hr;
            }
        }
    }

    return 0;
}
