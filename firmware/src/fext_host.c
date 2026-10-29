/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Jacek Mariański
 * (the log and the interrupt attach follow MIDIMON's fm1-base device/radio.c, Copyright (C) 2026 Charles Vestal,
 *  MPL-2.0) */
/* SLOOP's socket for an fext module (FELUCCA_FEXT, tools/build.py --fext; fext.h): a module written into USR4 on its
 * own, found at boot and run in place. SLOOP links nothing of it and knows nothing of what it does: fble, the first,
 * brings Bluetooth MIDI, its pages and its settings with it. Part of felucca.c, after editor.c.
 *   at boot (fext_boot, after cpu1_boot): the header checked (fext_load.h), the module's RAM parts copied into the
 *     windows app_fext.ld keeps free (fext_host.h), the guard's code windows widened to it, init called. Not when
 *     the last run with the module crashed or the watchdog fired (then by hand: SysEx op 3)
 *   the main loop (fext_service): the module polled (with the page shown), its MIDI in into SLOOP's input ring (the
 *     path USB-MIDI and the jack take), its settings kept in flash when SLOOP is quiet (sector 0xFB000, with its NV)
 *   the audio interrupt: SLOOP's MIDI out copied to the module's out ring, to the ports it asks for (no call into it)
 *   its pages: GLO > <its name> (and <its name> 2): the columns, the knobs and the middle's lines are the module's
 *   SysEx F0 7D 46 42 op ... F7 ("FB": the module's tools/fext_push.py):
 *     1                  status: F0 7D 46 42 01 <text> F7 (the module's state, its line 0; or why there is none)
 *     2 o0 o1 o2 data    write: flash offset (7 bits a byte, low first) inside the slot, then up to 256 bytes 7-bit
 *                        packed (each group: a byte of high bits, then up to 7 bytes); a sector's first write erases
 *                        it. Answer F0 7D 46 42 02 err F7 (0 ok, 1 range, 2 erase, 3 write, 4 verify, 5 format, 6 the
 *                        module runs: restart without it first, op 6 1)
 *     3                  load now (if not loaded); 5 remove the module (its header's sector)
 *     6 f                reboot; f = 1: the next boot does not load the module (to write a new one)
 *     7 bytes            a command for the module (fext_api control): its text answer
 *     9 p c s            a knob of its pages (page, column, steps + 64), as the hand turns it: the status after
 *     10                 info: where the module goes, 7-bit packed (fext_load.h fext_pack): u16 ABI major, minor,
 *                        u32 the slot's offset, size, CPU address, the windows CODE, DMA, RAM (lo, hi each)
 *   the log: F0 7D 4C 47 F7 ("LG") answers line by line, then "end". SLOOP's editor speaks F0 7D 46 4C ("FL"), the
 *     update F0 22 24 35 ..: no clash */

/* ---- the log: the control words in .noinit (24 B), the lines in .fext_log (app_fext.ld: RAM, not zeroed). Not
 * static: a file-local object would let the compiler fold the magic checks (fm1_irq.h fm1_crash) */
#define FLOG_N 24
#define FLOG_MAGIC 0x534C4246u                    /* "FBLS" */
struct fext_flog { uint32_t magic, w, crash_seen, running, repeat, noload; };
struct fext_flog_lines { uint32_t magic; char line[FLOG_N][40]; };
struct fext_flog fext_flog __attribute__((section(".noinit")));
struct fext_flog_lines fext_flog_l __attribute__((section(".fext_log")));
#define flog fext_flog
UI_SMALL static void flog_fresh(void)
{
    if (flog.magic != FLOG_MAGIC || fext_flog_l.magic != FLOG_MAGIC || flog.w > 100000u) {
        flog.magic = fext_flog_l.magic = FLOG_MAGIC;
        flog.w = flog.running = flog.repeat = flog.noload = 0;
        flog.crash_seen = fm1_crash.count;
    }
}
UI_SMALL static void flog_line(const char *s)
{
    char *d;
    uint32_t i;
    flog_fresh();
    if (flog.w) {                                 /* the same line as the last one: fold it, "xN" */
        char *p = fext_flog_l.line[(flog.w - 1u) % FLOG_N];
        for (i = 0; i < 26u && s[i] && s[i] == p[i]; i++)
            ;
        if (!s[i] && (!p[i] || (p[i] == ' ' && p[i + 1] == 'x'))) {
            p[i] = 0;
            str_cpy(p + i, " x", 3);
            fmt_int(p + i + 2, (int32_t)(++flog.repeat + 1u));
            return;
        }
    }
    flog.repeat = 0;
    d = fext_flog_l.line[flog.w % FLOG_N];
    for (i = 0; i < 39u && s[i]; i++)
        d[i] = s[i] >= 32 && s[i] < 127 ? s[i] : ' ';
    d[i] = 0;
    flog.w++;
}
UI_SMALL static char *fcat(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}
UI_SMALL static char *fhex(char *b, uint32_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        *b++ = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15u];
    *b = 0;
    return b;
}

/* ---- the module, as loaded */
static const fext_api *fext;                      /* its calls, once init has answered */
static fext_header fext_h;                        /* its header, as checked */
static const char *fext_why = "not looked";       /* why there is none */
static uint8_t fext_last_bad, fext_slot_busy, fext_irqs;   /* (fext_irqs: interrupts it attached: its code runs from
                                                            * the slot on its own now, so no rewrite under it) */
static uint32_t fext_quiet_t;
static const fext_slot FEXT_SLOT = {FEXT_SLOT_OFF, FEXT_SLOT_MAX, 0,
                                    {{FEXT_CODE_LO, FEXT_CODE_HI}, {FEXT_DMA_LO, FEXT_DMA_HI}, {FEXT_RAM_LO, FEXT_RAM_HI}}};

/* ---- the module's settings and NV: one sector, FL_FEXT_NV (0xFB000; fm1_flash.h lets the fext build write it). A
 * header (written last) with the CRCs, the settings (FEXT_NV_SET) at +0x20, the NV (FEXT_NV_STACK) at +0x100. A write
 * cut short leaves no header: none of either then. (The layout and magic are the fble socket's of 0.3: a SLOOP that
 * had it keeps its Bluetooth settings and address.) The settings wait in the module's RAM until SLOOP is quiet */
typedef struct { uint32_t magic, set_len, set_crc, nv_len, nv_crc, hdr_crc; } fxnv_hdr;
#define FXNV_MAGIC 0x564E4246u                    /* "FBNV" */
#define FXNV_SET 0x20u
#define FXNV_NV 0x100u
#define FXNV_SET_MAX (FXNV_NV - FXNV_SET)
#define FXNV_NV_MAX 1536u
_Static_assert(FXNV_NV + FXNV_NV_MAX <= ST_PAYLOAD_MAX, "the sector's parts");
static const void *fx_set_src;                    /* the module's settings to write (its RAM), and how long */
static uint32_t fx_set_n, fx_set_t;

static const fxnv_hdr *fxnv(void)                 /* the header, if whole */
{
    const fxnv_hdr *h = (const fxnv_hdr *)fm1_xip_ptr(FL_FEXT_NV);
    if (!flash_ok || h->magic != FXNV_MAGIC || fext_crc32(0, (const uint8_t *)h, sizeof *h - 4u) != h->hdr_crc)
        return 0;
    return h;
}
/* a part of the sector, if whole: its bytes, its length */
static const uint8_t *fxnv_part(int id, uint32_t *n)
{
    const fxnv_hdr *h = fxnv();
    uint32_t at = id == FEXT_NV_SET ? FXNV_SET : FXNV_NV, len = !h ? 0u : id == FEXT_NV_SET ? h->set_len : h->nv_len;
    const uint8_t *p = fm1_xip_ptr(FL_FEXT_NV + at);
    if (!len || len > (id == FEXT_NV_SET ? FXNV_SET_MAX : FXNV_NV_MAX) ||
        fext_crc32(0, p, len) != (id == FEXT_NV_SET ? h->set_crc : h->nv_crc))
        return 0;
    *n = len;
    return p;
}
/* the sector again, from st_buf (the main loop's scratch, storage.c): each part given, or the one there. Erase, the
 * parts, the header last */
UI_SMALL static int fxnv_write(const void *set, uint32_t set_n, const void *nv, uint32_t nv_n)
{
    fxnv_hdr *h = (fxnv_hdr *)st_buf;
    uint32_t took;
    if (!flash_ok)
        return -1;
    if (!set && !(set = fxnv_part(FEXT_NV_SET, &set_n)))
        set_n = 0;
    if (!nv && !(nv = fxnv_part(FEXT_NV_STACK, &nv_n)))
        nv_n = 0;
    if (set_n > FXNV_SET_MAX || nv_n > FXNV_NV_MAX)
        return -1;
    memcpy(st_buf + FXNV_NV, nv, nv_n);           /* (from the flash or the module: never st_buf) */
    memcpy(st_buf + FXNV_SET, set, set_n);
    memset(st_buf, 0xFF, FXNV_SET);
    h->magic = FXNV_MAGIC;
    h->set_len = set_n;
    h->set_crc = fext_crc32(0, st_buf + FXNV_SET, set_n);
    h->nv_len = nv_n;
    h->nv_crc = fext_crc32(0, st_buf + FXNV_NV, nv_n);
    h->hdr_crc = fext_crc32(0, st_buf, sizeof *h - 4u);
    if (fl_erase4k_quiet(FL_FEXT_NV, &took) || fl_write(FL_FEXT_NV + FXNV_SET, st_buf + FXNV_SET, FXNV_NV - FXNV_SET + nv_n) ||
        fl_write(FL_FEXT_NV, st_buf, sizeof *h)) {
        flog_line("fext: settings write failed");
        return -1;
    }
    return 0;
}
UI_SMALL static void fx_flush(void)
{
    if (fx_set_src) {
        const void *s = fx_set_src;
        fx_set_src = 0;
        fxnv_write(s, fx_set_n, 0, 0);
    }
}

/* the clocks and the analog block as found (a power-on and a restart from download mode differ: a radio's
 * calibration faults after one of them) */
UI_SMALL static void fext_regs_log(void)
{
    uint32_t r[FM1_CLOCK_NREGS], i, k, v;
    char b[40], *q;
    fm1_clock_regs(r);
    for (i = 0; i < FM1_CLOCK_NREGS; i += 3u) {
        q = fcat(b, "r");
        for (k = i; k < i + 3u && k < FM1_CLOCK_NREGS; k++)
            q = fhex(fcat(q, " "), r[k]);
        flog_line(b);
    }
    for (i = 0; i < 0x80u; i += 16u) {
        if (i == 0x40u || i == 0x50u || i == 0x60u)
            continue;
        q = b;
        *q++ = 'p';
        *q++ = "0123456789ABCDEF"[i >> 4];
        *q++ = '0';
        *q++ = ' ';
        for (k = i; k < i + 16u; k++) {
            v = fm1_p33_read(k);
            *q++ = "0123456789ABCDEF"[(v >> 4) & 15u];
            *q++ = "0123456789ABCDEF"[v & 15u];
        }
        *q = 0;
        flog_line(b);
    }
}

/* ---- the host's services (fext.h fext_host) */
static uint32_t fext_quiet_t4;
static uint32_t fh_ms(void)   /* fm1_ms stands while TIMER5 is masked (fext_quiet_set): from TIMER4 meanwhile */
{
    return fext_quiet ? fext_quiet_t + (fm1_ticks() - fext_quiet_t4) / (1000u * FM1_TICKS_PER_US) : fm1_ms;
}
UI_SMALL static void fh_wdt(void) { fm1_wdt_feed(); }
UI_SMALL static void fh_irq_attach(int index, int prio, void (*isr)(void))   /* JieLi's 0..2 above TIMER5's 4: 5..7 */
{
    uint32_t f;
    char b[40], *q = fcat(b, "irq ");
    *q++ = (char)('0' + index / 10 % 10);
    *q++ = (char)('0' + index % 10);
    *q++ = ' ';
    fhex(q, (uint32_t)(uintptr_t)isr);
    flog_line(b);
    fext_irqs = 1;
    f = irq_save();
    fm1_guard_unlock_top();
    fm1_irq_attach((uint32_t)index, isr, (uint32_t)(prio + 5 > 7 ? 7 : prio + 5));
    fm1_guard_lock_top();
    irq_restore(f);
}
UI_SMALL static void fh_stack_floor(uint32_t lo) { fm1_guard_stack_floor(lo); }
/* the trim's current corrupts what the flash reads: the audio and the input interrupts run from XIP, so they are
 * masked meanwhile (the flag alone keeps only the render out); the buffer zeroed first, the DMA loops silence */
UI_SMALL static void fext_quiet_set(int on)
{
    uint32_t f, b;
    f = irq_save();
    if (on && !fext_quiet) {
        fm1_irq_mask(FM1_IRQ_ALNK0);
        fm1_irq_mask(FM1_IRQ_TIMER5);
        for (b = 0; b < 2u * HALF_WORDS; b++)
            abuf[b] = 0;
    } else if (!on && fext_quiet) {
        fm1_irq_unmask(FM1_IRQ_ALNK0);
        fm1_irq_unmask(FM1_IRQ_TIMER5);
    }
    if (on && !fext_quiet) {
        fext_quiet_t = fm1_ms;
        fext_quiet_t4 = fm1_ticks();
    }
    fext_quiet = (uint8_t)(on != 0);
    irq_restore(f);
}
UI_SMALL static void fh_quiet(int on) { fext_quiet_set(on); }
UI_SMALL static int fh_nv_load(int id, void *dst, uint32_t max)
{
    uint32_t n;
    const uint8_t *p;
    if (id == FEXT_NV_SET && fx_set_src) {        /* (not written yet: as the module last gave them) */
        p = fx_set_src;
        n = fx_set_n;
    } else if ((id != FEXT_NV_SET && id != FEXT_NV_STACK) || !(p = fxnv_part(id, &n))) {
        return -1;
    }
    if (n < max)
        max = n;
    memcpy(dst, p, max);
    return (int)max;
}
UI_SMALL static int fh_nv_save(int id, const void *src, uint32_t n)   /* (the module's poll: the main loop) */
{
    if (id == FEXT_NV_SET) {                      /* when SLOOP is quiet (fext_service) */
        if (!n || n > FXNV_SET_MAX)
            return -1;
        fx_set_src = src;
        fx_set_n = n;
        fx_set_t = fm1_ms;
        return 0;
    }
    if (id != FEXT_NV_STACK || !n)
        return -1;
    flog_line("fext: nv saved");
    if (fx_set_src) {                             /* (the settings waiting go with it) */
        int r = fxnv_write(fx_set_src, fx_set_n, src, n);
        fx_set_src = 0;
        return r;
    }
    return fxnv_write(0, 0, src, n);
}
UI_SMALL static void fext_reboot(void)            /* as SLOOP's own resets (main.c UBOOT): its work first */
{
    if (fext)
        fext->stop();
    fx_flush();
    fm1_audio_stop();
#if FELUCCA_ARRANGER
    persist_flush_now();
#endif
    bootguard.pending = 0;                        /* intentional: not a failed boot */
    usb_detach();
    fm1_delay_ms(30);
    fm1_reboot();
}
UI_SMALL static void fh_say(const char *text)
{
    char b[40];
    fext_name_fold(b, text, sizeof b);            /* (UTF-8 as the font draws it: fext_text.h) */
    ui_message(b);
}
static const fext_host fext_host_table = {
    .abi_major = FEXT_ABI_MAJOR, .abi_minor = FEXT_ABI_MINOR, .size = sizeof(fext_host),
    .ms = fh_ms, .log = flog_line, .wdt_feed = fh_wdt, .irq_attach = fh_irq_attach, .stack_floor = fh_stack_floor,
    .quiet = fh_quiet, .nv_load = fh_nv_load, .nv_save = fh_nv_save, .reboot = fext_reboot, .say = fh_say,
};

/* ---- the loader */
static const uint8_t *fext_flash(uint32_t off, uint32_t n)   /* the unencrypted XIP window (persist_boot set it up) */
{
    return flash_ok && off >= 0x93000u && off <= 0x100000u && n <= 0x100000u - off ? fm1_xip_ptr(off) : 0;
}
extern char _fext_code_lo[], _fext_code_hi[], _fext_dma_lo[], _fext_dma_hi[], _fext_ram_lo[], _fext_ram_hi[];
UI_SMALL static int fext_load(void)
{
    char b[40];
    uint32_t i;
    fext_slot s = FEXT_SLOT;
    fext_init_fn init;
    if (fext)
        return 0;
    s.xip = (uint32_t)(uintptr_t)fm1_xip_ptr(FEXT_SLOT_OFF);
    fext_why = fext_check(fext_flash, &s, &fext_h);
    if (fext_why) {
        fcat(fcat(b, "fext: not loaded: "), fext_why);
        flog_line(b);
        return -1;
    }
    /* the windows this build keeps free (app_fext.ld) must be fext_host.h's */
    if ((uint32_t)(uintptr_t)_fext_code_lo != FEXT_CODE_LO || (uint32_t)(uintptr_t)_fext_code_hi != FEXT_CODE_HI ||
        (uint32_t)(uintptr_t)_fext_dma_lo != FEXT_DMA_LO || (uint32_t)(uintptr_t)_fext_dma_hi != FEXT_DMA_HI ||
        (uint32_t)(uintptr_t)_fext_ram_lo != FEXT_RAM_LO || (uint32_t)(uintptr_t)_fext_ram_hi != FEXT_RAM_HI) {
        fext_why = "host windows";
        flog_line("fext: not loaded: host windows");
        return -1;
    }
    fext_h.name[sizeof fext_h.name - 1u] = 0;
    fext_h.version[sizeof fext_h.version - 1u] = 0;
    fcat(fcat(fcat(fcat(b, "fext: found "), fext_h.name), " "), fext_h.version);
    flog_line(b);
    flog.running = 1;                             /* a crash from here on (the copy, the guard, init) bars the next
                                                   * boot's load */
    for (i = 0; i < FEXT_NCOPY; i++) {            /* .ram_text and .data in, .bss and .pool cleared */
        const fext_copy *c = &fext_h.copy[i];
        uint32_t *d = (uint32_t *)(uintptr_t)c->dst, k;
        const uint32_t *src = (const uint32_t *)(uintptr_t)c->src;
        for (k = 0; k < c->n / 4u; k++)
            d[k] = src ? src[k] : 0u;
    }
    fm1_guard_pc_widen(fext_h.xip + fext_h.size - 1u, fext_h.win[FEXT_W_CODE].hi > fext_h.win[FEXT_W_CODE].lo ?
                       fext_h.win[FEXT_W_CODE].hi - 1u : 0u);   /* the core may now run it */
    init = (fext_init_fn)(uintptr_t)fext_h.init;
    fhex(fcat(b, "fext: init at "), fext_h.init);
    flog_line(b);
    fext = init(&fext_host_table);
    if (!fext || fext->abi_major != FEXT_ABI_MAJOR || fext->size < sizeof(fext_api)) {
        fext = 0;
        flog.running = 0;
        fext_why = "init refused";
        flog_line("fext: init refused");
        return -1;
    }
    for (i = 0; i < 7u && fext_h.name[i] > ' ' && fext_h.name[i] < 0x7F; i++)   /* its pages' titles */
        fext_title[0][i] = fext_title[1][i] = fext_h.name[i];
    if (i) {
        fext_title[0][i] = 0;
        fext_title[1][i < 5u ? i : 4u] = 0;
        fcat(fext_title[1], " 2");
    }
    flog_line("fext: loaded");
    return 0;
}

/* at boot, after cpu1_boot (it arms the guard's code windows again: the module's are widened after) and before the
 * audio: what ended the last run, then the module */
UI_SMALL static void fext_boot(void)
{
    int crashed;
    flog_fresh();
    crashed = fm1_crash.magic == FM1_CRASH_MAGIC && fm1_crash.count != flog.crash_seen;
    flog.crash_seen = fm1_crash.count;
    if (crashed) {
        char b[40];
        fhex(fcat(fhex(fcat(b, "crash pc "), fm1_crash.pc), " rets "), fm1_crash.rets);
        flog_line(b);
    }
    fext_last_bad = (uint8_t)(flog.running && (crashed || (fm1_boot.p3_rst & 4u)));
    flog.running = 0;
    flog_line(fext_clock_fix == 1 ? "fext host: SLOOP boot, clock set to stock"
              : fext_clock_fix ? "fext host: SLOOP boot, clock NOT stock" : "fext host: SLOOP boot, clock stock");
    flog_line(fext_power_fix ? "fext host: power set to stock (DC-DC on)" : "fext host: power stock already");
    fext_regs_log();
    if (flog.noload) {
        flog.noload = 0;
        fext_why = "held (SysEx 06 01)";
        flog_line("fext: not loaded: held (SysEx 06 01)");
        return;
    }
    if (fext_last_bad) {
        fext_why = "last run crashed";
        flog_line("fext: last run crashed: not loaded");
        return;
    }
    fext_load();
}

/* ---- MIDI. SLOOP's out, from the audio interrupt (the ring's one producer): to each port the module asks for
 * (ports_out, read into fext_out_ports by the main loop), the port as the packet's cable. No call into it */
static fext_ring *volatile fext_out_ring;
static volatile uint32_t fext_out_ports, fext_live;
static void fext_midi_out(uint32_t pkt)
{
    fext_ring *r = fext_out_ring;
    uint32_t p, m;
    if (!r)
        return;
    for (p = 0, m = fext_out_ports; m && p < 16u; p++, m >>= 1) {
        if (!(m & 1u) || r->w - r->r >= r->n)
            continue;
        r->buf[r->w % r->n] = (pkt & ~0xF0u) | p << 4;
        RING_PUBLISH();
        r->w++;
    }
}
/* the module's MIDI in into SLOOP's input ring, the USB host's way (usb_host_desc.h uh_midi_ok: channel messages and
 * clock / start / continue / stop; no SysEx), with the interrupts off (TIMER5 fills the ring too). 8 slots stay for
 * the jack; what does not fit waits in the module's ring */
UI_SMALL static void fext_midi_in(void)
{
    fext_ring *r = fext->midi_in;
    while (r->r != r->w && MQ - (mi_w - mi_r) > 8u) {
        uint32_t p = r->buf[r->r % r->n] & ~0xF0u, cin = p & 15u, st = (p >> 8) & 0xFFu, f;
        RING_PUBLISH();
        r->r++;
        if (cin == 0xFu ? !(st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu)
                        : !(cin >= 8u && cin <= 0xEu && (st >> 4) == cin && !((p >> 16) & 0x80u) &&
                            (cin == 0xCu || cin == 0xDu || !(p & 0x80000000u))))
            continue;
        if (cin == 0xFu)
            p = 0xFu | st << 8;                   /* (cable 0: seq.c mclk_event takes it as USB's) */
        f = irq_save();
        midi_enqueue(p, 2u);
        irq_restore(f);
    }
}

/* ---- the main loop: every pass (and in its wait for the next frame) */
UI_SMALL static int fext_view(void)              /* the module's page shown: 0, 1, or -1 */
{
    if (ui.home)
        return -1;
    return cur_page()->graph == GR_EXT ? 0 : cur_page()->graph == GR_EXT2 ? 1 : -1;
}
UI_SMALL static void fext_service(void)
{
    uint32_t live;
    if (fx_set_src && fm1_ms - fx_set_t > 1500u && !song.playing)
        fx_flush();                               /* (an erase silences the audio a moment: not while it plays) */
    if (fext_quiet && fh_ms() - fext_quiet_t > 3000u) {
        fext_quiet_set(0);                        /* (the module's quiet(0) never came: the render back anyway) */
        flog_line("fext: quiet too long: ended");
    }
    if (!fext) {
        fext_out_ring = 0;
        return;
    }
    fext->poll(fext_view());
    fext_midi_in();
    live = *fext->ports_live;
    if (fext_live & ~live)
        midi_usb_lost = 1;                        /* (a port's link went: its notes end, as a USB device's: seq.c) */
    fext_live = live;
    fext_out_ports = *fext->ports_out;
    fext_out_ring = fext->midi_out;
}

/* ---- USR4: the module's while its header is there (or a write of one is under way) */
UI_SMALL static int fext_slot_taken(void)
{
    return fext_slot_busy || (flash_ok && *(const uint32_t *)fm1_xip_ptr(FEXT_SLOT_OFF) == FEXT_MAGIC);
}

/* ---- the module over USB-MIDI: F0 7D 46 42 op ... F7 (see the top) */
UI_SMALL static void fext_answer(uint8_t op, const char *text, int err)
{
    uint8_t f[72];
    uint32_t n = 0;
    f[n++] = 0xF0; f[n++] = 0x7D; f[n++] = 0x46; f[n++] = 0x42; f[n++] = op;
    if (text)
        while (*text && n < sizeof f - 1u)
            f[n++] = (uint8_t)(*text++ & 0x7Fu);
    else
        f[n++] = (uint8_t)(err & 0x7F);
    f[n++] = 0xF7;
    ota_wire_send(f, n);
}
UI_SMALL static void fext_log_send(const char *t)
{
    uint8_t f[48];
    uint32_t n = 0;
    f[n++] = 0xF0; f[n++] = 0x7D; f[n++] = 0x4C; f[n++] = 0x47;
    while (*t && n < sizeof f - 1u)
        f[n++] = (uint8_t)(*t++ & 0x7Fu);
    f[n++] = 0xF7;
    ota_wire_send(f, n);
}
/* the module may be rewritten or removed: not while its code runs on its own (an interrupt it attached); loaded but
 * idle, it is let go (not called again) */
UI_SMALL static int fext_let_go(void)
{
    if (fext && fext_irqs)
        return -1;
    if (fext) {
        fext->stop();
        fext = 0;
        fext_out_ring = 0;
        fext_why = "rewritten";
        flog_line("fext: let go for a write");
    }
    return 0;
}
/* a write frame's payload, decoded already (raw: st_buf): into the slot */
UI_SMALL static int fext_write(uint32_t off, const uint8_t *raw, int got)
{
    uint32_t len, i, took;
    const uint8_t *chk;
    if (got < 0)
        return 5;
    len = (uint32_t)got;
    if (!len || !fext_in(off, len, FEXT_SLOT_OFF, FEXT_SLOT_OFF + FEXT_SLOT_MAX))
        return 1;
    if (fext_let_go())
        return 6;                                 /* its code runs from here: restart without it first */
    if (!fext_slot_busy) {                        /* USR4's sample out of use first (editor.c ed_smp_erase) */
        fext_slot_busy = 1;
        usr_nz[3] = 0;
        for (i = 0; i < 16u; i++)
            usr_zone[3][i].n = 0;
        ed_smp_open[3] = 0;
        flog_line("fext: USR4 taken for the module");
    }
    if (!(off & 0xFFFu) && fl_erase4k_quiet(off, &took))
        return 2;
    if (fl_write(off, raw, len))
        return 3;
    chk = fm1_xip_ptr(off);
    for (i = 0; i < len; i++)
        if (chk[i] != raw[i])
            return 4;
    return 0;
}
UI_SMALL static void fext_log_frame(void)
{
    char b[48];
    uint32_t k, n;
    flog_fresh();
    if (fm1_crash.magic == FM1_CRASH_MAGIC) {
        fhex(fcat(fhex(fcat(fhex(fcat(fhex(fcat(b, "crash n "), fm1_crash.count), " vec "), fm1_crash.vec), " pc "),
                       fm1_crash.pc), " rets "), fm1_crash.rets);
        fext_log_send(b);
        fhex(fcat(fhex(fcat(fhex(fcat(b, "emu "), fm1_crash.emu), " dbg "), fm1_crash.dbg), " sp "), fm1_crash.sp);
        fext_log_send(b);
        fhex(fcat(fhex(fcat(fhex(fcat(fhex(fcat(b, "etm "), fm1_crash.etm[0]), " "), fm1_crash.etm[1]), " "),
                       fm1_crash.etm[2]), " "), fm1_crash.etm[3]);
        fext_log_send(b);
    } else {
        fext_log_send("no crash recorded");
    }
    fhex(fcat(b, "boot p3_rst "), fm1_boot.p3_rst);
    fext_log_send(b);
    n = flog.w < FLOG_N ? flog.w : FLOG_N;
    for (k = 0; k < n; k++)
        fext_log_send(fext_flog_l.line[(flog.w - n + k) % FLOG_N]);
    fext_log_send("end");
}
UI_SMALL static void fext_status_text(char *b, uint32_t max)   /* the module's line 0, or why there is none */
{
    fext_line l;
    if (!fext) {
        fcat(fcat(b, "NO MODULE: "), fext_why ? fext_why : "none");
        return;
    }
    if (!fext->line(0, 0, &l))
        l.text[0] = 0;
    l.text[sizeof l.text - 1u] = 0;
    fext_name_fold(b, l.text, max);
}
UI_SMALL static void fext_info(void)             /* op 10: where the module goes */
{
    uint8_t raw[40], f[56];
    uint32_t v[11], i, n = 0;
    v[0] = FEXT_ABI_MAJOR | FEXT_ABI_MINOR << 16;
    v[1] = FEXT_SLOT_OFF;
    v[2] = FEXT_SLOT_MAX;
    v[3] = (uint32_t)(uintptr_t)fm1_xip_ptr(FEXT_SLOT_OFF);
    for (i = 0; i < FEXT_NW; i++) {
        v[4 + 2 * i] = FEXT_SLOT.win[i].lo;
        v[5 + 2 * i] = FEXT_SLOT.win[i].hi;
    }
    memcpy(raw, v, 40);
    f[n++] = 0xF0; f[n++] = 0x7D; f[n++] = 0x46; f[n++] = 0x42; f[n++] = 10;
    n += fext_pack(raw, 40, f + n);
    f[n++] = 0xF7;
    ota_wire_send(f, n);
}
/* the main loop, between the editor's frames (ed_service) and the update's (ota_service): "FB" and "LG" */
UI_SMALL static void fext_sysex(void)
{
    const uint8_t *p;
    uint32_t n, off = 0;
    uint8_t a[8] = {0}, in[32];
    int got = -1;
    char b[64];
    if (!ota_frame_get(&p, &n))
        return;
    if (n >= 3u && p[0] == 0x7Du && p[1] == 0x4Cu && p[2] == 0x47u) {
        ota_frame_done();
        fext_log_frame();
        return;
    }
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x46u || p[2] != 0x42u)
        return;                                   /* (not ours: ota_service's) */
    for (off = 0; off < 8u && off + 3u < n; off++)
        a[off] = p[3u + off];                     /* op and its arguments */
    for (off = 0; off < sizeof in && off + 4u < n; off++)
        in[off] = p[4u + off];                    /* (op 7's bytes) */
    if (a[0] == 2u)
        got = fext_unpack(p + 4, n - 4u, &off, st_buf, 256u);
    else
        off = n > 4u ? n - 4u : 0u;               /* (op 7: how many) */
    ota_frame_done();                             /* (free before the answer: the next frame may come in) */
    b[0] = 0;
    switch (a[0]) {
    case 1:
        fext_status_text(b, sizeof b);
        fext_answer(1, b, 0);
        break;
    case 2:
        fext_answer(2, 0, fext_write(off, st_buf, got));
        break;
    case 3:
        fext_answer(3, fext_load() == 0 ? "loaded" : fext_why, 0);
        break;
    case 5: {
        uint32_t took;
        if (fext_let_go()) {
            fext_answer(5, "running: restart first", 0);
            break;
        }
        fext_why = "removed";
        fext_answer(5, fl_erase4k_quiet(FEXT_SLOT_OFF, &took) ? "erase failed" : "removed", 0);
        fext_slot_busy = 0;                       /* (USR4 takes a sample again: the editor's upload) */
        break;
    }
    case 6:
        flog.noload = a[1] == 1u;
        flog_line(flog.noload ? "fext: reboot (SysEx), no load" : "fext: reboot (SysEx)");
        fext_answer(6, "rebooting", 0);
        fm1_delay_ms(50);                         /* (the answer out first) */
        fext_reboot();
        break;
    case 7:
        if (!fext) {
            fext_answer(7, "no module", 0);
            break;
        }
        {
            char t[48];
            int k = fext->control(in, off < sizeof in ? off : sizeof in, t, sizeof t);
            t[k > 0 && k < (int)sizeof t ? k : 0] = 0;
            fext_name_fold(b, t, sizeof b);
        }
        fext_answer(7, b, 0);
        break;
    case 9:                                       /* (a test: a knob of the module's pages: page, column, steps + 64) */
        fext_knob(a[1], a[2], (int32_t)a[3] - 64);
        fext_status_text(b, sizeof b);
        fext_answer(9, b, 0);
        break;
    case 10:
        fext_info();
        break;
    default:
        fext_answer(a[0] & 0x7Fu, "?", 0);
        break;
    }
}

/* ---- the module's pages (params.c PAGES, GR_EXT / GR_EXT2): its columns, knobs and lines, drawn in SLOOP's style */
UI_SMALL static uint32_t fext_pages(void) { return fext ? (fext->pages > 1u ? 2u : 1u) : 1u; }
UI_SMALL static int fext_col_used(uint32_t page2, uint32_t col)
{
    fext_col c;
    if (!fext || page2 >= fext_pages())
        return 0;
    fext->column(page2, col, &c);
    return c.label[0] != 0;
}
UI_SMALL static void fext_knob(uint32_t page2, uint32_t col, int32_t steps)
{
    if (fext && page2 < fext_pages() && col < 4u)
        fext->knob(page2, col, steps);
}
UI_SMALL static void fext_columns(uint32_t page2)
{
    uint32_t c;
    for (c = 0; c < 4u; c++) {
        fext_col k;
        char l[8], v[24], u[4];
        if (!fext || page2 >= fext_pages()) {
            draw_column(c, "", "", "", C_HI, -1, ICON_AUTO);
            continue;
        }
        fext->column(page2, c, &k);
        k.label[sizeof k.label - 1u] = k.value[sizeof k.value - 1u] = k.unit[sizeof k.unit - 1u] = 0;
        fext_name_fold(l, k.label, sizeof l);
        fext_name_fold(v, k.value, sizeof v);
        fext_name_fold(u, k.unit, sizeof u);
        draw_column(c, l, v, u, k.style == FEXT_S_DIM ? C_DIM : k.style == FEXT_S_BRIGHT ? C_HI : VAL(c), -1, ICON_AUTO);
    }
}
/* the middle: the module's lines, 17 pixels apart (a gap of 4 above one that asks); a list's items indented, with a
 * cursor and a star before them, a right-aligned text after. Without a module: why, and the slot */
UI_SMALL static void fext_graph(uint32_t page2)
{
    char b[48];
    uint32_t i;
    int32_t y = 4;
    if (!fext) {
        fext_status_text(b, sizeof b);
        b[26] = 0;
        cv_text(4, 4, &FONT_S, b, C_HI);
        cv_text(4, 21, &FONT_S, fext_slot_taken() ? "USR4: MODULE NOT LOADED" : "PUT A MODULE IN USR4", C_DIM);
        return;
    }
    for (i = 0; i < 8u && y < 120; i++, y += 17) {
        fext_line l;
        int item;
        if (!fext->line(page2, i, &l))
            break;
        l.text[sizeof l.text - 1u] = l.right[sizeof l.right - 1u] = 0;
        item = (l.mark & FEXT_L_ITEM) != 0;
        if (l.gap)
            y += 4;
        fext_name_fold(b, l.text, sizeof b);
        b[item ? 16 : 26] = 0;
        if (l.mark & FEXT_L_CURSOR)
            cv_rect(4, y + 6, 3, 3, C_WHITE);
        if (l.mark & FEXT_L_STAR)
            cv_text(8, y, &FONT_S, "*", C_HI);
        cv_text(item ? 14 : 4, y, &FONT_S, b,
                l.style == FEXT_S_BRIGHT ? C_WHITE : l.style == FEXT_S_DIM ? C_DIM : item ? C_GRAY : C_HI);
        if (l.right[0]) {
            fext_name_fold(b, l.right, 9);
            cv_text(236 - text_w(&FONT_S, b), y, &FONT_S, b, C_DIM);
        }
    }
}
UI_SMALL static uint32_t fext_graph_sig(uint32_t page2)
{
    uint32_t h = 0x9E3779B9u + (fext != 0) * 7u, i;
    if (!fext) {
        char b[48];
        fext_status_text(b, sizeof b);
        return str_hash(h, b) + (uint32_t)fext_slot_taken();
    }
    for (i = 0; i < 8u; i++) {
        fext_line l;
        if (!fext->line(page2, i, &l))
            break;
        l.text[sizeof l.text - 1u] = l.right[sizeof l.right - 1u] = 0;
        h = str_hash(str_hash(h, l.text), l.right) * 31u + l.style * 7u + l.mark * 3u + l.gap;
    }
    return h;
}
