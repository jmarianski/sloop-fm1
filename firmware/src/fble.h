/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Jacek Mariański */
/* fble: the Bluetooth MIDI module's interface (ABI 0.3), as SLOOP hosts it (FELUCCA_FBLE, tools/build.py --fble).
 * fm1-base's include/fble.h of M5 (the diet) with SLOOP's RAM windows; the layouts (header, host, api, ring) are the
 * same.
 *
 * The module is a separate image (fm1-base's ble/: the shim and the MIDI service on JieLi's BT libraries, Apache-2.0),
 * written into the flash on its own (fble_push.py, SysEx "FB": fble_host.c) and run in place through the unencrypted
 * XIP window (fl_plain_window_init, from 0x93000 up). SLOOP links none of it: at boot it finds the header at the start
 * of segment 0, checks it (fble_load.h), copies the module's RAM parts into the windows below, which its own linker
 * script keeps free (firmware/app_fble.ld.in, filled in from these numbers by build.py), and calls init. Everything
 * after that goes through the two tables below: the host's services (fble_host) and the module's calls and rings
 * (fble_api). This file is the one place SLOOP's windows are written down: the linker script, the loader's check and
 * the module (linked for SLOOP: the same numbers in its own fble.h) all take them from here.
 *
 *   flash  segment 0: USR4 [0xE7000, 0xFB000), the header first: the whole module (80 KiB). No segment 1 (0.3; USR3
 *          is SLOOP's user samples). While a module is there, USR4 holds no sample (the editor's upload refuses slot 4)
 *   RAM    rt   [0x01C06F00, 0x01C07F00)   4 KiB  .ram_text (ISRs, delays): run from RAM, the guard's code window
 *                                                 widened to it; the top of SLOOP's RAMTEXT (.lowram ends below it,
 *                                                 crt0's STATUS area is above it)
 *          low  [0x01C08000, 0x01C0E000)  24 KiB  .data, .bss (the BT heap: the radio's DMA must reach it): the bottom
 *                                                 of SLOOP's RAM region, below its .data
 *          any  [0x01C1E000, 0x01C20000)   8 KiB  .pool (the BT tasks' stacks): the top of SLOOP's RAM region, above
 *                                                 its .bss, below the pool
 * The sizes are M5's (rt 4, low 24, any 8 KiB); the bases are SLOOP's: MIDIMON's (rt 0x01C06000, low 0x01C0E000,
 * any 0x01C6C000) are SLOOP's .lowram, .bss and pool. A module linked for MIDIMON's is refused ("RAM windows"): it is
 * linked again with these.
 * The module is linked for exactly these windows (no relocation in ABI 0): the host keeps them free, and the loader
 * refuses a header asking for anything else.
 *
 * 0.3 (M5): one flash segment, seg[1] all 0; the windows shrunk to what the module uses. 0.2: fble_host.tap. */
#pragma once
#include <stdint.h>

#define FBLE_MAGIC 0x454C4246u                    /* "FBLE", little-endian */
#define FBLE_ABI_MAJOR 0u                         /* another major: not loaded */
#define FBLE_ABI_MINOR 3u                         /* more minor: backwards compatible additions */

/* the flash: one segment (a flash offset; the CPU reads offset off at FM1_XIP(off) = 0x02000000 + off - 0x4000) */
#define FBLE_SEG0_OFF 0xE7000u                    /* USR4: the header, then code */
#define FBLE_SEG0_MAX 0x14000u                    /* 80 KiB */
/* the RAM windows (CPU addresses) */
#define FBLE_RT_LO 0x01C06F00u
#define FBLE_RT_HI 0x01C07F00u
#define FBLE_LOW_LO 0x01C08000u
#define FBLE_LOW_HI 0x01C0E000u
#define FBLE_ANY_LO 0x01C1E000u
#define FBLE_ANY_HI 0x01C20000u

/* a flash segment: where, how long, its CRC-32 (zlib's; segment 0's from the end of the header on) */
typedef struct { uint32_t off, size, crc; } fble_seg;
/* a step of the load: n bytes from src (a flash address, through XIP) to dst; src 0: zeros */
typedef struct { uint32_t dst, src, n; } fble_copy;
#define FBLE_NCOPY 4                              /* .ram_text, .data, .bss, .pool */

/* the header: the first bytes of segment 0 */
typedef struct {
    uint32_t magic;                               /* FBLE_MAGIC */
    uint16_t abi_major, abi_minor;
    uint32_t hdr_size;                            /* sizeof (fble_header) */
    uint32_t hdr_crc;                             /* CRC-32 of the header with this field 0 */
    fble_seg seg[2];                              /* seg[1]: all 0 (0.3: no segment 1; was USR3's) */
    uint32_t xip_lo, xip_hi;                      /* the code's addresses (the guard's window must cover them) */
    uint32_t rt_lo, rt_hi;                        /* the RAM windows it was linked for (FBLE_*_LO/HI or smaller) */
    uint32_t low_lo, low_hi;
    uint32_t any_lo, any_hi;
    fble_copy copy[FBLE_NCOPY];                   /* what the loader does before init */
    uint32_t init;                                /* const fble_api *init(const fble_host *, void *low, uint32_t
                                                   * low_n, void *any, uint32_t any_n): 0 = refused */
    uint32_t feat;                                /* FBLE_F_* */
    char name[16];                                /* "fble" */
    char version[16];                             /* "0.3-m5" */
} fble_header;
#define FBLE_F_PERIPHERAL 1u                      /* a computer can connect to the FM-1 */
#define FBLE_F_CENTRAL 2u                         /* the FM-1 connects to a Bluetooth MIDI device */

/* a ring of 4-byte packets, one producer and one consumer: w and r count up and wrap; the producer writes
 * buf[w % n], then w; the consumer reads buf[r % n], then r. USB-MIDI style packets (cable 0: the computer's link,
 * 1: the device's) */
typedef struct {
    volatile uint32_t w, r;
    uint32_t n;
    uint32_t *buf;
} fble_ring;

/* commands (host -> module, ring cmd: one word each, the op in the top byte) and events (module -> host, ring evt) */
enum { FBLE_CMD_ADVERTISE = 1, FBLE_CMD_SCAN, FBLE_CMD_CONNECT, FBLE_CMD_DISCONNECT, FBLE_CMD_ENABLE };
#define FBLE_CMD(op, arg) ((uint32_t)(op) << 24 | ((uint32_t)(arg) & 0xFFFFFFu))
enum { FBLE_EVT_STATE = 1, FBLE_EVT_FOUND, FBLE_EVT_DEVICE };
#define FBLE_EVT(op, arg) ((uint32_t)(op) << 24 | ((uint32_t)(arg) & 0xFFFFFFu))
enum { FBLE_OFF, FBLE_STARTING, FBLE_READY, FBLE_FAILED };
enum { FBLE_DEV_OFF, FBLE_DEV_SCANNING, FBLE_DEV_CONNECTING, FBLE_DEV_SEARCHING, FBLE_DEV_READY };   /* api->device() */

/* the host's services */
typedef struct {
    uint16_t abi_major, abi_minor;
    uint32_t size;                                /* sizeof (fble_host): later members only if it is big enough */
    uint32_t (*ms)(void);                         /* milliseconds, monotonic */
    void (*log)(const char *line);                /* a line for the log (up to ~40 characters) */
    void (*wdt_feed)(void);
    void (*irq_attach)(int index, int prio, void (*isr)(void));   /* JieLi's priority 0..2: the host maps it */
    void (*stack_floor)(uint32_t lo);             /* the tasks' stacks go down to lo (the guard's stack window) */
    void (*quiet)(int on);                        /* the radio calibrates: no flash reads from interrupts meanwhile */
    int (*nv_load)(void *dst, uint32_t max);      /* the module's few bytes of NV (address, pairing): length or < 0 */
    int (*nv_save)(const void *src, uint32_t n);
    void (*reboot)(void);
    void (*tap)(const uint8_t *b, uint32_t len, int dev);   /* 0.2: each BLE MIDI packet in, raw (the monitor's copy);
                                                             * dev 1 = the central's device, 0 = the computer */
} fble_host;

/* the module's calls */
typedef struct {
    uint16_t abi_major, abi_minor;
    uint32_t size;
    int (*start)(void);                           /* the radio on (BT stack init): 0 = starting */
    void (*poll)(void);                           /* every main loop: timers, the BT tasks, the rings */
    void (*stop)(void);                           /* links down, the radio quiet */
    int (*state)(void);                           /* FBLE_OFF .. FBLE_FAILED */
    fble_ring *midi_in, *midi_out, *cmd, *evt;
    int (*found)(int i, char name[21], int *rssi, uint8_t addr[6]);   /* device i of the scan: 0 = none */
    int (*device)(void);                          /* the central's state: FBLE_DEV_* */
    int (*peer)(void);                            /* the computer's link: 0 none, 1 linked, 2 listening */
} fble_api;

typedef const fble_api *(*fble_init_fn)(const fble_host *h, void *low, uint32_t low_n, void *any, uint32_t any_n);
