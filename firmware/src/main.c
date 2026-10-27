/* SPDX-License-Identifier: GPL-3.0-only
 * Reed FM-1 firmware — alpha bring-up.
 *
 * A minimal, standalone FM-1 application on felucca-bsp (MPL-2.0):
 *   - ALNK0 I2S audio, the fixed-point Reed engine in the DMA interrupt
 *   - 41-key scan (27 notes + 14 buttons), 7 encoders
 *   - ST7789 240x240 LCD, 5 parameters
 *   - USB-MIDI input (the BSP's class-compliant device)
 *
 * Not in this image yet: USB audio, OTA/update entry, flash settings, presets,
 * velocity. See docs/PLAN.md. Build with firmware/build.sh and the JieLi
 * toolchain; this alpha has not been validated on a physical FM-1.
 *
 * The startup, LCD and USB scaffolding are adapted from felucca-bsp's
 * examples/hwtest (MPL-2.0, Copyright (C) 2026 Leo Kuroshita / Hügelton
 * Instruments). See LICENSING.md and the BSP's NOTICE.
 */
#include <stdint.h>

#ifndef REED_LAB
#define REED_LAB 0
#endif

#if REED_LAB
/* Lab image: USB-CDC control/log and USB audio capture (UAC, 44.1 kHz only)
 * so the host script can record the same output the DAC gets. */
#define FELUCCA_CDC 1
#define FELUCCA_UAC 1
#define FELUCCA_UAC_48K 0
#define FELUCCA_UAC_DEEP 1
#define FELUCCA_UAC_AS_FIRST 1
#define FELUCCA_USB_PRODUCT "FM-1 Reed LAB"
#else
#define FELUCCA_CDC 0
#define FELUCCA_UAC 0
#define FELUCCA_USB_PRODUCT "FM-1 Reed"
#endif
#define FELUCCA_USB_MANUFACTURER "Reed"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define HALF_FRAMES 128
#define HALF_WORDS (HALF_FRAMES * 2u)

#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_timer.h"
#include "fm1_input.h"
#include "fm1_audio.h"
#include "fm1_lcd_hw.h"
#include "libc.c"
#include "usb.c"

#include "wurli.h"
#include "font5x7.h"

#define Q20_ONE 1048576

extern uint32_t _data_start[], _data_end[], _data_load[];
extern uint32_t _bss_start[], _bss_end[];
extern uint32_t _rt_start[], _rt_end[], _rt_load[];
extern void isr_timer5(void);
extern void isr_alnk0(void);

static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static wr_engine_t eng;
static volatile uint32_t ms;
static volatile uint32_t audio_max_us, audio_last_us, audio_late;
static struct { uint32_t magic, pending, failed; } guard __attribute__((section(".noinit")));

/* --------------------------------------------------------------- events --- */
/* Main loop -> audio ISR. Packed: type in 31..30, note 23..16, velocity 15..8,
 * parameter id 7..0 (for PARAM, value in 23..16). */
enum {
    EV_NOTE_ON = 1u << 29, EV_NOTE_OFF = 2u << 29, EV_PARAM = 3u << 29,
    EV_PANIC = 4u << 29, EV_SUSTAIN = 5u << 29, EV_ALL_OFF = 6u << 29
};
#define EVQ 64u
static volatile uint32_t evq[EVQ];
static volatile uint32_t ev_w, ev_r;

static int ev_put(uint32_t ev)
{
    uint32_t at = ev_w % EVQ;
    if (ev_w - ev_r >= EVQ)
        return 0;
    evq[at] = ev;
    RING_PUBLISH();
    ev_w++;
    return 1;
}

static void ev_note_on(uint32_t note, uint32_t vel)
{
    if (note < WR_MIDI_LO || note > WR_MIDI_HI)
        return;
    ev_put(EV_NOTE_ON | (note << 16) | (vel << 8));
}
static void ev_note_off(uint32_t note)
{
    ev_put(EV_NOTE_OFF | (note << 16));
}
static void ev_param(uint32_t id, uint32_t value)
{
    ev_put(EV_PARAM | (value << 16) | id);
}
static void ev_panic(void)
{
    ev_put(EV_PANIC);
}
static void ev_all_off(void)
{
    ev_put(EV_ALL_OFF);
}
static void ev_sustain(int on)
{
    ev_put(EV_SUSTAIN | (on ? 1u << 16 : 0u));
}

static void ev_drain(void)
{
    uint32_t w = ev_w;
    while (ev_r != w) {
        uint32_t ev = evq[ev_r % EVQ];
        ev_r++;
        switch (ev & (7u << 29)) {
        case EV_NOTE_ON:
            wr_note_on(&eng, (int)((ev >> 16) & 0xFFu), (int)((ev >> 8) & 0xFFu));
            break;
        case EV_NOTE_OFF:
            wr_note_off(&eng, (int)((ev >> 16) & 0xFFu));
            break;
        case EV_PARAM:
            wr_set_param(&eng, (int)(ev & 0xFFu), (int)((ev >> 16) & 0xFFu));
            break;
        case EV_PANIC:
            wr_panic(&eng);
            break;
        case EV_SUSTAIN:
            wr_set_sustain(&eng, (int)((ev >> 16) & 1u));
            break;
        case EV_ALL_OFF:
            wr_all_notes_off(&eng);
            break;
        }
    }
}

/* ----------------------------------------------------------- lab console --- */
#if REED_LAB
static char lab_line[64];
static uint32_t lab_len;
static wr_profile_t cc_profile;
static int32_t cc_master_db;

static void cc_sync_from_engine(int32_t master_db)
{
    cc_profile = wr_get_profile(&eng);
    cc_master_db = master_db;
}

static void lab_putc(char c)
{
    if ((uint32_t)(co_w - co_r) >= CO_N)
        return;
    cdc_out[co_w % CO_N] = (uint8_t)c;
    RING_PUBLISH();
    co_w++;
}

static void lab_puts(const char *s)
{
    while (*s)
        lab_putc(*s++);
}

static void lab_u32(uint32_t v)
{
    char b[10];
    int n = 0;
    if (!v) {
        lab_putc('0');
        return;
    }
    while (v && n < 10) {
        b[n++] = (char)('0' + v % 10u);
        v /= 10u;
    }
    while (n)
        lab_putc(b[--n]);
}

static int32_t lab_num(const char **pp)
{
    const char *p = *pp;
    int neg = 0;
    int32_t v = 0;
    while (*p == ' ')
        p++;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    *pp = p;
    return neg ? -v : v;
}

static void lab_stat(void)
{
    uint32_t cpu = audio_last_us * 100u / 2901u;
    lab_puts("STAT active=");
    lab_u32((uint32_t)wr_active_voices(&eng));
    lab_puts(" limit=");
    lab_u32((uint32_t)wr_get_voice_limit(&eng));
    lab_puts(" last_us=");
    lab_u32(audio_last_us);
    lab_puts(" max_us=");
    lab_u32(audio_max_us);
    lab_puts(" late=");
    lab_u32(audio_late);
    lab_puts(" cpu=");
    lab_u32(cpu);
    lab_puts("\n");
}

static void lab_set_default(void)
{
    wr_profile_t p;
    p.voices = WR_DEFAULT_VOICES;
    p.modes = WR_NMODES;
    p.pickup = WR_PICKUP_FULL;
    p.noise = 1;
    p.drag = 1;
    p.chain = WR_CHAIN_FULL;
    fm1_irq_off();
    wr_all_notes_off(&eng);
    wr_set_profile(&eng, &p);
    wr_set_master_q24(&eng, wr_gain_db_tab[24]); /* 0 dB */
    fm1_irq_on();
    cc_sync_from_engine(0);
}

static void lab_profile(const char *q)
{
    int32_t voices = lab_num(&q);
    int32_t modes = lab_num(&q);
    int32_t pickup = lab_num(&q);
    int32_t noise = lab_num(&q);
    int32_t drag = lab_num(&q);
    int32_t chain = lab_num(&q);
    int32_t db = lab_num(&q);
    if (voices < 1 || voices > WR_MAX_VOICES || modes < 1 || modes > WR_NMODES ||
        pickup < 0 || pickup > 1 || noise < 0 || noise > 1 || drag < 0 || drag > 1 ||
        chain < 0 || chain > 1) {
        lab_puts("ERR profile\n");
        return;
    }
    if (db < -24)
        db = -24;
    if (db > 6)
        db = 6;
    wr_profile_t p;
    p.voices = (uint8_t)voices;
    p.modes = (uint8_t)modes;
    p.pickup = (uint8_t)pickup;
    p.noise = (uint8_t)noise;
    p.drag = (uint8_t)drag;
    p.chain = (uint8_t)chain;
    fm1_irq_off();
    wr_all_notes_off(&eng);
    int ok = wr_set_profile(&eng, &p);
    if (ok)
        wr_set_master_q24(&eng, wr_gain_db_tab[db + 24]);
    fm1_irq_on();
    if (!ok) {
        lab_puts("ERR profile\n");
        return;
    }
    cc_sync_from_engine(db);
    lab_puts("OK ");
    lab_u32((uint32_t)wr_get_voice_limit(&eng));
    lab_putc(' ');
    lab_u32((uint32_t)wr_get_profile(&eng).modes);
    lab_putc(' ');
    lab_u32((uint32_t)wr_get_master_q24(&eng));
    lab_putc('\n');
}

static void lab_command(void)
{
    lab_line[lab_len] = 0;
    if (lab_len && lab_line[lab_len - 1] == '\r')
        lab_line[--lab_len] = 0;
    if (!lab_len)
        return;
    switch (lab_line[0]) {
    case 'P':
    case 'p':
        lab_profile(lab_line + 1);
        break;
    case 'S':
    case 's':
        lab_stat();
        break;
    case 'C':
    case 'c':
        audio_max_us = 0;
        audio_last_us = 0;
        audio_late = 0;
        lab_puts("OK stats\n");
        break;
    case 'X':
    case 'x':
        fm1_irq_off();
        wr_panic(&eng);
        fm1_irq_on();
        lab_puts("OK panic\n");
        break;
    case 'R':
    case 'r':
        lab_set_default();
        lab_puts("OK default\n");
        break;
    case 'V':
    case 'v':
        lab_puts("REED-LAB 0.2\n");
        break;
    default:
        lab_puts("ERR command\n");
        break;
    }
    lab_len = 0;
}

static void lab_poll(void)
{
    while (ci_r != ci_w) {
        char c = (char)cdc_in[ci_r % CI_N];
        ci_r++;
        if (c == '\n') {
            lab_command();
        } else if (lab_len < sizeof(lab_line) - 1) {
            lab_line[lab_len++] = c;
        }
    }
}
#endif

/* ---------------------------------------------------------------- audio --- */
void fm1_alnk0_irq(void)
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t t0 = fm1_ticks();
        uint32_t half = fm1_audio_free_half();
        int32_t *o = &abuf[half * HALF_WORDS];
        int32_t mono[HALF_FRAMES];
#if REED_LAB
        uac_render_start();
#endif
        ev_drain();
        wr_render(&eng, mono, HALF_FRAMES);
        for (uint32_t i = 0; i < HALF_FRAMES; i++) {
            int32_t s = mono[i];
            if (s > Q20_ONE)
                s = Q20_ONE;
            else if (s < -Q20_ONE)
                s = -Q20_ONE;
            s <<= 3; /* Q20 -> 24-bit, no DAC headroom trim */
            o[2u * i] = s;
            o[2u * i + 1u] = s;
        }
#if REED_LAB
        uac_tap(o, HALF_FRAMES);
#endif
        fm1_audio_ack_half();
        if (REED_LAB && fm1_audio_free_half() != half)
            audio_late++;
        uint32_t us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
        audio_last_us = us;
        if (us > audio_max_us)
            audio_max_us = us;
    }
}

/* ---------------------------------------------------------------- timer --- */
void fm1_timer5_irq(void) /* 10 kHz: key scan, ms, USB at 2 kHz */
{
    static uint32_t sub, last, acc;
    uint32_t t = fm1_ticks();
    fm1_timer5_ack();
    fm1_input_tick();
    acc += t - last;
    last = t;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        ms++;
    }
    fm1_ms = ms;
    if (++sub == 5u) {
        sub = 0;
        usb_poll();
    }
}

/* ------------------------------------------------------------------ LCD --- */
#define C(x) ((uint16_t)((((x) >> 8) | ((x) << 8)) & 0xFFFFu))
#define COL_BG C(0x1082)
#define COL_AMBER C(0xE549)
#define COL_CREAM C(0xF73A)
#define COL_DIM C(0x6B4D)
#define COL_LINE C(0x39E7)
#define COL_GREEN C(0x4E68)
#define COL_RED C(0xC986)

static uint16_t row_px[240];

static void lcd_cmd(uint8_t c, const uint8_t *d, uint32_t n)
{
    static uint8_t buf[4];
    uint32_t i;
    fm1_lcd_send_cmd(c);
    fm1_lcd_wait();
    fm1_lcd_deselect();
    if (!n)
        return;
    for (i = 0; i < n; i++)
        buf[i] = d[i];
    fm1_lcd_send_data(buf, n);
    fm1_lcd_wait();
    fm1_lcd_deselect();
}

static void lcd_init(void)
{
    static const uint8_t colmod = 0x55, madctl = 0x00;
    fm1_lcd_hw_init();
    fm1_delay_ms(50);
    lcd_cmd(0x01, 0, 0);
    fm1_delay_ms(150);
    lcd_cmd(0x11, 0, 0);
    fm1_delay_ms(120);
    lcd_cmd(0x3A, &colmod, 1);
    lcd_cmd(0x36, &madctl, 1);
    lcd_cmd(0x21, 0, 0);
    lcd_cmd(0x13, 0, 0);
    lcd_cmd(0x29, 0, 0);
    fm1_lcd_baud(4);
}

static void lcd_window(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint8_t a[4];
    a[0] = 0;
    a[1] = (uint8_t)x;
    a[2] = 0;
    a[3] = (uint8_t)(x + w - 1u);
    lcd_cmd(0x2A, a, 4);
    a[1] = (uint8_t)y;
    a[3] = (uint8_t)(y + h - 1u);
    lcd_cmd(0x2B, a, 4);
    lcd_cmd(0x2C, 0, 0);
}

static void lcd_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t rgb)
{
    if (!w || !h || x + w > 240u || y + h > 240u)
        return;
    for (uint32_t i = 0; i < w; i++)
        row_px[i] = rgb;
    lcd_window(x, y, w, h);
    for (uint32_t i = 0; i < h; i++) {
        fm1_lcd_send_data(row_px, w * 2u);
        fm1_lcd_wait();
        fm1_lcd_deselect();
    }
}

/* 5x7 font, 1-pixel gap, optional integer scale. */
static void lcd_text(uint32_t x, uint32_t y, const char *s, uint32_t scale, uint16_t fg, uint16_t bg)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    uint32_t w = n * 6u * scale;
    if (!n || x + w > 240u || y + 8u * scale > 240u)
        return;
    for (uint32_t r = 0; r < 8u * scale; r++) {
        for (uint32_t i = 0; i < w; i++)
            row_px[i] = bg;
        uint32_t ry = r / scale;
        for (uint32_t k = 0; k < n; k++) {
            uint8_t ch = (uint8_t)s[k];
            if (ch < 32 || ch > 126)
                ch = '?';
            const unsigned char *g = &font5x7[(ch - 32) * 5];
            for (uint32_t c = 0; c < 5; c++) {
                if (ry < 7 && (g[c] & (1u << ry))) {
                    for (uint32_t sx = 0; sx < scale; sx++)
                        row_px[(k * 6u + c) * scale + sx] = fg;
                }
            }
        }
        lcd_window(x, y + r, w, 1);
        fm1_lcd_send_data(row_px, w * 2u);
        fm1_lcd_wait();
        fm1_lcd_deselect();
    }
}

static const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static void note_str(int note, char *out)
{
    int n = note % 12;
    int o = note / 12 - 1;
    const char *s = NOTE_NAMES[n];
    out[0] = s[0];
    if (s[1]) {
        out[1] = s[1];
        out[2] = (char)('0' + (o < 0 ? 0 : o > 9 ? 9 : o));
        out[3] = 0;
    } else {
        out[1] = (char)('0' + (o < 0 ? 0 : o > 9 ? 9 : o));
        out[2] = 0;
    }
}

/* ---------------------------------------------------------------- screen --- */
static const char *PARAM_NAMES[5] = {"VOLUME", "TREMOLO", "DRIVE", "SPEAKER", "TUNE"};
static const int PARAM_DEF[5] = {102, 64, 64, 90, 0};
static int shown_note = -1, shown_voices = -1, shown_oct = 99;

static void draw_static(void)
{
    lcd_rect(0, 0, 240, 240, COL_BG);
    lcd_rect(0, 0, 240, 3, COL_AMBER);
    lcd_text(12, 14, "REED", 3, COL_AMBER, COL_BG);
    lcd_text(12, 42, "WURLITZER 200A", 1, COL_CREAM, COL_BG);
    lcd_text(12, 54, "FM-1 / FIXED-POINT MODEL", 1, COL_DIM, COL_BG);
    lcd_rect(12, 66, 216, 1, COL_LINE);
    for (int i = 0; i < 5; i++)
        lcd_text(12, 78 + i * 24, PARAM_NAMES[i], 1, COL_DIM, COL_BG);
    lcd_rect(12, 198, 216, 1, COL_LINE);
    lcd_text(12, 206, "VOICES", 1, COL_DIM, COL_BG);
    lcd_text(90, 206, "NOTE", 1, COL_DIM, COL_BG);
    lcd_text(170, 206, "OCT", 1, COL_DIM, COL_BG);
    lcd_text(12, 222, "ALPHA / FELUCCA-BSP / REED 0.1", 1, COL_DIM, COL_BG);
}

static void draw_param(int i, int value)
{
    char buf[8];
    int v = value;
    if (i == 4) {
        v = value;
        buf[0] = (char)('0' + (v < 0 ? -v : v) / 100);
        if (buf[0] == '0')
            buf[0] = ' ';
        buf[1] = (char)('0' + ((v < 0 ? -v : v) % 100) / 10);
        buf[2] = (char)('0' + ((v < 0 ? -v : v) % 10));
        buf[3] = 0;
        lcd_text(150, 78 + i * 24, v < 0 ? "-" : "+", 1, COL_AMBER, COL_BG);
        lcd_text(158, 78 + i * 24, buf, 1, COL_AMBER, COL_BG);
        lcd_text(186, 78 + i * 24, "CT", 1, COL_DIM, COL_BG);
        lcd_rect(12, 86 + i * 24, 216, 3, COL_LINE);
        int w = (int)((v + 50) * 216 / 100);
        lcd_rect(12, 86 + i * 24, (uint32_t)(w < 0 ? 0 : w > 216 ? 216 : w), 3, COL_AMBER);
        return;
    }
    buf[0] = (char)('0' + (v / 100) % 10);
    buf[1] = (char)('0' + (v / 10) % 10);
    buf[2] = (char)('0' + v % 10);
    buf[3] = 0;
    lcd_text(150, 78 + i * 24, buf, 1, COL_CREAM, COL_BG);
    lcd_rect(12, 86 + i * 24, 216, 3, COL_LINE);
    lcd_rect(12, 86 + i * 24, (uint32_t)(v * 216 / 127), 3, COL_AMBER);
}

static void draw_status(int note, int voices, int oct)
{
    char b[8];
    if (note != shown_note) {
        if (note < 0)
            lcd_text(126, 206, "---", 1, COL_CREAM, COL_BG);
        else {
            note_str(note, b);
            lcd_text(126, 206, b, 1, COL_CREAM, COL_BG);
        }
        shown_note = note;
    }
    if (voices != shown_voices) {
        b[0] = (char)('0' + (voices / 10) % 10);
        b[1] = (char)('0' + voices % 10);
        b[2] = 0;
        lcd_text(60, 206, b, 1, COL_CREAM, COL_BG);
        shown_voices = voices;
    }
    if (oct != shown_oct) {
        b[0] = (oct < 0 ? '-' : '+');
        b[1] = (char)('0' + (oct < 0 ? -oct : oct));
        b[2] = 0;
        lcd_text(198, 206, b, 1, COL_CREAM, COL_BG);
        shown_oct = oct;
    }
}

/* ------------------------------------------------------------------ MIDI --- */
static void midi_usb(void)
{
    uint32_t w = mi_w;
    while (mi_r != w) {
        uint32_t pkt = midi_in_q[mi_r % MQ];
        mi_r++;
        uint8_t cin = (uint8_t)(pkt & 0xFu);
        uint8_t st = (uint8_t)(pkt >> 8);
        uint8_t d1 = (uint8_t)(pkt >> 16);
        uint8_t d2 = (uint8_t)(pkt >> 24);
        (void)cin;
        if ((st & 0xF0) == 0x90 && d2)
            ev_note_on(d1, d2);
        else if ((st & 0xF0) == 0x80 || ((st & 0xF0) == 0x90 && !d2))
            ev_note_off(d1);
        else if ((st & 0xF0) == 0xB0) {
            if (d1 == 64)
                ev_sustain(d2 >= 64);
            else if (d1 == 7)
                ev_param(WR_P_VOLUME, d2);
            else if (d1 == 1)
                ev_param(WR_P_TREMOLO, d2);
            else if (d1 == 11)
                ev_param(WR_P_DRIVE, d2);
#if REED_LAB
            else if (d1 == 20)
                cc_profile.voices = (uint8_t)(d2 < 1 ? 1 : d2 > WR_MAX_VOICES ? WR_MAX_VOICES : d2);
            else if (d1 == 21)
                cc_profile.modes = (uint8_t)(d2 < 1 ? 1 : d2 > WR_NMODES ? WR_NMODES : d2);
            else if (d1 == 22)
                cc_profile.pickup = d2 >= 64 ? WR_PICKUP_LITE : WR_PICKUP_FULL;
            else if (d1 == 23)
                cc_profile.noise = d2 >= 64 ? 1 : 0;
            else if (d1 == 24)
                cc_profile.drag = d2 >= 64 ? 1 : 0;
            else if (d1 == 25)
                cc_profile.chain = d2 >= 64 ? WR_CHAIN_LITE : WR_CHAIN_FULL;
            else if (d1 == 26)
                cc_master_db = (int32_t)(d2 > 30 ? 30 : d2) - 24;
            else if (d1 == 27 && d2 >= 64) {
                fm1_irq_off();
                wr_all_notes_off(&eng);
                int ok = wr_set_profile(&eng, &cc_profile);
                if (ok)
                    wr_set_master_q24(&eng, wr_gain_db_tab[cc_master_db + 24]);
                fm1_irq_on();
            } else if (d1 == 120)
                ev_panic();
            else if (d1 == 123)
                ev_all_off();
#endif
        }
    }
    if (midi_in_overflow) {
        midi_in_overflow = 0;
        ev_panic();
    }
}

/* ----------------------------------------------------------------- fault --- */
static void fm1_fault(const fm1_crash_t *c)
{
    (void)c;
    lcd_rect(0, 0, 240, 240, COL_RED);
    lcd_text(12, 20, "FAULT", 3, COL_CREAM, COL_RED);
    lcd_text(12, 60, "REBOOTING", 1, COL_CREAM, COL_RED);
    fm1_delay_ms(800);
    fm1_reboot();
}

/* ------------------------------------------------------------------ main --- */
static int octave;

static void set_octave(int v)
{
    if (v < -2)
        v = -2;
    if (v > 2)
        v = 2;
    if (v == octave)
        return;
    octave = v;
    ev_panic();
}

static void app_main(void)
{
    uint32_t last_notes = 0, last_ui = 0;
    int boots_ok = 0;
    int params[5];
    for (int i = 0; i < 5; i++)
        params[i] = PARAM_DEF[i];

    fm1_input_init();
    wr_init(&eng, 44100);
    for (int i = 0; i < 5; i++)
        wr_set_param(&eng, i, params[i]);
#if REED_LAB
    cc_sync_from_engine(0);
#endif
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    lcd_init();
    draw_static();
    for (int i = 0; i < 5; i++)
        draw_param(i, params[i]);
    draw_status(-1, 0, 0);

    usb_start();
#if REED_LAB
    lab_puts("REED-LAB 0.2 ready\n");
    lab_puts("commands: P voices modes pickup noise drag chain db | S | R | X | V\n");
#endif
    fm1_timer5_start(isr_timer5, 4); /* above ALNK0, as Felucca */
    fm1_irq_enable_all();

    for (;;) {
#if REED_LAB
        lab_poll();
#endif
        fm1_input_scan();
        uint32_t now = ms;

        /* Keys. */
        uint32_t pressed = fm1_input_note_edges();
        uint32_t notes = fm1_in.notes;
        for (int n = 0; n < 27; n++) {
            uint32_t bit = 1u << n;
            if (pressed & bit)
                ev_note_on((uint32_t)(53 + n + octave * 12), 100);
            if ((last_notes & bit) && !(notes & bit))
                ev_note_off((uint32_t)(53 + n + octave * 12));
        }
        last_notes = notes;

        /* Buttons: 0 octave down, 1 octave up, 2 sustain hold, 3 panic. */
        uint32_t released;
        uint32_t btn = fm1_input_edges(&released);
        (void)released;
        if (btn & 1u)
            set_octave(octave - 1);
        if (btn & 2u)
            set_octave(octave + 1);
        if (btn & 4u) {
            static int sus;
            sus = !sus;
            ev_sustain(sus);
        }
        if (btn & 8u) {
            ev_panic();
            last_notes = 0;
        }

        /* Encoders 1..5: volume, tremolo, drive, speaker, tune. */
        for (int i = 0; i < 5; i++) {
            int32_t d = fm1_enc_take((uint32_t)i);
            if (!d)
                continue;
            params[i] += (int)d;
            if (i == 4) {
                if (params[i] < -50)
                    params[i] = -50;
                if (params[i] > 50)
                    params[i] = 50;
            } else {
                if (params[i] < 0)
                    params[i] = 0;
                if (params[i] > 127)
                    params[i] = 127;
            }
            ev_param((uint32_t)i, (uint32_t)params[i]);
            draw_param(i, params[i]);
        }

        midi_usb();
        usb_retry(now);

        if (now - last_ui >= 100u) {
            last_ui = now;
            int active = wr_active_voices(&eng);
            int note = -1;
            for (int n = 26; n >= 0; n--)
                if (notes & (1u << n)) {
                    note = 53 + n + octave * 12;
                    break;
                }
            draw_status(note, active, octave);
            (void)audio_last_us;
            (void)audio_max_us;
        }
        if (!boots_ok && now > 2000u) { /* reached the loop for 2 s: the next boot is not a loop */
            boots_ok = 1;
            guard.pending = 0;
            guard.failed = 0;
        }
        fm1_wdt_feed();
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d;
    fm1_time_init();
    fm1_wdt_arm(0x0D);
    if (guard.magic != 0x52454544u) {
        guard.magic = 0x52454544u;
        guard.failed = 0;
        guard.pending = 0;
    }
    if (guard.pending)
        guard.failed++;
    guard.pending = 1;
    if (guard.failed >= 2u) { /* two boots that never reached the main loop: stock UBOOT */
        guard.failed = 0;
        guard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    app_main();
    for (;;)
        ;
}
