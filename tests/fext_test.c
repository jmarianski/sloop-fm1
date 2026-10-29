/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Jacek Mariański */
/* SLOOP's socket for an fext module (FELUCCA_FEXT: firmware/src/fext.h, fext_load.h, fext_host.h; fext_host.c runs the
 * same fext_check and fext_unpack on the FM-1), on this computer: a 1 MiB flash with the module where the installer
 * writes it (USR4, 0xE7000), read through SLOOP's XIP mapping (flash offset off at 0x02000000 + off - 0x4000).
 *
 *   tests/fext_test [IMAGE [FRAMES]]
 * Always: a module made here for SLOOP's slot and windows is accepted, and what the loader must refuse is refused: an
 * erased slot, the magic, the ABI, the header's size and CRC, the image's size and CRC, a module not moved to this slot
 * (linked elsewhere: the installer moves it), init outside the image, windows outside SLOOP's or not aligned, loads
 * outside the windows or the image. The write frames' decoder (fext_unpack) and fext_pack: round trip, malformed
 * frames. Module texts folded for the font (fext_text.h).
 * With IMAGE (the module moved to SLOOP's slot: fble's build/check/fble-sloop.bin): the real module is accepted, its
 * load steps are listed; with FRAMES (what fext_push.py push --frames writes): every write frame decodes into USR4 only
 * (fext_host.c's fext_write refuses anything else) and they give the same flash.
 * Exit 0: all as expected. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fext_host.h"
#include "fext_load.h"
#include "fext_text.h"

static uint8_t flash[0x100000];
static uint8_t save[0x100000];
static int fails;

static const uint8_t *fl(uint32_t off, uint32_t n)
{
    return off >= 0x93000u && off < sizeof flash && n <= sizeof flash - off ? flash + off : 0;
}
static uint32_t xip(uint32_t off) { return 0x02000000u + off - 0x4000u; }
static const fext_slot SLOT = {FEXT_SLOT_OFF, FEXT_SLOT_MAX, 0x02000000u + FEXT_SLOT_OFF - 0x4000u,
                               {{FEXT_CODE_LO, FEXT_CODE_HI}, {FEXT_DMA_LO, FEXT_DMA_HI}, {FEXT_RAM_LO, FEXT_RAM_HI}}};

static void expect(const char *what, const char *want)
{
    fext_header h;
    const char *got = fext_check(fl, &SLOT, &h);
    int ok = want ? got && !strcmp(got, want) : !got;
    printf("%s  %-46s -> %s\n", ok ? "ok  " : "FAIL", what, got ? got : "accepted");
    fails += !ok;
}

static fext_header *hdr(void) { return (fext_header *)(flash + FEXT_SLOT_OFF); }
static void recrc(int image)                      /* the CRCs again after a change (the image's too, if asked) */
{
    fext_header *p = hdr();
    if (image)
        p->crc = fext_crc32(0, flash + FEXT_SLOT_OFF + sizeof *p, p->size - (uint32_t)sizeof *p);
    p->hdr_crc = 0;
    p->hdr_crc = fext_crc32(0, (uint8_t *)p, sizeof *p);
}

/* a module as fext's linker scripts lay one out, moved to SLOOP: header, code, the .ram_text and .data images */
static void make_module(uint32_t n)
{
    fext_header *h = hdr();
    uint32_t x0 = xip(FEXT_SLOT_OFF), i;
    memset(flash, 0xFF, sizeof flash);
    for (i = 0; i < n; i++)
        flash[FEXT_SLOT_OFF + i] = (uint8_t)(i * 7u + 3u);
    memset(h, 0, sizeof *h);
    h->magic = FEXT_MAGIC;
    h->abi_major = FEXT_ABI_MAJOR;
    h->abi_minor = FEXT_ABI_MINOR;
    h->hdr_size = sizeof *h;
    h->size = n;
    h->xip = x0;
    h->win[FEXT_W_CODE] = (fext_win){FEXT_CODE_LO, FEXT_CODE_HI};
    h->win[FEXT_W_DMA] = (fext_win){FEXT_DMA_LO, FEXT_DMA_LO + 0x5C00u};
    h->win[FEXT_W_RAM] = (fext_win){FEXT_RAM_LO, FEXT_RAM_HI};
    h->copy[0] = (fext_copy){FEXT_CODE_LO, x0 + n - 0x2000u, 0xF28u};         /* .ram_text */
    h->copy[1] = (fext_copy){FEXT_DMA_LO, x0 + n - 0x1000u, 0x400u};          /* .data */
    h->copy[2] = (fext_copy){FEXT_DMA_LO + 0x400u, 0, 0x5800u};               /* .bss */
    h->copy[3] = (fext_copy){FEXT_RAM_LO, 0, FEXT_RAM_HI - FEXT_RAM_LO};      /* .pool */
    h->init = x0 + 0x200u;
    h->caps = FEXT_C_MIDI;
    memcpy(h->name, "TEST", 5);
    memcpy(h->version, "0.1", 4);
    recrc(1);
}

/* each change from the saved good flash, one at a time */
#define BAD(what, want, change, image) do { memcpy(flash, save, sizeof flash); { fext_header *b = hdr(); (void)b; change; } \
                                           recrc(image); expect(what, want); } while (0)
static void refusals(void)
{
    fext_header *h = hdr();
    uint32_t n = h->size, x0 = xip(FEXT_SLOT_OFF);
    memcpy(save, flash, sizeof flash);
    memcpy(flash, save, sizeof flash);
    hdr()->magic ^= 1u;
    expect("the magic", "no module");
    BAD("another ABI major", "abi", b->abi_major++, 0);
    BAD("a later ABI minor (compatible)", 0, b->abi_minor++, 0);
    BAD("another header size", "header size", b->hdr_size += 4u, 0);
    memcpy(flash, save, sizeof flash);
    hdr()->caps ^= 4u;
    expect("the header changed, its CRC not", "header crc");
    memcpy(flash, save, sizeof flash);
    flash[FEXT_SLOT_OFF + n - 1u] ^= 1u;
    expect("the image changed, its CRC not", "crc");
    BAD("longer than USR4", "size", b->size = FEXT_SLOT_MAX + 4u, 0);
    BAD("not moved here (linked at 0x10000000)", "not moved here", b->xip = 0x10000000u, 0);
    BAD("init in the header", "init", b->init = x0 + 4u, 0);
    BAD("init past the image", "init", b->init = x0 + n, 0);
    BAD("CODE window into SLOOP's .lowram", "RAM windows", b->win[FEXT_W_CODE].lo = FEXT_CODE_LO - 0x100u, 0);
    BAD("DMA window into SLOOP's .data", "RAM windows", b->win[FEXT_W_DMA].hi = FEXT_DMA_HI + 0x100u, 0);
    BAD("RAM window into the pool", "RAM windows", b->win[FEXT_W_RAM].hi = FEXT_RAM_HI + 0x100u, 0);
    BAD("a window not 256-aligned", "RAM windows", b->win[FEXT_W_DMA].lo += 0x10u, 0);
    BAD("a window upside down", "RAM windows", b->win[FEXT_W_DMA].lo = b->win[FEXT_W_DMA].hi + 0x100u, 0);
    BAD("MIDIMON's windows (CODE in its RAM)", "RAM windows",
        (b->win[FEXT_W_CODE] = (fext_win){0x01C30000u, 0x01C31000u}), 0);
    BAD("a load into SLOOP's RAM", "load: destination", b->copy[1].dst = FEXT_DMA_HI, 0);
    BAD("a load across a window's end", "load: destination", b->copy[2].n = 0x6000u, 0);
    BAD("a load into a window it did not ask for", "load: destination",
        (b->win[FEXT_W_RAM] = (fext_win){0, 0}), 0);
    BAD("a load from outside the image", "load: source", b->copy[0].src = x0 + n - 0x100u, 0);
    BAD("a load from the header", "load: source", b->copy[0].src = x0, 0);
    BAD("a load not word-aligned", "load: alignment", b->copy[1].n = 0x401u, 0);
    memcpy(flash, save, sizeof flash);
}

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
    n = fext_unpack(fr + 5, k - 6, &off, got, sizeof got);
    ok = n == 256 && off == 0xE7100u && !memcmp(raw, got, 256);
    printf("%s  unpack: 256 B at E7100 round trip (%u B frame)\n", ok ? "ok  " : "FAIL", k);
    fails += !ok;
    n = fext_unpack(fr + 5, k - 6, &off, got, 255);
    printf("%s  unpack: longer than the buffer -> %d\n", n == -1 ? "ok  " : "FAIL", n);
    fails += n != -1;
    fr[9] |= 0x80u;
    n = fext_unpack(fr + 5, k - 6, &off, got, sizeof got);
    printf("%s  unpack: a byte with bit 7 -> %d\n", n == -1 ? "ok  " : "FAIL", n);
    fails += n != -1;
    {
        uint8_t raw40[40], f40[52], back[40];
        uint32_t m, o2;
        for (i = 0; i < 40u; i++)
            raw40[i] = (uint8_t)(0xFFu - i * 9u);
        m = fext_pack(raw40, 40, f40 + 3);
        f40[0] = f40[1] = f40[2] = 0;
        for (i = 3, ok = 1; i < m + 3u; i++)
            ok &= f40[i] < 0x80u;
        ok = ok && m == 46u && fext_unpack(f40, m + 3u, &o2, back, sizeof back) == 40 && !memcmp(back, raw40, 40);
        printf("%s  pack: 40 B (op 10's info) -> %u B 7-bit, unpacked the same\n", ok ? "ok  " : "FAIL", m);
        fails += !ok;
    }
    n = fext_unpack(fr + 5, 2, &off, got, sizeof got);
    printf("%s  unpack: no offset -> %d\n", n == -1 ? "ok  " : "FAIL", n);
    fails += n != -1;
}

/* the real frames into an erased flash, as fext_host.c's fext_write (USR4 only; a sector erased by the frame at its
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
        got = fext_unpack(buf + i + 5, (uint32_t)(e - i - 5), &off, raw, sizeof raw);
        if (got <= 0 || !fext_in(off, (uint32_t)got, FEXT_SLOT_OFF, FEXT_SLOT_OFF + FEXT_SLOT_MAX))
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

/* a module's texts (fext_text.h; fble's: the devices' names): UTF-8 into what the font draws (gfx.c glyph: ASCII, Latin-1 but C1, sharp s and
 * y diaeresis), Polish and the other Latin Extended-A letters without their marks */
static void name_case(const char *src, uint32_t max, const char *want)
{
    char got[64];
    int ok;
    memset(got, 'X', sizeof got);
    fext_name_fold(got, src, max);
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
        fext_name_fold(got, src, sizeof got);
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
    fext_header h;
    uint32_t n, b;
    const char *why;

    printf("SLOOP's slot %05X +%05X (XIP %08X), windows code %08X..%08X dma %08X..%08X ram %08X..%08X\n",
           FEXT_SLOT_OFF, FEXT_SLOT_MAX, SLOT.xip, FEXT_CODE_LO, FEXT_CODE_HI, FEXT_DMA_LO, FEXT_DMA_HI, FEXT_RAM_LO,
           FEXT_RAM_HI);
    fails += SLOT.xip != 0x020E3000u || FEXT_SLOT_OFF + FEXT_SLOT_MAX != 0xFB000u;

    memset(flash, 0xFF, sizeof flash);
    expect("erased USR4", "no module");
    make_module(0x13000u);
    expect("a module made for SLOOP's slot and windows", 0);
    refusals();
    unpack_cases();
    name_cases();

    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f)
            return perror(argv[1]), 2;
        memset(flash, 0xFF, sizeof flash);
        n = (uint32_t)fread(flash + FEXT_SLOT_OFF, 1, FEXT_SLOT_MAX + 1u, f);
        fclose(f);
        if (n > FEXT_SLOT_MAX) {
            printf("FAIL  %s: longer than USR4\n", argv[1]);
            return 1;
        }
        memcpy(save, flash, sizeof flash);
        why = fext_check(fl, &SLOT, &h);
        if (why) {
            printf("FAIL  the module moved to SLOOP (%s) is refused: %s\n", argv[1], why);
            return 1;
        }
        printf("ok    the module moved to SLOOP: %u B, %.8s %.16s, abi %u.%u, caps %X, init %08X\n", n, h.name,
               h.version, h.abi_major, h.abi_minor, h.caps, h.init);
        printf("      windows code %08X..%08X dma %08X..%08X ram %08X..%08X\n", h.win[0].lo, h.win[0].hi, h.win[1].lo,
               h.win[1].hi, h.win[2].lo, h.win[2].hi);
        for (b = 0; b < FEXT_NCOPY; b++)
            printf("      load %u: %08X <- %08X, %u B\n", b, h.copy[b].dst, h.copy[b].src, h.copy[b].n);
        refusals();
        if (argc > 2) {
            fails += frames(argv[2], save);
            expect("the flash the frames wrote", 0);
        }
    }
    printf(fails ? "fext_test: %d FAILED\n" : "fext_test: all as expected\n", fails);
    return fails != 0;
}
