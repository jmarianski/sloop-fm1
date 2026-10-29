/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Jacek Mariański */
/* The Bluetooth module's loader as SLOOP builds it (FELUCCA_FBLE: firmware/src/fble.h, fble_load.h; fble_host.c runs
 * the same fble_check and fble_unpack on the FM-1), on this computer: a 1 MiB flash with the module where fble_push.py
 * writes it (USR4, 0xE7000), read through SLOOP's XIP mapping (flash offset off at 0x02000000 + off - 0x4000).
 *
 *   tests/fble_test [SEG0 [FRAMES]]
 * Always: a module made here for SLOOP's windows (one segment, ABI 0.3) is accepted, and what the loader must refuse
 * is refused: an erased slot, the magic, the ABI, the header's and segment 0's CRC, a segment 1 (ABI 0.2's USR3: SLOOP's
 * user samples), a module linked for MIDIMON's windows, a load into SLOOP's own RAM, init outside the code, a segment 0
 * past USR4. The write frames' decoder (fble_unpack): round trip, malformed frames.
 * With SEG0 (fble/build-sloop/fble_seg0.bin of the module linked for SLOOP, sh build.sh fble-sloop): the real module is
 * accepted, its load steps are listed; with FRAMES (fble_push.frames, what fble_push.py push sends): every write frame
 * decodes into USR4 only (fble_host.c's fble_write refuses anything else) and they give the same flash.
 * Exit 0: all as expected. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fble.h"
#include "fble_load.h"
#include "fble_name.h"

static uint8_t flash[0x100000];
static uint8_t save[0x100000];
static int fails;

static const uint8_t *fl(uint32_t off, uint32_t n)
{
    return off >= 0x93000u && off < sizeof flash && n <= sizeof flash - off ? flash + off : 0;
}
static uint32_t xip(uint32_t off) { return 0x02000000u + off - 0x4000u; }

static void expect(const char *what, const char *want)
{
    fble_header h;
    const char *got = fble_check(fl, xip, &h);
    int ok = want ? got && !strcmp(got, want) : !got;
    printf("%s  %-44s -> %s\n", ok ? "ok  " : "FAIL", what, got ? got : "accepted");
    fails += !ok;
}

static fble_header *hdr(void) { return (fble_header *)(flash + FBLE_SEG0_OFF); }
static void recrc(void)
{
    fble_header *p = hdr();
    p->hdr_crc = 0;
    p->hdr_crc = fble_crc32(0, (uint8_t *)p, sizeof *p);
}

/* a module as fble's linker script lays one out for SLOOP: header, code, .ram_text and .data images */
static void make_module(uint32_t n0)
{
    fble_header *h = hdr();
    uint32_t x0 = xip(FBLE_SEG0_OFF), i;
    memset(flash, 0xFF, sizeof flash);
    for (i = sizeof *h; i < n0; i++)
        flash[FBLE_SEG0_OFF + i] = (uint8_t)(i * 7u + 3u);
    memset(h, 0, sizeof *h);
    h->magic = FBLE_MAGIC;
    h->abi_major = FBLE_ABI_MAJOR;
    h->abi_minor = FBLE_ABI_MINOR;
    h->hdr_size = sizeof *h;
    h->seg[0].off = FBLE_SEG0_OFF;
    h->seg[0].size = n0;
    h->seg[0].crc = fble_crc32(0, flash + FBLE_SEG0_OFF + sizeof *h, n0 - (uint32_t)sizeof *h);
    h->xip_lo = x0 + 0x100u;
    h->xip_hi = x0 + n0 - 0x2000u;
    h->rt_lo = FBLE_RT_LO;
    h->rt_hi = FBLE_RT_LO + 0xF28u;
    h->low_lo = FBLE_LOW_LO;
    h->low_hi = FBLE_LOW_LO + 0x5C28u;
    h->any_lo = FBLE_ANY_LO;
    h->any_hi = FBLE_ANY_HI;
    h->copy[0] = (fble_copy){FBLE_RT_LO, x0 + n0 - 0x2000u, 0xF28u};         /* .ram_text */
    h->copy[1] = (fble_copy){FBLE_LOW_LO, x0 + n0 - 0x1000u, 0x400u};        /* .data */
    h->copy[2] = (fble_copy){FBLE_LOW_LO + 0x400u, 0, 0x5828u};              /* .bss */
    h->copy[3] = (fble_copy){FBLE_ANY_LO, 0, FBLE_ANY_HI - FBLE_ANY_LO};     /* .pool */
    h->init = x0 + 0x1234u;
    h->feat = FBLE_F_PERIPHERAL | FBLE_F_CENTRAL;
    strcpy(h->name, "fble");
    strcpy(h->version, "0.3-test");
    recrc();
}

#define CASE(what, want, change) do { memcpy(flash, save, sizeof flash); change; expect(what, want); } while (0)
static void refusals(uint32_t n0)
{
    CASE("magic changed", "no module", flash[FBLE_SEG0_OFF] ^= 1);
    CASE("abi major 1", "abi", hdr()->abi_major = 1);
    CASE("abi minor 9 (CRC redone)", 0, { hdr()->abi_minor = 9; recrc(); });
    CASE("a header byte changed", "header crc", hdr()->name[3] ^= 1);
    CASE("a code byte in segment 0", "segment 0 crc", flash[FBLE_SEG0_OFF + n0 / 2] ^= 0x40);
    CASE("a segment 1 in USR3, as ABI 0.2's (CRC redone)", "segment 1",
         { hdr()->seg[1].off = 0xC8000u; hdr()->seg[1].size = 0x100u; recrc(); });
    CASE("segment 1 with only a CRC (CRC redone)", "segment 1", { hdr()->seg[1].crc = 1; recrc(); });
    CASE("segment 0 past USR4 (CRC redone)", "segment 0", { hdr()->seg[0].size = FBLE_SEG0_MAX + 4u; recrc(); });
    CASE("segment 0 not at 0xE7000 (CRC redone)", "segment 0", { hdr()->seg[0].off = 0xC8000u; recrc(); });
    CASE("init outside the code (CRC redone)", "init", { hdr()->init = 0x02000200u; recrc(); });
    CASE("MIDIMON's rt window 0x01C06000 (CRC redone)", "RAM windows", { hdr()->rt_lo = 0x01C06000u; recrc(); });
    CASE("MIDIMON's low window 0x01C0E000.. (CRC redone)", "RAM windows",
         { hdr()->low_lo = 0x01C0E000u; hdr()->low_hi = 0x01C14000u; recrc(); });
    CASE("MIDIMON's any window 0x01C6C000.. (CRC redone)", "RAM windows",
         { hdr()->any_lo = 0x01C6C000u; hdr()->any_hi = 0x01C6E000u; recrc(); });
    CASE("low window into SLOOP's .data (CRC redone)", "RAM windows",
         { hdr()->low_hi = FBLE_LOW_HI + 0x100u; recrc(); });
    CASE("a load into SLOOP's RAM (CRC redone)", "load: destination",
         { hdr()->copy[2].dst = FBLE_LOW_HI; recrc(); });
    CASE("a load into the pool (CRC redone)", "load: destination",
         { hdr()->copy[3].dst = FBLE_ANY_HI; recrc(); });
    CASE("a load from outside segment 0 (CRC redone)", "load: source",
         { hdr()->copy[1].src = xip(0xC8000u); recrc(); });
    CASE("a load not word aligned (CRC redone)", "load: alignment", { hdr()->copy[1].n = 0x401u; recrc(); });
}

/* frames as fble_push.py sends them (op 2: the offset in three 7-bit bytes, then 8-byte groups) */
static uint32_t pack(uint8_t *o, uint32_t off, const uint8_t *raw, uint32_t n)
{
    uint32_t k = 0, i, j;
    o[k++] = 0xF0; o[k++] = 0x7D; o[k++] = 0x46; o[k++] = 0x42; o[k++] = 2;
    o[k++] = off & 0x7Fu; o[k++] = off >> 7 & 0x7Fu; o[k++] = off >> 14 & 0x7Fu;
    for (i = 0; i < n; i += 7) {
        uint8_t hi = 0;
        for (j = 0; j < 7 && i + j < n; j++)
            hi |= (uint8_t)((raw[i + j] >> 7) << j);
        o[k++] = hi;
        for (j = 0; j < 7 && i + j < n; j++)
            o[k++] = raw[i + j] & 0x7Fu;
    }
    o[k++] = 0xF7;
    return k;
}

static void unpack_cases(void)
{
    uint8_t raw[256], got[256], fr[400];
    uint32_t i, k, off = 0;
    int n, ok;
    for (i = 0; i < sizeof raw; i++)
        raw[i] = (uint8_t)(i * 37u + 11u);
    k = pack(fr, 0xE7100u, raw, 256);
    n = fble_unpack(fr + 5, k - 6, &off, got, sizeof got);
    ok = n == 256 && off == 0xE7100u && !memcmp(raw, got, 256);
    printf("%s  unpack: 256 B at E7100 round trip (%u B frame)\n", ok ? "ok  " : "FAIL", k);
    fails += !ok;
    n = fble_unpack(fr + 5, k - 6, &off, got, 255);
    printf("%s  unpack: longer than the buffer -> %d\n", n == -1 ? "ok  " : "FAIL", n);
    fails += n != -1;
    fr[9] |= 0x80u;
    n = fble_unpack(fr + 5, k - 6, &off, got, sizeof got);
    printf("%s  unpack: a byte with bit 7 -> %d\n", n == -1 ? "ok  " : "FAIL", n);
    fails += n != -1;
    n = fble_unpack(fr + 5, 2, &off, got, sizeof got);
    printf("%s  unpack: no offset -> %d\n", n == -1 ? "ok  " : "FAIL", n);
    fails += n != -1;
}

/* the real frames into an erased flash, as fble_host.c's fble_write (USR4 only; a sector erased by the frame at its
 * start) */
static int frames(const char *path, const uint8_t *want)
{
    static uint8_t buf[1u << 20];
    uint8_t raw[256];
    size_t n, i = 0;
    uint32_t count = 0, off, k, lo = 0xFFFFFFFFu, hi = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return perror(path), 1;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    memset(flash, 0xFF, sizeof flash);
    while (i < n) {
        size_t e = i;
        int got;
        while (e < n && buf[e] != 0xF7)
            e++;
        if (e >= n || buf[i] != 0xF0 || e - i > 640 || buf[i + 1] != 0x7D || buf[i + 2] != 0x46 ||
            buf[i + 3] != 0x42 || buf[i + 4] != 2)
            return printf("FAIL  frame %u: not a write frame (or over 640 B)\n", count), 1;
        got = fble_unpack(buf + i + 5, (uint32_t)(e - i - 5), &off, raw, sizeof raw);
        if (got <= 0 || !fble_in(off, (uint32_t)got, FBLE_SEG0_OFF, FBLE_SEG0_OFF + FBLE_SEG0_MAX))
            return printf("FAIL  frame %u: does not decode into USR4\n", count), 1;
        if (!(off & 0xFFFu))
            memset(flash + off, 0xFF, 0x1000);
        for (k = 0; k < (uint32_t)got; k++)
            flash[off + k] &= raw[k];
        lo = off < lo ? off : lo;
        hi = off + (uint32_t)got > hi ? off + (uint32_t)got : hi;
        count++;
        i = e + 1;
    }
    for (k = 0; k < sizeof flash; k++)
        if (flash[k] != want[k])
            return printf("FAIL  the frames' flash differs at %05X\n", k), 1;
    printf("ok    %u write frames (%zu B), all in USR4 (%05X..%05X), give the same flash\n", count, n, lo, hi);
    return 0;
}

/* the devices' names (fble_name.h): UTF-8 into what the font draws (gfx.c glyph: ASCII, Latin-1 but C1, sharp s and
 * y diaeresis), Polish and the other Latin Extended-A letters without their marks */
static void name_case(const char *src, uint32_t max, const char *want)
{
    char got[64];
    int ok;
    memset(got, 'X', sizeof got);
    fble_name_fold(got, src, max);
    ok = !strcmp(got, want);
    printf("%s  name %-34s -> %s\n", ok ? "ok  " : "FAIL", src, got);
    fails += !ok;
}
static int drawable(uint8_t c) { return (c >= 0x20u && c < 0x7Fu) || (c >= 0xA0u && c != 0xDFu && c != 0xFFu); }
static void name_cases(void)
{
    char src[8], got[21];
    uint32_t cp, i, bad = 0, n = 0;
    name_case("\xC5\xBB\xC3\x93\xC5\x81\xC4\x84", 21, "Z\xD3LA");                        /* ŻÓŁĄ */
    name_case("\xC5\xBB\xC3\x93\xC5\x81W \xC4\x84\xC4\x98", 21, "Z\xD3LW AE");           /* ŻÓŁW ĄĘ */
    name_case("Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85 ja\xC5\xBA\xC5\x84", 21,
              "Zaz\xF3lc gesla jazn");                                                    /* Zażółć gęślą jaźń */
    name_case("\xC4\x8C" "e\xC5\xA1tina \xC5\x90r\xC3\xBC" "let", 21, "Cestina Or\xFClet");  /* Čeština Őrület */
    name_case("Stra\xC3\x9F" "e", 21, "Strasse");                                         /* sharp s */
    name_case("Caf\xE9", 21, "Caf\xE9");                                                   /* sent in Latin-1 */
    name_case("ABCDEFGHIJKLMNOPQRS\xC5", 21, "ABCDEFGHIJKLMNOPQRS");                       /* 20 B: cut in two */
    name_case("NUX \xC5", 21, "NUX \xC5");                                                 /* shorter: Latin-1 */
    name_case("A\xC5X", 21, "A\xC5X");                                                     /* a lone lead byte */
    name_case("\xE6\x97\xA5\xE6\x9C\xAC", 21, "??");                                       /* CJK */
    name_case("MIDI \xF0\x9F\x8E\xB9", 21, "MIDI ?");                                      /* an emoji */
    name_case("A\tB\xC2\x85" "C", 21, "AB?C");                                             /* a control, C1 */
    name_case("\xC5\xBB\xC3\x93\xC5\x81\xC4\x84\xC5\xBB", 5, "Z\xD3LA");                  /* max: 4 and the 0 */
    name_case("ab\xC3\x9F", 4, "abs");                                                     /* sharp s at the end */
    for (cp = 1; cp < 0x3000u; cp++) {                                                    /* every code point: drawable */
        i = 0;
        if (cp < 0x80u)
            src[i++] = (char)cp;
        else if (cp < 0x800u)
            src[i++] = (char)(0xC0u | cp >> 6), src[i++] = (char)(0x80u | (cp & 0x3Fu));
        else
            src[i++] = (char)(0xE0u | cp >> 12), src[i++] = (char)(0x80u | (cp >> 6 & 0x3Fu)),
            src[i++] = (char)(0x80u | (cp & 0x3Fu));
        src[i] = 0;
        fble_name_fold(got, src, sizeof got);
        for (i = 0; got[i]; i++)
            bad += !drawable((uint8_t)got[i]);
        n += (cp >= 0x20u && cp != 0x7Fu) != (got[0] != 0);
    }
    printf("%s  U+0001..U+2FFF: every name drawable by the font, one character each (%u bad, %u lost)\n",
           bad || n ? "FAIL" : "ok  ", bad, n);
    fails += bad || n;
}

int main(int argc, char **argv)
{
    fble_header h;
    uint32_t n0, b;
    const char *why;

    printf("SLOOP's windows: rt %08X..%08X low %08X..%08X any %08X..%08X, segment 0 %05X +%05X (XIP %08X)\n",
           FBLE_RT_LO, FBLE_RT_HI, FBLE_LOW_LO, FBLE_LOW_HI, FBLE_ANY_LO, FBLE_ANY_HI, FBLE_SEG0_OFF, FBLE_SEG0_MAX,
           xip(FBLE_SEG0_OFF));
    fails += xip(FBLE_SEG0_OFF) != 0x020E3000u || FBLE_SEG0_OFF + FBLE_SEG0_MAX != 0xFB000u;

    memset(flash, 0xFF, sizeof flash);
    expect("erased USR4", "no module");
    make_module(0x13000u);
    expect("a module made for SLOOP's windows", 0);
    memcpy(save, flash, sizeof flash);
    refusals(0x13000u);
    unpack_cases();
    name_cases();

    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f)
            return perror(argv[1]), 2;
        memset(flash, 0xFF, sizeof flash);
        n0 = (uint32_t)fread(flash + FBLE_SEG0_OFF, 1, FBLE_SEG0_MAX + 1u, f);
        fclose(f);
        if (n0 > FBLE_SEG0_MAX) {
            printf("FAIL  %s: longer than USR4\n", argv[1]);
            return 1;
        }
        memcpy(save, flash, sizeof flash);
        why = fble_check(fl, xip, &h);
        if (why) {
            printf("FAIL  the module linked for SLOOP (%s) is refused: %s\n", argv[1], why);
            return 1;
        }
        printf("ok    the module linked for SLOOP: %u B, %s %s, abi %u.%u, init %08X, code %08X..%08X\n", n0, h.name,
               h.version, h.abi_major, h.abi_minor, h.init, h.xip_lo, h.xip_hi);
        printf("      windows rt %08X..%08X low %08X..%08X any %08X..%08X\n", h.rt_lo, h.rt_hi, h.low_lo, h.low_hi,
               h.any_lo, h.any_hi);
        for (b = 0; b < FBLE_NCOPY; b++)
            printf("      load %u: %08X <- %08X, %u B\n", b, h.copy[b].dst, h.copy[b].src, h.copy[b].n);
        refusals(n0);
        if (argc > 2) {
            fails += frames(argv[2], save);
            expect("the flash the frames wrote", 0);
        }
    }
    printf(fails ? "fble_test: %d FAILED\n" : "fble_test: all as expected\n", fails);
    return fails != 0;
}
