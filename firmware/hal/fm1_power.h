/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Copyright (C) 2026 Charles Vestal (VDC14 and SYSVDD, the second core held on reset) */
/* (SLOOP fext) from fm1-base bsp/chip/fm1_sys.h: the power setup, as there; fm1_sys.h first */
#pragma once
/* ----------------------------------------------------- the power setup --- */
/* The rest of M-VAVE's power setup (V15's __hw_power_init, the SDK's cpu.a power_hw.c read with llvm-dis: its
 * power_dcdc_ldo_config and the writes after it). The boot loaders leave the 1.3 V supply on its linear regulator
 * (P3_ANA_CON0 bit 2) and the DC-DC (bit 3) off; V15 moves it onto the DC-DC and tunes it (ANA_CON7 0xA5, ANA_CON8
 * 0x96), sets VDDIOM 4 (3.2 V), and puts the WiFi 1.2 V regulator on automatic (WLDO12_AUTO 0xC0). The always-on P33
 * keeps them across a reset, so an FM-1 that last ran V15 has them and one that has not lacks them: on an FM-1
 * (2026-10-23) WiFi heard far less without them (scans empty, 1-4 frames a second against 12-21). The order is the
 * SDK's: the regulator on, 1 ms, the DC-DC on, 1 ms, the regulator off, 1 ms; then the tuning. Not taken: the sleep
 * path's settings (low-voltage detect, wake-up pins, the low-power timers). After fm1_vdc14_raise / fm1_sysvdd_raise
 * (VDC14 and SYSVDD are part of the same setup). Returns 1 if the DC-DC was off (and is on now), 0 if it was on. */
#define FM1_P3_ANA_CON0 0x00u                   /* bit 2 LDO13_EN, bit 3 DCDC13_EN, bit 7 RC_250k_EN */
#define FM1_P3_ANA_CON1 0x01u
#define FM1_P3_ANA_CON2 0x02u                   /* bit 6 VDDOK_DIS */
#define FM1_P3_ANA_CON5 0x05u                   /* [3:0] VDDIOM, [5:4] VDDIOW, [7:6] VDDIO drive */
#define FM1_P3_ANA_CON7 0x07u                   /* the DC-DC's bias, duty, oscillator */
#define FM1_P3_ANA_CON8 0x08u                   /* the DC-DC's V2I, current sense, comparator, zero, ramp */
#define FM1_P3_ANA_KEEP 0x16u
#define FM1_P3_WLDO12_AUTO 0x74u
static int fm1_power_stock(void)
{
    int moved = 0;
    if (!(fm1_p33_read(FM1_P3_ANA_CON0) & 0x08u)) {
        fm1_p33_or(FM1_P3_ANA_CON0, 0x04u);             /* the regulator on (it is) */
        fm1_delay_us(1000);
        fm1_p33_or(FM1_P3_ANA_CON0, 0x08u);             /* the DC-DC on beside it */
        fm1_delay_us(1000);
        fm1_p33_and(FM1_P3_ANA_CON0, (uint8_t)~0x04u);  /* the regulator off */
        fm1_delay_us(1000);
        moved = 1;
    }
    fm1_p33_write(FM1_P3_ANA_CON1, 0x20u);
    fm1_p33_write(FM1_P3_ANA_CON7, 0xA5u);
    fm1_p33_write(FM1_P3_ANA_CON8, 0x96u);
    fm1_p33_write(FM1_P3_ANA_CON5, (uint8_t)((fm1_p33_read(FM1_P3_ANA_CON5) & 0xC0u) | 4u));   /* VDDIOM 4, VDDIOW 0 */
    fm1_p33_or(FM1_P3_ANA_CON0, 0x80u);
    fm1_p33_and(FM1_P3_ANA_KEEP, 0xC0u);
    fm1_p33_or(FM1_P3_ANA_CON2, 0x40u);
    fm1_p33_write(FM1_P3_WLDO12_AUTO, 0xC0u);
    return moved;
}
