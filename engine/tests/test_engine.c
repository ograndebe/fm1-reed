/* SPDX-License-Identifier: GPL-3.0-only
 * Host tests for the Reed engine. No test framework: assert() and a main.
 *   cc -O2 -Iengine/include engine/src/wurli_engine.c engine/src/wurli_tables.c engine/tests/test_engine.c -lm
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wurli.h"

#define SR 44100

static int32_t buf[SR * 4];

static double peak_of(const int32_t *b, int n)
{
    int32_t p = 0;
    for (int i = 0; i < n; i++) {
        int32_t a = b[i] < 0 ? -b[i] : b[i];
        if (a > p)
            p = a;
    }
    return p / 1048576.0;
}

static double rms_of(const int32_t *b, int from, int to)
{
    double s = 0;
    for (int i = from; i < to; i++) {
        double v = b[i] / 1048576.0;
        s += v * v;
    }
    return sqrt(s / (to - from));
}

static void render(wr_engine_t *e, int n)
{
    wr_render(e, buf, (uint32_t)n);
}

static void clean_defaults(wr_engine_t *e)
{
    wr_set_param(e, WR_P_VOLUME, 110);
    wr_set_param(e, WR_P_TREMOLO, 0);
    wr_set_param(e, WR_P_SPEAKER, 100);
    wr_set_param(e, WR_P_DRIVE, 64);
}

static double autocorr_f0(const int32_t *b, int n)
{
    int start = n / 4, len = 8192;
    if (start + len + 3000 > n)
        len = n - start - 3000;
    double best = -1e30;
    int best_lag = 0;
    for (int lag = 22; lag < 1200; lag++) { /* 36 Hz .. 2 kHz */
        double c = 0;
        for (int i = 0; i < len; i += 2)
            c += (double)b[start + i] * b[start + i + lag];
        if (c > best) {
            best = c;
            best_lag = lag;
        }
    }
    return (double)SR / best_lag;
}

int main(void)
{
    wr_engine_t e;

    /* 1. Silence. */
    wr_init(&e, SR);
    render(&e, 2048);
    for (int i = 0; i < 2048; i++)
        assert(buf[i] == 0);

    /* 2. A note sounds, in a sane range. */
    wr_init(&e, SR);
    clean_defaults(&e);
    assert(wr_note_on(&e, 60, 100) == 1);
    render(&e, SR);
    double pk = peak_of(buf, SR);
    printf("C4 vel100 peak: %.4f (%.1f dBFS)\n", pk, 20 * log10(pk + 1e-12));
    assert(pk > 0.02 && pk < 0.30);

    /* 3. Velocity changes level. */
    wr_engine_t soft, loud;
    wr_init(&soft, SR);
    clean_defaults(&soft);
    wr_note_on(&soft, 60, 40);
    render(&soft, SR / 2);
    double ps = peak_of(buf, SR / 2);
    wr_init(&loud, SR);
    clean_defaults(&loud);
    wr_note_on(&loud, 60, 120);
    render(&loud, SR / 2);
    double pl = peak_of(buf, SR / 2);
    printf("velocity 40: %.4f  120: %.4f\n", ps, pl);
    assert(pl > ps * 1.25);

    /* 4. Deterministic. */
    wr_engine_t a, b;
    wr_init(&a, SR);
    wr_init(&b, SR);
    wr_note_on(&a, 64, 90);
    wr_note_on(&b, 64, 90);
    int32_t ba[SR], bb[SR];
    wr_render(&a, ba, SR);
    wr_render(&b, bb, SR);
    assert(memcmp(ba, bb, sizeof(ba)) == 0);

    /* 5. Pitch accuracy (A3, C4, C5, C6). */
    const int notes[] = {57, 60, 72, 84};
    for (int k = 0; k < 4; k++) {
        wr_init(&e, SR);
        clean_defaults(&e);
        wr_note_on(&e, notes[k], 100);
        render(&e, SR);
        double f0 = autocorr_f0(buf, SR);
        double expect = 440.0 * pow(2.0, (notes[k] - 69) / 12.0);
        double cents = 1200.0 * log2(f0 / expect);
        printf("MIDI %d: %.2f Hz (expect %.2f, %+.1f cents)\n", notes[k], f0, expect, cents);
        assert(cents > -25 && cents < 25);
    }

    /* 6. Damper: note off silences a mid note. */
    wr_init(&e, SR);
    clean_defaults(&e);
    wr_note_on(&e, 60, 100);
    render(&e, SR / 2);
    double before = rms_of(buf, SR / 2 - 4410, SR / 2);
    wr_note_off(&e, 60);
    render(&e, SR);
    double after = rms_of(buf, SR - 4410, SR);
    printf("damper: before %.4f after %.4f\n", before, after);
    assert(after < before * 0.2);

    /* 7. Sustain pedal. */
    wr_init(&e, SR);
    clean_defaults(&e);
    wr_set_sustain(&e, 1);
    wr_note_on(&e, 60, 100);
    render(&e, SR / 2);
    wr_note_off(&e, 60);
    render(&e, SR / 2);
    double held = rms_of(buf, SR / 2 - 4410, SR / 2);
    wr_set_sustain(&e, 0);
    render(&e, SR);
    double released = rms_of(buf, SR - 4410, SR);
    printf("sustain: held %.4f released %.4f\n", held, released);
    assert(held > 0.001 && released < held * 0.25);

    /* 8. Polyphony and voice stealing. */
    wr_init(&e, SR);
    clean_defaults(&e);
    for (int k = 0; k < 12; k++)
        wr_note_on(&e, 48 + k, 100);
    assert(wr_active_voices(&e) <= WR_MAX_VOICES);
    render(&e, SR);
    assert(peak_of(buf, SR) < 1.0);
    assert(wr_active_voices(&e) <= WR_MAX_VOICES);

    /* 9. All notes off -> quiet. */
    wr_all_notes_off(&e);
    render(&e, SR * 3 / 2);
    double tail = rms_of(buf, SR * 3 / 2 - 4410, SR * 3 / 2);
    printf("all off tail rms: %.5f\n", tail);
    assert(tail < 0.001);

    /* 10. Long render stability, low + high notes. */
    wr_init(&e, SR);
    wr_note_on(&e, 33, 127);
    wr_note_on(&e, 96, 127);
    for (int k = 0; k < 30; k++) {
        render(&e, SR);
        assert(peak_of(buf, SR) < 1.0);
    }

    /* 11. Fixed-point golden hash (C4 ff, 1 s, clean chain). */
    wr_init(&e, SR);
    clean_defaults(&e);
    wr_note_on(&e, 60, 127);
    render(&e, SR);
    uint32_t h = 2166136261u;
    for (int i = 0; i < SR; i++) {
        uint32_t v = (uint32_t)buf[i];
        for (int k = 0; k < 4; k++) {
            h ^= (v >> (k * 8)) & 0xFF;
            h *= 16777619u;
        }
    }
    printf("golden C4 ff: %08x\n", h);
    assert(h == 0x0bc2f64fu); /* update only for an intended sound change */

    /* 12. Stereo helper. */
    wr_init(&e, SR);
    wr_note_on(&e, 60, 100);
    int16_t st[256 * 2];
    int16_t st2[256 * 2];
    wr_render_stereo(&e, st, 256);
    for (int i = 0; i < 256; i++)
        assert(st[2 * i] == st[2 * i + 1]);
    wr_init(&e, SR);
    wr_note_on(&e, 60, 100);
    wr_render_stereo(&e, st2, 256);
    assert(memcmp(st, st2, sizeof(st)) == 0);

    puts("all engine tests passed");
    return 0;
}
