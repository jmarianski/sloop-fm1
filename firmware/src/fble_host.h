/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Jacek Mariański */
/* fble_host.c's names the files before it use (felucca.c includes this before params.c when FELUCCA_FBLE is 1):
 * the audio interrupt's quiet flag, the MIDI out hook, the editor's USR4 check and the GLO > BLE pages' parts. */
#pragma once
#include "fble.h"

static volatile uint8_t fble_quiet;               /* the module's radio calibrates: the render holds (audio.c) */
static void fble_midi_out(uint32_t pkt);          /* usb.c midi_out_try (the audio interrupt): a copy to Bluetooth */
static int fble_usr4_taken(void);                 /* editor.c: USR4 is the module's, no sample upload there */
static void fble_knob(uint32_t page2, uint32_t slot, int32_t steps);   /* ui_input.c edit_param */
static void fble_columns(uint32_t page2);         /* ui_draw.c draw_columns */
static void fble_graph(void);                     /* ui_draw.c draw_graph */
static uint32_t fble_graph_sig(void);             /* ui_draw.c graph_signature */
