/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Jacek Mariański */
/* fext_host.c's names the files before it use (felucca.c includes this before params.c when FELUCCA_FEXT is 1), and
 * where SLOOP keeps a module: its slot and RAM windows (fext.h). This is the one place they are written down: the
 * linker script (firmware/app_fext.ld.in, filled in by build.py), the loader's check and the host's info (SysEx
 * "FB" op 10, which the installer moves the module by) all take them from here.
 *   slot  USR4 [0xE7000, 0xFB000), 80 KiB: the user sample slot 4 (no sample there while a module is: editor.c)
 *   CODE  [0x01C06F00, 0x01C07F00)  4 KiB: the top of SLOOP's RAMTEXT (.lowram ends below it, crt0's STATUS area
 *                                   is above it)
 *   DMA   [0x01C08000, 0x01C0E000) 24 KiB: the bottom of SLOOP's RAM region, below its .data
 *   RAM   [0x01C1E000, 0x01C20000)  8 KiB: the top of SLOOP's RAM region, above its .bss, below the pool */
#pragma once
#include "fext.h"

#define FEXT_SLOT_OFF 0xE7000u
#define FEXT_SLOT_MAX 0x14000u
#define FEXT_CODE_LO 0x01C06F00u
#define FEXT_CODE_HI 0x01C07F00u
#define FEXT_DMA_LO 0x01C08000u
#define FEXT_DMA_HI 0x01C0E000u
#define FEXT_RAM_LO 0x01C1E000u
#define FEXT_RAM_HI 0x01C20000u

static int8_t fext_power_fix;                     /* main.c fm1_power_stock(): 1 the DC-DC moved on */
static int8_t fext_clock_fix;                     /* main.c fm1_clock_stock(): 0 stock already, 1 set, -1 failed */
static volatile uint8_t fext_quiet;               /* the module's radio calibrates: the render holds (audio.c) */
static char fext_title[2][8] = {"EXT", "EXT 2"};  /* the module's pages' titles (params.c PAGES): its name's */
static void fext_midi_out(uint32_t pkt);          /* usb.c midi_out_try (the audio interrupt): a copy to the module */
static int fext_slot_taken(void);                 /* editor.c: USR4 is the module's, no sample upload there */
static uint32_t fext_pages(void);                 /* params.c page_shown: the module's pages (1 without one) */
static int fext_col_used(uint32_t page2, uint32_t col);   /* ui_input.c: a knob of a column the module shows */
static void fext_knob(uint32_t page2, uint32_t col, int32_t steps);   /* ui_input.c edit_param */
static void fext_columns(uint32_t page2);         /* ui_draw.c draw_columns */
static void fext_graph(uint32_t page2);           /* ui_draw.c draw_graph */
static uint32_t fext_graph_sig(uint32_t page2);   /* ui_draw.c graph_signature */
