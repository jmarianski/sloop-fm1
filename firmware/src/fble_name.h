/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Jacek Mariański */
/* A Bluetooth device's name (its advertising, UTF-8) as SLOOP's screen can draw it: the font has ASCII and Latin-1
 * (gfx.c glyph: capitals only, lower case folds), no UTF-8. Shared by the firmware (fble_host.c fble_found: every name
 * from the module) and a test on the computer (tests/fble_test.c), so the folding that runs on the FM-1 is the one tested.
 *
 *   fble_name_fold(dst, src, max)   src (0-terminated) into dst (max bytes with the 0):
 *     ASCII                         as it is (control characters left out)
 *     U+00A0..U+00FF                its Latin-1 byte (sharp s: "ss", y diaeresis: "y": the font has no capital of them)
 *     U+0100..U+017F (Latin         its letter without the mark: Polish, Czech, Hungarian, Baltic, Turkish ...
 *       Extended-A)                 (Ł -> L, ż -> z, ő -> o)
 *     any other code point          '?'
 *     a byte that is no UTF-8       Latin-1 from 0xA0 (a name sent in Latin-1), else '?'
 *     a sequence cut at the end     left out when the name has FBLE_NAME_LEN bytes (the module keeps 20: a letter may
 *                                   be cut in two); shorter, its bytes are Latin-1 */
#pragma once
#include <stdint.h>

#define FBLE_NAME_LEN 20u                           /* the module's names: 20 bytes and the 0 (fble.h found) */

static const char FBLE_LATIN_A[129] =               /* U+0100..U+017F without their marks */
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";

static void fble_name_fold(char *dst, const char *src, uint32_t max)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t o = 0, cp, n, i;
    if (!max)
        return;
    while (*s && o + 1u < max) {
        uint8_t c = *s;
        n = c >= 0xF0u && c < 0xF8u ? 3u : c >= 0xE0u ? (c < 0xF0u ? 2u : 0u) : c >= 0xC2u ? 1u : 0u;
        for (i = 1; i <= n && (s[i] & 0xC0u) == 0x80u; i++)
            ;
        if (n && i <= n && !s[i] && (uint32_t)(s + i - (const uint8_t *)src) >= FBLE_NAME_LEN)
            break;                                  /* cut at the end */
        if (n && i > n) {                           /* a whole sequence */
            cp = c & (0x3Fu >> n);
            for (i = 1; i <= n; i++)
                cp = cp << 6 | (s[i] & 0x3Fu);
            s += n + 1u;
        } else {                                    /* not UTF-8: Latin-1 */
            cp = c >= 0x80u && c < 0xA0u ? '?' : c;
            s++;
        }
        if (cp < 0x20u || cp == 0x7Fu)
            continue;
        if (cp == 0xDFu) {
            dst[o++] = 's';
            if (o + 1u < max)
                dst[o++] = 's';
            continue;
        }
        dst[o++] = (char)(cp == 0xFFu ? 'y' : cp < 0x80u || (cp >= 0xA0u && cp < 0x100u) ? cp
                          : cp >= 0x100u && cp < 0x180u ? (uint32_t)(uint8_t)FBLE_LATIN_A[cp - 0x100u] : '?');
    }
    dst[o] = 0;
}
