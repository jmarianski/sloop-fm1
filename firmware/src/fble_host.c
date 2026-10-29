/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Jacek Mariański
 * (the log and the interrupt attach follow MIDIMON's fm1-base device/radio.c, Copyright (C) 2026 Charles Vestal,
 *  MPL-2.0; the central's FIRST / CHOOSE / RECONNECT logic follows MIDIMON's app/ble.c, Copyright (C) 2026 ignis32,
 *  MPL-2.0) */
/* SLOOP's socket for fble, the Bluetooth MIDI module (FELUCCA_FBLE, tools/build.py --fble; fble.h). Part of
 * felucca.c, after editor.c. SLOOP links nothing of JieLi's: the module is written into USR4 on its own and found at
 * boot.
 *   at boot (fble_boot, after cpu1_boot): the header checked (fble_load.h, ABI 0.3: one segment), the module's RAM parts
 *     copied into the windows app_fble.ld keeps free, the guard's code windows widened to it, init called. Not when
 *     the last run with the module crashed or the watchdog fired (then by hand: SysEx op 3)
 *   the main loop (fble_service): the radio started when GLO > BLE > RADIO is ON, the module polled, its MIDI in into
 *     SLOOP's input ring (the path USB-MIDI and the jack take), the central's connecting (FIRST, CHOOSE), the pages'
 *     settings kept in flash (sector 0xFB000, with the module's NV)
 *   the audio interrupt: SLOOP's MIDI out copied to the module's out ring (GLO > BLE 2 > OUT), no call into it
 *   SysEx F0 7D 46 42 op ... F7 ("FB": fble/tools/fble_push.py, unchanged from MIDIMON's host-stub):
 *     1                  status: F0 7D 46 42 01 <text> F7
 *     2 o0 o1 o2 data    write: flash offset (7 bits a byte, low first) inside USR4, then up to 256 bytes 7-bit packed
 *                        (each group: a byte of high bits, then up to 7 bytes); a sector's first write erases it.
 *                        Answer F0 7D 46 42 02 err F7 (0 ok, 1 range, 2 erase, 3 write, 4 verify, 5 format, 6 the
 *                        module runs: restart first)
 *     3                  load now (if not loaded); 4 start the radio; 5 remove the module (its header's sector)
 *     6 f                reboot; f = 1: the next boot does not load the module (to write a new one)
 *     7 op a0 a1 a2      a command for the module (fble.h FBLE_CMD_*; the argument 7 bits a byte, low first)
 *     8                  the radio: state, the central's device state, the computer's link, the scan
 *   the log: F0 7D 4C 47 F7 ("LG") answers line by line, then "end". SLOOP's editor speaks F0 7D 46 4C ("FL"), the
 *     update F0 22 24 35 ..: no clash */

/* ---- the log: the control words in .noinit (24 B), the lines in .fble_log (app_fble.ld: RAM, not zeroed). Not
 * static: a file-local object would let the compiler fold the magic checks (fm1_irq.h fm1_crash) */
#define FLOG_N 24
#define FLOG_MAGIC 0x534C4246u                    /* "FBLS" */
struct fble_flog { uint32_t magic, w, crash_seen, running, repeat, noload; };
struct fble_flog_lines { uint32_t magic; char line[FLOG_N][40]; };
struct fble_flog fble_flog __attribute__((section(".noinit")));
struct fble_flog_lines fble_flog_l __attribute__((section(".fble_log")));
#define flog fble_flog
UI_SMALL static void flog_fresh(void)
{
    if (flog.magic != FLOG_MAGIC || fble_flog_l.magic != FLOG_MAGIC || flog.w > 100000u) {
        flog.magic = fble_flog_l.magic = FLOG_MAGIC;
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
        char *p = fble_flog_l.line[(flog.w - 1u) % FLOG_N];
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
    d = fble_flog_l.line[flog.w % FLOG_N];
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
static const fble_api *fble;                      /* its calls, once init has answered */
static fble_header fble_h;                        /* its header, as checked */
static const char *fble_why = "not looked";       /* why there is none */
static uint8_t fble_started, fble_last_bad, fble_usr4_busy;
static uint32_t fble_quiet_t;

/* ---- the pages' settings and the module's NV: one sector, FL_FBNV_LO (0xFB000; fm1_flash.h lets the fble build
 * write it). A header (written last) with the CRCs, the settings at +0x20, the module's NV at +0x100. A write cut
 * short leaves no header: the defaults then (the radio OFF), and a new random address for the module */
enum { FB_C_OFF, FB_C_FIRST, FB_C_CHOOSE };
typedef struct {
    uint8_t radio, central, pc, recon, out, have, rsv[2];
    uint8_t addr[6];                              /* CHOOSE: the device chosen, by its address */
    char name[22];
} fb_set_t;
static fb_set_t fb_set;
typedef struct { uint32_t magic, set_len, set_crc, nv_len, nv_crc, hdr_crc; } fbnv_hdr;
#define FBNV_MAGIC 0x564E4246u                    /* "FBNV" */
#define FBNV_SET 0x20u
#define FBNV_NV 0x100u
#define FBNV_NV_MAX 1536u
_Static_assert(sizeof(fb_set_t) <= FBNV_NV - FBNV_SET && FBNV_NV + FBNV_NV_MAX <= ST_PAYLOAD_MAX, "the sector's parts");
static uint8_t fb_dirty;
static uint32_t fb_dirty_t;

static const fbnv_hdr *fbnv(void)                 /* the header, if whole */
{
    const fbnv_hdr *h = (const fbnv_hdr *)fm1_xip_ptr(FL_FBNV_LO);
    if (!flash_ok || h->magic != FBNV_MAGIC || fble_crc32(0, (const uint8_t *)h, sizeof *h - 4u) != h->hdr_crc)
        return 0;
    return h;
}
UI_SMALL static void fb_set_defaults(void)
{
    memset(&fb_set, 0, sizeof fb_set);
    fb_set.central = FB_C_FIRST;
    fb_set.recon = 1;
}
UI_SMALL static void fbnv_read_set(void)
{
    const fbnv_hdr *h = fbnv();
    const uint8_t *p = fm1_xip_ptr(FL_FBNV_LO + FBNV_SET);
    fb_set_defaults();
    if (h && h->set_len == sizeof fb_set && fble_crc32(0, p, sizeof fb_set) == h->set_crc) {
        memcpy(&fb_set, p, sizeof fb_set);
        fb_set.name[sizeof fb_set.name - 1u] = 0;
        if (fb_set.central > FB_C_CHOOSE)
            fb_set.central = FB_C_FIRST;
    }
}
/* the sector again, from st_buf (the main loop's scratch, storage.c): the settings as they are now, and the NV given
 * (nv 0: the one there). Erase, the parts, the header last */
UI_SMALL static int fbnv_write(const void *nv, uint32_t nv_len)
{
    const fbnv_hdr *h = fbnv();
    fbnv_hdr *n = (fbnv_hdr *)st_buf;
    uint32_t took;
    if (!flash_ok)
        return -1;
    if (!nv) {                                    /* (kept: read before the erase) */
        nv_len = h && h->nv_len <= FBNV_NV_MAX ? h->nv_len : 0u;
        nv = fm1_xip_ptr(FL_FBNV_LO + FBNV_NV);
        if (nv_len && fble_crc32(0, nv, nv_len) != h->nv_crc)
            nv_len = 0;
    }
    if (nv_len > FBNV_NV_MAX)
        return -1;
    memcpy(st_buf + FBNV_NV, nv, nv_len);         /* (from the flash or the module: never st_buf) */
    memset(st_buf, 0xFF, FBNV_NV);
    memcpy(st_buf + FBNV_SET, &fb_set, sizeof fb_set);
    n->magic = FBNV_MAGIC;
    n->set_len = sizeof fb_set;
    n->set_crc = fble_crc32(0, (const uint8_t *)&fb_set, sizeof fb_set);
    n->nv_len = nv_len;
    n->nv_crc = fble_crc32(0, st_buf + FBNV_NV, nv_len);
    n->hdr_crc = fble_crc32(0, st_buf, sizeof *n - 4u);
    if (fl_erase4k_quiet(FL_FBNV_LO, &took) ||
        fl_write(FL_FBNV_LO + FBNV_SET, st_buf + FBNV_SET, FBNV_NV - FBNV_SET + nv_len) ||
        fl_write(FL_FBNV_LO, st_buf, sizeof *n)) {
        flog_line("fble: settings write failed");
        return -1;
    }
    return 0;
}
UI_SMALL static void fb_changed(void)
{
    fb_dirty = 1;
    fb_dirty_t = fm1_ms;
}
UI_SMALL static void fb_flush(void)
{
    if (fb_dirty) {
        fb_dirty = 0;
        fbnv_write(0, 0);
    }
}

/* the clocks and the analog block as found (a power-on and a restart from download mode differ: the radio's
 * calibration faults after one of them) */
UI_SMALL static void fble_regs_log(void)
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

/* ---- the host's services (fble.h fble_host) */
static uint32_t fble_quiet_t4;
static uint32_t fh_ms(void)   /* fm1_ms stands while TIMER5 is masked (fble_quiet_set): from TIMER4 meanwhile */
{
    return fble_quiet ? fble_quiet_t + (fm1_ticks() - fble_quiet_t4) / (1000u * FM1_TICKS_PER_US) : fm1_ms;
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
    f = irq_save();
    fm1_guard_unlock_top();
    fm1_irq_attach((uint32_t)index, isr, (uint32_t)(prio + 5 > 7 ? 7 : prio + 5));
    fm1_guard_lock_top();
    irq_restore(f);
}
UI_SMALL static void fh_stack_floor(uint32_t lo) { fm1_guard_stack_floor(lo); }
/* the trim's current corrupts what the flash reads: the audio and the input interrupts run from XIP, so they are
 * masked meanwhile (the flag alone keeps only the render out); the buffer zeroed first, the DMA loops silence */
UI_SMALL static void fble_quiet_set(int on)
{
    uint32_t f, b;
    f = irq_save();
    if (on && !fble_quiet) {
        fm1_irq_mask(FM1_IRQ_ALNK0);
        fm1_irq_mask(FM1_IRQ_TIMER5);
        for (b = 0; b < 2u * HALF_WORDS; b++)
            abuf[b] = 0;
    } else if (!on && fble_quiet) {
        fm1_irq_unmask(FM1_IRQ_ALNK0);
        fm1_irq_unmask(FM1_IRQ_TIMER5);
    }
    if (on && !fble_quiet) {
        fble_quiet_t = fm1_ms;
        fble_quiet_t4 = fm1_ticks();
    }
    fble_quiet = (uint8_t)(on != 0);
    irq_restore(f);
}
UI_SMALL static void fh_quiet(int on) { fble_quiet_set(on); }
UI_SMALL static int fh_nv_load(void *dst, uint32_t max)
{
    const fbnv_hdr *h = fbnv();
    const uint8_t *p = fm1_xip_ptr(FL_FBNV_LO + FBNV_NV);
    if (!h || h->nv_len > FBNV_NV_MAX || !h->nv_len || fble_crc32(0, p, h->nv_len) != h->nv_crc)
        return -1;
    if (h->nv_len < max)
        max = h->nv_len;
    memcpy(dst, p, max);
    return (int)max;
}
UI_SMALL static int fh_nv_save(const void *src, uint32_t n)   /* (the module's poll: the main loop) */
{
    flog_line("fble: nv saved");
    return n ? fbnv_write(src, n) : -1;
}
UI_SMALL static void fble_reboot(void)            /* as SLOOP's own resets (main.c UBOOT): its work first */
{
    fb_flush();
    fm1_audio_stop();
#if FELUCCA_ARRANGER
    persist_flush_now();
#endif
    bootguard.pending = 0;                        /* intentional: not a failed boot */
    usb_detach();
    fm1_delay_ms(30);
    fm1_reboot();
}
UI_SMALL static void fh_tap(const uint8_t *b, uint32_t len, int dev) { (void)b; (void)len; (void)dev; }   /* (no monitor) */
static const fble_host fble_host_table = {
    .abi_major = FBLE_ABI_MAJOR, .abi_minor = FBLE_ABI_MINOR, .size = sizeof(fble_host),
    .ms = fh_ms, .log = flog_line, .wdt_feed = fh_wdt, .irq_attach = fh_irq_attach, .stack_floor = fh_stack_floor,
    .quiet = fh_quiet, .nv_load = fh_nv_load, .nv_save = fh_nv_save, .reboot = fble_reboot, .tap = fh_tap,
};

/* ---- the loader */
static const uint8_t *fble_flash(uint32_t off, uint32_t n)   /* the unencrypted XIP window (persist_boot set it up) */
{
    return flash_ok && off >= 0x93000u && off <= 0x100000u && n <= 0x100000u - off ? fm1_xip_ptr(off) : 0;
}
static uint32_t fble_xip(uint32_t off) { return (uint32_t)(uintptr_t)fm1_xip_ptr(off); }
extern char _fble_rt_lo[], _fble_rt_hi[], _fble_low_lo[], _fble_low_hi[], _fble_any_lo[], _fble_any_hi[];
UI_SMALL static int fble_load(void)
{
    char b[40];
    uint32_t i;
    fble_init_fn init;
    if (fble)
        return 0;
    fble_why = fble_check(fble_flash, fble_xip, &fble_h);
    if (fble_why && str_eq(fble_why, "segment 1"))
        fble_why = "segment 1: USR3 is SLOOP's";   /* (a module of two segments, ABI 0.2: USR3 holds samples here) */
    if (fble_why) {
        fcat(fcat(b, "fble: not loaded: "), fble_why);
        flog_line(b);
        return -1;
    }
    /* the windows this build keeps free (app_fble.ld) must be fble.h's */
    if ((uint32_t)(uintptr_t)_fble_rt_lo != FBLE_RT_LO || (uint32_t)(uintptr_t)_fble_rt_hi != FBLE_RT_HI ||
        (uint32_t)(uintptr_t)_fble_low_lo != FBLE_LOW_LO || (uint32_t)(uintptr_t)_fble_low_hi != FBLE_LOW_HI ||
        (uint32_t)(uintptr_t)_fble_any_lo != FBLE_ANY_LO || (uint32_t)(uintptr_t)_fble_any_hi != FBLE_ANY_HI) {
        fble_why = "host windows";
        flog_line("fble: not loaded: host windows");
        return -1;
    }
    fcat(fcat(fcat(fcat(b, "fble: found "), fble_h.name), " "), fble_h.version);
    flog_line(b);
    flog.running = 1;                             /* a crash from here on (the copy, the guard, init) bars the next
                                                   * boot's load */
    for (i = 0; i < FBLE_NCOPY; i++) {            /* .ram_text and .data in, .bss and .pool cleared */
        const fble_copy *c = &fble_h.copy[i];
        uint32_t *d = (uint32_t *)(uintptr_t)c->dst, k;
        const uint32_t *s = (const uint32_t *)(uintptr_t)c->src;
        for (k = 0; k < c->n / 4u; k++)
            d[k] = s ? s[k] : 0u;
    }
    fm1_guard_pc_widen(fble_h.xip_hi - 1u, FBLE_RT_HI - 1u);   /* the core may now run it */
    init = (fble_init_fn)(uintptr_t)fble_h.init;
    fhex(fcat(b, "fble: init at "), fble_h.init);
    flog_line(b);
    fble = init(&fble_host_table, (void *)(uintptr_t)FBLE_LOW_LO, FBLE_LOW_HI - FBLE_LOW_LO,
                (void *)(uintptr_t)FBLE_ANY_LO, FBLE_ANY_HI - FBLE_ANY_LO);
    if (!fble || fble->abi_major != FBLE_ABI_MAJOR || fble->size < sizeof(fble_api)) {
        fble = 0;
        flog.running = 0;
        fble_why = "init refused";
        flog_line("fble: init refused");
        return -1;
    }
    flog_line("fble: loaded, radio off");
    return 0;
}
UI_SMALL static void fble_start_now(void)
{
    if (!fble || fble_started)
        return;
    fble_started = 1;
    flog_line(fble->start() == 0 ? "fble: start" : "fble: start failed");
}

/* at boot, after cpu1_boot (it arms the guard's code windows again: the module's are widened after) and before the
 * audio: what ended the last run, the settings, then the module */
UI_SMALL static void fble_boot(void)
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
    fble_last_bad = (uint8_t)(flog.running && (crashed || (fm1_boot.p3_rst & 4u)));
    flog.running = 0;
    flog_line(fble_clock_fix == 1 ? "fble host: SLOOP boot, clock set to stock"
              : fble_clock_fix ? "fble host: SLOOP boot, clock NOT stock" : "fble host: SLOOP boot, clock stock");
    flog_line(fble_power_fix ? "fble host: power set to stock (DC-DC on)" : "fble host: power stock already");
    fble_regs_log();
    fbnv_read_set();
    if (flog.noload) {
        flog.noload = 0;
        fble_why = "held (SysEx 06 01)";
        flog_line("fble: not loaded: held (SysEx 06 01)");
        return;
    }
    if (fble_last_bad) {
        fble_why = "last run crashed";
        flog_line("fble: last run crashed: not loaded");
        return;
    }
    fble_load();
}

/* ---- the rings */
UI_SMALL static void fble_cmd(uint32_t op, uint32_t arg)
{
    fble_ring *r;
    if (!fble)
        return;
    r = fble->cmd;
    if (r->w - r->r >= r->n)
        return;
    r->buf[r->w % r->n] = FBLE_CMD(op, arg);
    RING_PUBLISH();
    r->w++;
}
/* SLOOP's MIDI out, from the audio interrupt (the ring's one producer): to the device (cable 1) and the computer
 * (cable 0), when connected and GLO > BLE 2 > OUT is ON. fble_out_* are the main loop's view (fble_service): no call
 * into the module from here */
static fble_ring *volatile fble_out_ring;
static volatile uint8_t fble_out_dev, fble_out_pc;
static void fble_midi_out(uint32_t pkt)
{
    fble_ring *r = fble_out_ring;
    uint32_t l;
    if (!r)
        return;
    for (l = 0; l < 2u; l++) {
        if (!(l ? fble_out_dev : fble_out_pc) || r->w - r->r >= r->n)
            continue;
        r->buf[r->w % r->n] = (pkt & ~0xF0u) | l << 4;
        RING_PUBLISH();
        r->w++;
    }
}
/* the module's MIDI in into SLOOP's input ring, the USB host's way (usb_host_desc.h uh_midi_ok: channel messages and
 * clock / start / continue / stop; no SysEx), with the interrupts off (TIMER5 fills the ring too). 8 slots stay for
 * the jack; what does not fit waits in the module's ring */
UI_SMALL static void fble_midi_in(void)
{
    fble_ring *r = fble->midi_in;
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

/* ---- the central (MIDIMON app/ble.c's ble_task, ignis32): FIRST connects to the first device the scan sees,
 * CHOOSE to the one chosen (by its address), with RECONNECT again when it goes and at power-on */
static struct {
    uint8_t on;                                   /* the module told: enabled (radio ON) */
    uint8_t ready_seen, try_, cursor, dev, pc;
    uint32_t wait, scan_t;
    char cname[22];                               /* the device connecting / connected */
} fb;
UI_SMALL static int fble_ready(void) { return fble && fble_started && fble->state() == FBLE_READY; }
UI_SMALL static int fble_found(int i, char *name, int *rssi, uint8_t *addr)
{
    char n[21];
    int r = 0;
    if (!fble_ready() || !fble->found(i, n, &r, addr))
        return 0;
    if (name)
        str_cpy(name, n, 21);
    if (rssi)
        *rssi = r;
    return 1;
}
UI_SMALL static int fble_count(void)
{
    int n = 0;
    while (n < 8 && fble_found(n, 0, 0, 0))
        n++;
    return n;
}
UI_SMALL static int fble_page_shown(void)
{
    return !ui.home && (cur_page()->graph == GR_BLE || cur_page()->graph == GR_BLE2);
}
UI_SMALL static void fble_connect_to(int i)
{
    if (!fble_found(i, fb.cname, 0, 0))
        return;
    fble_cmd(FBLE_CMD_CONNECT, (uint32_t)i);
    ui_say("BLE CONNECTING ", fb.cname);
}
/* the settings into the module (when READY: it takes commands then only) */
UI_SMALL static void fble_apply(void)
{
    fble_cmd(FBLE_CMD_ADVERTISE, fb_set.pc && (fble_h.feat & FBLE_F_PERIPHERAL));
    if (fb_set.central == FB_C_OFF) {
        fble_cmd(FBLE_CMD_SCAN, 0);
        if (fble->device() != FBLE_DEV_OFF)
            fble_cmd(FBLE_CMD_DISCONNECT, 0);
        fb.try_ = 0;
    }
}
UI_SMALL static void fble_task(void)
{
    uint32_t now = fm1_ms;
    int dev, pc, i;
    if (!fble_ready())
        return;
    if (!fb.ready_seen) {                         /* the stack is up: the settings in */
        fb.ready_seen = fb.on = 1;
        fb.try_ = fb_set.central == FB_C_CHOOSE && fb_set.recon && fb_set.have;
        fble_apply();
        ui_message("BLE: READY");
    }
    if (fb.on != fb_set.radio) {                  /* RADIO turned while the stack is up: links down / up */
        fb.on = fb_set.radio;
        if (!fb.on) {
            fble_cmd(FBLE_CMD_SCAN, 0);
            fble_cmd(FBLE_CMD_DISCONNECT, 0);
            fble_cmd(FBLE_CMD_ADVERTISE, 0);
        }
        fble_cmd(FBLE_CMD_ENABLE, fb.on);
        if (fb.on)
            fble_apply();
    }
    dev = fble->device();
    pc = fble->peer() == 2;
    if (pc != fb.pc) {
        fb.pc = (uint8_t)pc;
        ui_say("BLE: ", pc ? "COMPUTER CONNECTED" : "COMPUTER GONE");
        if (!pc)
            midi_usb_lost = 1;                    /* (its notes end, as a USB device's: seq.c) */
    }
    if (dev != fb.dev) {
        if (dev == FBLE_DEV_READY) {
            ui_say("BLE CONNECTED ", fb.cname);
            fb.try_ = 0;
        } else if (fb.dev == FBLE_DEV_READY) {    /* it went: FIRST looks again; CHOOSE does with RECONNECT on */
            ui_say("BLE: ", "LOST");
            midi_usb_lost = 1;
            fb.try_ = fb.try_ || (fb_set.central == FB_C_CHOOSE && fb_set.recon && fb_set.have);
            fb.wait = now + 1000u;
        } else if ((fb.dev == FBLE_DEV_CONNECTING || fb.dev == FBLE_DEV_SEARCHING) && dev == FBLE_DEV_OFF) {
            ui_say("BLE: ", "NO ANSWER");
            fb.wait = now + 3000u;
        }
        fb.dev = (uint8_t)dev;
    }
    if (!fb.on || fb_set.central == FB_C_OFF)
        return;
    if (dev == FBLE_DEV_CONNECTING || dev == FBLE_DEV_SEARCHING || dev == FBLE_DEV_READY)
        return;
    if (!(fble_page_shown() && fb_set.central == FB_C_CHOOSE) && fb_set.central != FB_C_FIRST && !fb.try_) {
        if (dev == FBLE_DEV_SCANNING && now - fb.scan_t > 300u) {   /* nothing to look for */
            fb.scan_t = now;
            fble_cmd(FBLE_CMD_SCAN, 0);
        }
        return;
    }
    if (dev == FBLE_DEV_OFF) {
        if (now - fb.scan_t > 300u) {
            fb.scan_t = now;
            fble_cmd(FBLE_CMD_SCAN, 1);
        }
        return;
    }
    if ((int32_t)(now - fb.wait) < 0)
        return;                                   /* (after a failure, a pause) */
    if (fb_set.central == FB_C_FIRST) {
        if (fble_found(0, 0, 0, 0))
            fble_connect_to(0);
        return;
    }
    for (i = 0; fb.try_ && i < 8; i++) {          /* CHOOSE: the chosen one, by its address */
        uint8_t a[6];
        if (!fble_found(i, 0, 0, a))
            break;
        if (!memcmp(a, fb_set.addr, 6)) {
            fble_connect_to(i);
            return;
        }
    }
}

/* ---- the main loop: every pass (and in its wait for the next frame) */
UI_SMALL static void fble_service(void)
{
    uint32_t e;
    if (fb_dirty && fm1_ms - fb_dirty_t > 1500u && !song.playing)
        fb_flush();                               /* (an erase silences the audio a moment: not while it plays) */
    if (fble_quiet && fh_ms() - fble_quiet_t > 3000u) {
        fble_quiet_set(0);                        /* (the module's quiet(0) never came: the render back anyway) */
        flog_line("fble: quiet too long: ended");
    }
    if (!fble) {
        fble_out_ring = 0;
        return;
    }
    if (fb_set.radio && !fble_started)
        fble_start_now();
    if (!fble_started)
        return;
    fble->poll();
    while (fble->evt->r != fble->evt->w) {        /* the events: into the log (the state is read where it is used) */
        char b[40];
        e = fble->evt->buf[fble->evt->r % fble->evt->n];
        RING_PUBLISH();
        fble->evt->r++;
        fhex(fcat(b, "fble evt "), e);
        flog_line(b);
    }
    if (fble->state() != FBLE_READY) {
        fble_out_ring = 0;
        return;
    }
    fble_midi_in();
    fble_task();
    fble_out_dev = (uint8_t)(fb_set.out && fb.on && fb.dev == FBLE_DEV_READY);
    fble_out_pc = (uint8_t)(fb_set.out && fb.on && fb.pc);
    fble_out_ring = fble->midi_out;
}

/* ---- USR4: the module's while its header is there (or a write of one is under way) */
UI_SMALL static int fble_usr4_taken(void)
{
    return fble_usr4_busy || (flash_ok && *(const uint32_t *)fm1_xip_ptr(FBLE_SEG0_OFF) == FBLE_MAGIC);
}

/* ---- the module over USB-MIDI: F0 7D 46 42 op ... F7 (see the top) */
UI_SMALL static void fble_answer(uint8_t op, const char *text, int err)
{
    uint8_t f[48];
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
UI_SMALL static void fble_log_send(const char *t)
{
    uint8_t f[48];
    uint32_t n = 0;
    f[n++] = 0xF0; f[n++] = 0x7D; f[n++] = 0x4C; f[n++] = 0x47;
    while (*t && n < sizeof f - 1u)
        f[n++] = (uint8_t)(*t++ & 0x7Fu);
    f[n++] = 0xF7;
    ota_wire_send(f, n);
}
/* a write frame's payload, decoded already (raw: st_buf): into USR4 */
UI_SMALL static int fble_write(uint32_t off, const uint8_t *raw, int got)
{
    uint32_t len, i, took;
    const uint8_t *chk;
    if (got < 0)
        return 5;
    len = (uint32_t)got;
    if (!len || !fble_in(off, len, FBLE_SEG0_OFF, FBLE_SEG0_OFF + FBLE_SEG0_MAX))
        return 1;                                 /* (segment 1's slot is SLOOP's USR3: never written) */
    if (fble && fble_started)
        return 6;                                 /* its code runs from here: restart first */
    if (fble) {
        fble = 0;                                 /* not started: its code is not called again; rewritten */
        fble_why = "rewritten";
        flog_line("fble: unloaded for a write");
    }
    if (!fble_usr4_busy) {                        /* USR4's sample out of use first (editor.c ed_smp_erase) */
        fble_usr4_busy = 1;
        usr_nz[3] = 0;
        for (i = 0; i < 16u; i++)
            usr_zone[3][i].n = 0;
        ed_smp_open[3] = 0;
        flog_line("fble: USR4 taken for the module");
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
UI_SMALL static void fble_log_frame(void)
{
    char b[48];
    uint32_t k, n;
    flog_fresh();
    if (fm1_crash.magic == FM1_CRASH_MAGIC) {
        fhex(fcat(fhex(fcat(fhex(fcat(fhex(fcat(b, "crash n "), fm1_crash.count), " vec "), fm1_crash.vec), " pc "),
                       fm1_crash.pc), " rets "), fm1_crash.rets);
        fble_log_send(b);
        fhex(fcat(fhex(fcat(fhex(fcat(b, "emu "), fm1_crash.emu), " dbg "), fm1_crash.dbg), " sp "), fm1_crash.sp);
        fble_log_send(b);
        fhex(fcat(fhex(fcat(fhex(fcat(fhex(fcat(b, "etm "), fm1_crash.etm[0]), " "), fm1_crash.etm[1]), " "),
                       fm1_crash.etm[2]), " "), fm1_crash.etm[3]);
        fble_log_send(b);
    } else {
        fble_log_send("no crash recorded");
    }
    fhex(fcat(b, "boot p3_rst "), fm1_boot.p3_rst);
    fble_log_send(b);
    n = flog.w < FLOG_N ? flog.w : FLOG_N;
    for (k = 0; k < n; k++)
        fble_log_send(fble_flog_l.line[(flog.w - n + k) % FLOG_N]);
    fble_log_send("end");
}
UI_SMALL static void fble_status_text(char *b);
/* the main loop, between the editor's frames (ed_service) and the update's (ota_service): "FB" and "LG" */
UI_SMALL static void fble_sysex(void)
{
    const uint8_t *p;
    uint32_t n, off = 0;
    uint8_t a[8] = {0};
    int got = -1;
    char b[48];
    if (!ota_frame_get(&p, &n))
        return;
    if (n >= 3u && p[0] == 0x7Du && p[1] == 0x4Cu && p[2] == 0x47u) {
        ota_frame_done();
        fble_log_frame();
        return;
    }
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x46u || p[2] != 0x42u)
        return;                                   /* (not ours: ota_service's) */
    for (off = 0; off < 8u && off + 3u < n; off++)
        a[off] = p[3u + off];                     /* op and its arguments */
    if (a[0] == 2u)
        got = fble_unpack(p + 4, n - 4u, &off, st_buf, 256u);
    ota_frame_done();                             /* (free before the answer: the next frame may come in) */
    switch (a[0]) {
    case 1:
        fble_status_text(b);
        fble_answer(1, b, 0);
        break;
    case 2:
        fble_answer(2, 0, fble_write(off, st_buf, got));
        break;
    case 3:
        fble_answer(3, fble_load() == 0 ? "loaded" : fble_why, 0);
        break;
    case 4:
        if (fble && !fb_set.radio) {
            fb_set.radio = 1;                     /* (a start by hand: GLO > BLE > RADIO shows ON, kept) */
            fb_changed();
        }
        fble_start_now();
        fble_answer(4, fble_started ? "started" : "no module", 0);
        break;
    case 5: {
        uint32_t took;
        if (fble && fble_started) {
            fble_answer(5, "running: restart first", 0);
            break;
        }
        fble = 0;
        fble_why = "removed";
        fble_answer(5, fl_erase4k_quiet(FBLE_SEG0_OFF, &took) ? "erase failed" : "removed", 0);
        fble_usr4_busy = 0;                       /* (USR4 takes a sample again: the editor's upload) */
        break;
    }
    case 6:
        flog.noload = a[1] == 1u;
        flog_line(flog.noload ? "fble: reboot (SysEx), no load" : "fble: reboot (SysEx)");
        fble_answer(6, "rebooting", 0);
        fm1_delay_ms(50);                         /* (the answer out first) */
        fble_reboot();
        break;
    case 7:
        if (n < 8u || !fble) {
            fble_answer(7, !fble ? "no module" : "?", 0);
            break;
        }
        fble_cmd(a[1], a[2] | (uint32_t)a[3] << 7 | (uint32_t)a[4] << 14);
        fhex(fcat(b, "fble: SysEx cmd "), FBLE_CMD(a[1], a[2] | (uint32_t)a[3] << 7 | (uint32_t)a[4] << 14));
        flog_line(b);
        fble_answer(7, "sent", 0);
        break;
    case 8: {
        char name[21];
        int k = fble_count();
        char *q = fcat(b, "st ");
        *q++ = (char)('0' + (!fble ? FBLE_FAILED : !fble_started ? FBLE_OFF : fble->state()));
        q = fcat(q, " dev ");
        *q++ = (char)('0' + ((fble_ready() ? fble->device() : 0) & 7));
        q = fcat(q, " pc ");
        *q++ = (char)('0' + (fble_ready() ? fble->peer() : 0));
        q = fcat(q, " found ");
        *q++ = (char)('0' + k);
        *q = 0;
        if (k && fble_found(0, name, 0, 0)) {
            name[12] = 0;
            fcat(fcat(q, " "), name);
        }
        fble_answer(8, b, 0);
        break;
    }
    default:
        fble_answer(a[0] & 0x7Fu, "?", 0);
        break;
    }
}

/* ---- GLO > BLE and BLE 2 (params.c PAGES, GR_BLE / GR_BLE2): the knobs, the columns, the middle (the state, the
 * module, the devices the scan found). In the style of GLO > USB > PORT: a switch that matters arms first */
static const char *const FB_ONOFF[2] = {"OFF", "ON"};
static const char *const FB_MODE[3] = {"OFF", "FIRST", "PICK"};   /* (PICK: CHOOSE, five letters at most) */
UI_SMALL static void fble_status_text(char *b)   /* one line: the status SysEx's (op 1) and the page's top */
{
    int st = fble && fble_started ? fble->state() : FBLE_OFF;
    if (!fble) {
        fcat(fcat(b, "NO MODULE: "), fble_why ? fble_why : "none");
        return;
    }
    if (st == FBLE_READY) {
        int d = fble->device();
        if (!fb_set.radio)
            fcat(b, "OFF");
        else if (d == FBLE_DEV_READY)
            fcat(fcat(b, "CONNECTED "), fb.cname);
        else if (d == FBLE_DEV_CONNECTING || d == FBLE_DEV_SEARCHING)
            fcat(b, "CONNECTING");
        else if (d == FBLE_DEV_SCANNING)
            fcat(b, "SCANNING");
        else
            fcat(b, fble->peer() == 2 ? "READY, COMPUTER CONNECTED" : "READY");
        return;
    }
    fcat(b, st == FBLE_STARTING || (fb_set.radio && st == FBLE_OFF) ? "STARTING" : st == FBLE_FAILED ? "FAILED" : "OFF");
}
UI_SMALL static void fble_knob(uint32_t page2, uint32_t slot, int32_t steps)
{
    int on = steps > 0;
    if (!page2 && slot == 0u) {                   /* RADIO: one detent arms, a second one within ~1.5 s switches */
        if (on == fb_set.radio)
            return;
        if (ui.arm != 0xD0u) {
            ui.arm = 0xD0u;
            ui.arm_t = 90;
            ui_say("AGAIN: ", on ? "BLE RADIO ON" : "BLE RADIO OFF");
            return;
        }
        ui.arm = 0;
        fb_set.radio = (uint8_t)on;
        fb_changed();
        ui_say("BLE RADIO ", !fble && on ? "ON: NO MODULE" : FB_ONOFF[on]);
        return;
    }
    ui.arm = 0;
    if (!page2 && slot == 1u) {                   /* MODE: OFF, FIRST, CHOOSE */
        int32_t v = clamp((int32_t)fb_set.central + (on ? 1 : -1), FB_C_OFF, FB_C_CHOOSE);
        if (v == fb_set.central)
            return;
        fb_set.central = (uint8_t)v;
        fb.try_ = fb_set.central == FB_C_CHOOSE && fb_set.recon && fb_set.have;
        fb_changed();
        if (fble_ready() && fb.on)
            fble_apply();
        ui_say("BLE MODE ", v == FB_C_CHOOSE ? "CHOOSE (PICK)" : FB_MODE[v]);
    } else if (!page2 && slot == 2u) {            /* the list's cursor */
        int n = fble_count();
        fb.cursor = (uint8_t)clamp((int32_t)fb.cursor + steps, 0, n ? n - 1 : 0);
    } else if (!page2 && slot == 3u) {            /* USE: the device under the cursor, kept, connected (CHOOSE) */
        uint8_t a[6];
        char nm[21];
        if (!on)
            return;
        if (!fble_found(fb.cursor, nm, 0, a)) {
            ui_message(fble_ready() ? "BLE: NO DEVICE THERE" : "BLE: RADIO NOT READY");
            return;
        }
        if (ui.arm != 0xD3u) {
            ui.arm = 0xD3u;
            ui.arm_t = 90;
            ui_say("AGAIN: USE ", nm);
            return;
        }
        ui.arm = 0;
        memcpy(fb_set.addr, a, 6);
        str_cpy(fb_set.name, nm, sizeof fb_set.name);
        fb_set.have = 1;
        fb_set.central = FB_C_CHOOSE;
        fb_changed();
        fb.try_ = 1;
        fb.wait = 0;
        if (fble->device() != FBLE_DEV_OFF && fble->device() != FBLE_DEV_SCANNING)
            fble_cmd(FBLE_CMD_DISCONNECT, 0);     /* (another one connected: let it go; the new one by its address) */
        ui_say("BLE CHOSEN ", nm);
    } else if (page2 && slot == 0u) {             /* PC: a computer may connect (the module advertises) */
        if (fble && !(fble_h.feat & FBLE_F_PERIPHERAL)) {
            ui_message("BLE: MODULE HAS NO PC LINK");
            return;
        }
        if (on == fb_set.pc)
            return;
        fb_set.pc = (uint8_t)on;
        fb_changed();
        if (fble_ready() && fb.on)
            fble_cmd(FBLE_CMD_ADVERTISE, fb_set.pc);
        ui_say("BLE PC ", FB_ONOFF[on]);
    } else if (page2 && slot == 1u) {             /* RECONNECT the chosen one */
        if (on == fb_set.recon)
            return;
        fb_set.recon = (uint8_t)on;
        fb_changed();
        ui_say("BLE RECONNECT ", FB_ONOFF[on]);
    } else if (page2 && slot == 2u) {             /* OUT: SLOOP's MIDI out to Bluetooth too */
        if (on == fb_set.out)
            return;
        fb_set.out = (uint8_t)on;
        fb_changed();
        ui_say("BLE MIDI OUT ", FB_ONOFF[on]);
    }
}
UI_SMALL static void fble_columns(uint32_t page2)
{
    char v[8];
    int n;
    if (page2) {
        int pc_ok = !fble || (fble_h.feat & FBLE_F_PERIPHERAL);
        draw_column(0, "PC", pc_ok ? FB_ONOFF[fb_set.pc] : "--", "", pc_ok ? VAL(0u) : C_DIM, -1, ICON_AUTO);
        draw_column(1, "RECON", FB_ONOFF[fb_set.recon], "", VAL(1u), -1, ICON_AUTO);
        draw_column(2, "OUT", FB_ONOFF[fb_set.out], "", VAL(2u), -1, ICON_AUTO);
        draw_column(3, "", "", "", C_HI, -1, ICON_AUTO);
        return;
    }
    n = fble_count();
    draw_column(0, "RADIO", FB_ONOFF[fb_set.radio], "", VAL(0u), -1, ICON_AUTO);
    draw_column(1, "MODE", FB_MODE[fb_set.central % 3u], "", VAL(1u), -1, ICON_AUTO);
    if (n) {
        char u[4] = {'/', (char)('0' + n), 0, 0};
        fmt_int(v, (int32_t)(fb.cursor < n ? fb.cursor : n - 1) + 1);
        draw_column(2, "LIST", v, u, VAL(2u), -1, ICON_AUTO);
    } else {
        draw_column(2, "LIST", "--", "", C_DIM, -1, ICON_AUTO);
    }
    draw_column(3, "USE", "--", "", n ? C_HI : C_DIM, -1, ICON_AUTO);
}
/* the middle: the state (white when connected), the module and USR4, then the scan's devices (the cursor's in white,
 * the chosen one marked) with their signal */
UI_SMALL static void fble_graph(void)
{
    char b[48], nm[21];
    int i, n = fble_count(), top, rssi;
    uint8_t a[6];
    fble_status_text(b);
    b[26] = 0;
    cv_text(4, 4, &FONT_S, b, fble_ready() && fble->device() == FBLE_DEV_READY ? C_WHITE : C_HI);
    if (fble)
        fcat(fcat(fcat(fcat(b, "MODULE "), fble_h.version), "  "), fble_started ? "ON" : "IDLE");
    else
        fcat(b, fble_usr4_taken() ? "USR4: MODULE NOT LOADED" : "PUT A MODULE IN USR4");
    b[26] = 0;
    cv_text(4, 21, &FONT_S, b, C_DIM);
    if (!n) {
        cv_text(14, 46, &FONT_S, !fb_set.radio ? "" : fb_set.central == FB_C_OFF ? "MODE OFF: NO DEVICE" :
                                 fble_ready() ? "NO DEVICE FOUND YET" : "", C_DIM);
        return;
    }
    if (fb.cursor >= n)
        fb.cursor = (uint8_t)(n - 1);
    top = fb.cursor > 4 ? fb.cursor - 4 : 0;
    for (i = top; i < n && i < top + 5; i++) {
        int32_t y = 42 + (i - top) * 17, sel = i == fb.cursor;
        if (!fble_found(i, nm, &rssi, a))
            break;
        nm[16] = 0;
        if (sel)
            cv_rect(4, y + 6, 3, 3, C_WHITE);
        if (fb_set.have && !memcmp(a, fb_set.addr, 6))
            cv_text(8, y, &FONT_S, "*", C_HI);
        cv_text(14, y, &FONT_S, nm, sel ? C_WHITE : C_GRAY);
        fmt_int(b, rssi);
        cv_text(236 - text_w(&FONT_S, b), y, &FONT_S, b, C_DIM);
    }
}
UI_SMALL static uint32_t fble_graph_sig(void)
{
    char b[48], nm[21];
    uint32_t h = 0x9E3779B9u;
    int i, rssi;
    b[0] = 0;
    fble_status_text(b);
    h = str_hash(h, b) + fb.cursor * 977u + fb_set.have * 31u + (fble != 0) * 7u + fble_started * 3u;
    for (i = 0; i < 8 && fble_found(i, nm, &rssi, 0); i++)
        h = str_hash(h, nm) * 31u + (uint32_t)(rssi / 4);
    return h;
}
