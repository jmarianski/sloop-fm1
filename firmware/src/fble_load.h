/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Jacek Mariański */
/* fble's loader, the part with no hardware in it (include/fble.h): is there a module, and may it be loaded? Shared by
 * the firmware (MIDIMON's host-stub: radio_fble.c; SLOOP: fble_host.c) and a test on the computer (MIDIMON:
 * fble/tests/test_load.c; SLOOP: tests/fble_test.c), so the check that runs on the FM-1 is the one tested. (SLOOP's
 * copy is fm1-base's of ABI 0.3, M5, with these two comments widened.)
 *
 *   fble_check(fl, xip, &h)     0, or why not (a short text); fl(off, n): n bytes of the flash at offset off (the
 *                               firmware: the unencrypted XIP window), 0 when not readable; xip(off): the CPU
 *                               address of flash offset off. On success h holds the header
 * Checked: the magic, the ABI's major, the header's size and CRC, segment 0's place, size and CRC (and no segment 1:
 * ABI 0.3), the code's range inside segment 0, the RAM windows inside fble.h's (which the host keeps free), each load step's source
 * inside segment 0 and destination inside its window, and init inside the code.
 *   fble_unpack(p, n, &off, raw, max)   a write frame's payload (SysEx "FB" op 2, radio_fble.c / fble_host.c): the flash offset (three
 *                               7-bit bytes, low first), then groups of a byte of high bits and up to 7 bytes; the
 *                               bytes into raw, their number returned, -1 when malformed or longer than max */
#pragma once
#include <stdint.h>
#include "fble.h"

typedef const uint8_t *(*fble_flash_fn)(uint32_t off, uint32_t n);
typedef uint32_t (*fble_xip_fn)(uint32_t off);

static uint32_t fble_crc32(uint32_t crc, const uint8_t *b, uint32_t n)   /* zlib's CRC-32, 4 bits a step */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        crc = (crc >> 4) ^ T[crc & 15u];
        crc = (crc >> 4) ^ T[crc & 15u];
    }
    return ~crc;
}

/* [lo, lo + n) inside [wlo, whi), without wrapping */
static int fble_in(uint32_t lo, uint32_t n, uint32_t wlo, uint32_t whi)
{
    return lo >= wlo && lo <= whi && n <= whi - lo;
}

static const char *fble_check(fble_flash_fn fl, fble_xip_fn xip, fble_header *h)
{
    const uint8_t *p = fl(FBLE_SEG0_OFF, sizeof *h);
    uint32_t crc, i, x0, x1, n0;
    uint8_t *d = (uint8_t *)h;
    if (!p)
        return "no flash";
    for (i = 0; i < sizeof *h; i++)               /* (a copy: the header's CRC is taken with its field 0) */
        d[i] = p[i];
    if (h->magic != FBLE_MAGIC)
        return "no module";
    if (h->abi_major != FBLE_ABI_MAJOR)
        return "abi";
    if (h->hdr_size != sizeof *h)
        return "header size";
    crc = h->hdr_crc;
    h->hdr_crc = 0;
    if (fble_crc32(0, d, sizeof *h) != crc)
        return "header crc";
    h->hdr_crc = crc;
    n0 = h->seg[0].size;
    if (h->seg[0].off != FBLE_SEG0_OFF || n0 < sizeof *h || n0 > FBLE_SEG0_MAX)
        return "segment 0";
    if (h->seg[1].off || h->seg[1].size || h->seg[1].crc)   /* 0.3: one segment (USR3 is not the module's) */
        return "segment 1";
    if (!(p = fl(FBLE_SEG0_OFF + sizeof *h, n0 - (uint32_t)sizeof *h)) ||
        fble_crc32(0, p, n0 - (uint32_t)sizeof *h) != h->seg[0].crc)
        return "segment 0 crc";
    /* the code: inside segment 0 */
    x0 = xip(FBLE_SEG0_OFF);
    x1 = xip(FBLE_SEG0_OFF) + n0;
    if (h->xip_lo < x0 || h->xip_hi > x1 || h->xip_lo >= h->xip_hi)
        return "code range";
    if (!fble_in(h->init, 2, xip(FBLE_SEG0_OFF), xip(FBLE_SEG0_OFF) + n0))
        return "init";
    if (h->rt_lo < FBLE_RT_LO || h->rt_hi > FBLE_RT_HI || h->low_lo < FBLE_LOW_LO || h->low_hi > FBLE_LOW_HI ||
        h->any_lo < FBLE_ANY_LO || h->any_hi > FBLE_ANY_HI)
        return "RAM windows";
    for (i = 0; i < FBLE_NCOPY; i++) {
        const fble_copy *c = &h->copy[i];
        if (!c->n)
            continue;
        if ((c->dst | c->n | c->src) & 3u)
            return "load: alignment";
        if (!fble_in(c->dst, c->n, h->rt_lo, h->rt_hi) && !fble_in(c->dst, c->n, h->low_lo, h->low_hi) &&
            !fble_in(c->dst, c->n, h->any_lo, h->any_hi))
            return "load: destination";
        if (c->src && !fble_in(c->src, c->n, xip(FBLE_SEG0_OFF) + (uint32_t)sizeof *h, xip(FBLE_SEG0_OFF) + n0))
            return "load: source";
    }
    return 0;
}

static int fble_unpack(const uint8_t *p, uint32_t n, uint32_t *off, uint8_t *raw, uint32_t max)
{
    uint32_t len = 0, i;
    if (n < 3u || ((p[0] | p[1] | p[2]) & 0x80u))
        return -1;
    *off = (uint32_t)p[0] | (uint32_t)p[1] << 7 | (uint32_t)p[2] << 14;
    p += 3;
    n -= 3;
    while (n) {
        uint8_t hi = *p++;
        n--;
        if (hi & 0x80u)
            return -1;
        for (i = 0; i < 7u && n; i++, n--) {
            if (len >= max || (*p & 0x80u))
                return -1;
            raw[len++] = (uint8_t)(*p++ | ((hi >> i) & 1u) << 7);
        }
    }
    return (int)len;
}
