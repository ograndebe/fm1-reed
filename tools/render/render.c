/* SPDX-License-Identifier: GPL-3.0-only
 * reed — offline renderer and benchmark for the Reed FM-1 engine.
 *
 *   reed note  --note 60 --vel 100 --seconds 3 --out note.wav
 *   reed midi  --in song.mid --out song.wav [--volume N] [--tremolo N]
 *              [--speaker N] [--drive N] [--tune CENTS] [--gain DB]
 *   reed demo  --out demo.wav
 *   reed bench
 *
 * Build:
 *   cc -O2 -Iengine/include engine/src/wurli_engine.c engine/src/wurli_tables.c \
 *      tools/render/render.c -lm -o build/reed
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wurli.h"

#define SR 44100

/* ------------------------------------------------------------------ WAV --- */
typedef struct {
    FILE *f;
    uint32_t frames;
} wav_t;

static double master_gain = 1.0;

static void wav_open(wav_t *w, const char *path)
{
    w->f = fopen(path, "wb");
    if (!w->f) {
        perror(path);
        exit(1);
    }
    uint32_t v = 0;
    fwrite("RIFF", 1, 4, w->f);
    fwrite(&v, 4, 1, w->f);
    fwrite("WAVEfmt ", 1, 8, w->f);
    v = 16;
    fwrite(&v, 4, 1, w->f);
    uint16_t a = 1, ch = 2, ba = 4, bits = 16;
    uint32_t br = SR * 4, sr = SR;
    fwrite(&a, 2, 1, w->f);
    fwrite(&ch, 2, 1, w->f);
    fwrite(&sr, 4, 1, w->f);
    fwrite(&br, 4, 1, w->f);
    fwrite(&ba, 2, 1, w->f);
    fwrite(&bits, 2, 1, w->f);
    fwrite("data", 1, 4, w->f);
    fwrite(&v, 4, 1, w->f);
    w->frames = 0;
}

static void wav_write(wav_t *w, const int32_t *mono, uint32_t n)
{
    int16_t block[128 * 2];
    uint32_t done = 0;
    while (done < n) {
        uint32_t k = n - done > 128 ? 128 : n - done;
        for (uint32_t i = 0; i < k; i++) {
            double g = (double)mono[done + i] * master_gain;
            if (g > 1048576)
                g = 1048576;
            else if (g < -1048576)
                g = -1048576;
            int16_t v = (int16_t)((int32_t)g >> 5);
            block[2 * i] = v;
            block[2 * i + 1] = v;
        }
        fwrite(block, 2, k * 2, w->f);
        done += k;
    }
    w->frames += n;
}

static void wav_close(wav_t *w)
{
    uint32_t v = 36 + w->frames * 4;
    fseek(w->f, 4, SEEK_SET);
    fwrite(&v, 4, 1, w->f);
    fseek(w->f, 40, SEEK_SET);
    v = w->frames * 4;
    fwrite(&v, 4, 1, w->f);
    fclose(w->f);
}

/* ------------------------------------------------------------------ MIDI --- */
enum { EV_NOTE_ON, EV_NOTE_OFF, EV_TEMPO, EV_SUSTAIN };

typedef struct {
    uint32_t tick;
    uint32_t order;
    uint8_t type;
    uint8_t a, b;
} mev_t;

typedef struct {
    mev_t *ev;
    uint32_t n, cap;
    uint32_t tempo_us;
    uint32_t division;
} midi_t;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void midi_add(midi_t *m, uint32_t tick, int type, int a, int b)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 256;
        m->ev = realloc(m->ev, m->cap * sizeof(*m->ev));
        if (!m->ev) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
    }
    m->ev[m->n] = (mev_t){tick, m->n, (uint8_t)type, (uint8_t)a, (uint8_t)b};
    m->n++;
}

static int cmp_ev(const void *a, const void *b)
{
    const mev_t *x = a, *y = b;
    if (x->tick != y->tick)
        return x->tick < y->tick ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static uint32_t vlq(const uint8_t *d, size_t len, size_t *p)
{
    uint32_t v = 0;
    while (*p < len) {
        uint8_t b = d[(*p)++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            break;
    }
    return v;
}

static void midi_load(midi_t *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    fclose(f);

    memset(m, 0, sizeof(*m));
    m->tempo_us = 500000;
    if (sz < 14 || memcmp(d, "MThd", 4)) {
        fprintf(stderr, "%s: not a Standard MIDI File\n", path);
        exit(1);
    }
    m->division = rd16(d + 12);
    if (m->division & 0x8000) {
        fprintf(stderr, "%s: SMPTE timing is not supported\n", path);
        exit(1);
    }

    size_t p = 8 + rd32(d + 4);
    while (p + 8 <= (size_t)sz) {
        if (memcmp(d + p, "MTrk", 4)) {
            p += 8 + rd32(d + p + 4);
            continue;
        }
        size_t len = rd32(d + p + 4);
        size_t end = p + 8 + len;
        size_t q = p + 8;
        uint32_t tick = 0;
        uint8_t status = 0, running = 0;
        while (q < end) {
            tick += vlq(d, end, &q);
            if (q >= end)
                break;
            uint8_t b = d[q];
            if (b & 0x80) {
                status = b;
                q++;
            } else {
                b = running;
            }
            if (!status)
                break;
            if (status == 0xFF) {
                uint8_t meta = d[q++];
                uint32_t ml = vlq(d, end, &q);
                if (meta == 0x51 && ml == 3) {
                    uint32_t us = (uint32_t)d[q] << 16 | (uint32_t)d[q + 1] << 8 | d[q + 2];
                    midi_add(m, tick, EV_TEMPO, us >> 16, 0);
                    /* Store the full tempo in a side list: the event carries a
                     * 24-bit value as two bytes is not enough. Tempo is kept
                     * out of band below. */
                }
                if (meta == 0x2F)
                    break;
                q += ml;
                running = 0;
                continue;
            }
            running = status;
            uint8_t hi = status & 0xF0;
            if (hi == 0xC0 || hi == 0xD0) {
                q += 1;
                continue;
            }
            if (q + 1 >= end)
                break;
            uint8_t a = d[q++], b2 = d[q++];
            if (hi == 0x90 && b2 > 0)
                midi_add(m, tick, EV_NOTE_ON, a, b2);
            else if (hi == 0x80 || (hi == 0x90 && b2 == 0))
                midi_add(m, tick, EV_NOTE_OFF, a, 0);
            else if (hi == 0xB0 && a == 64)
                midi_add(m, tick, EV_SUSTAIN, b2 >= 64, 0);
            /* other controllers are ignored */
        }
        p = end;
    }
    free(d);
    qsort(m->ev, m->n, sizeof(*m->ev), cmp_ev);
}

/* Tempo handling: we re-scan the raw file's tempo metas because the compact
 * event above cannot hold 24 bits in two bytes. */
typedef struct {
    uint32_t tick, us;
} tempo_t;

static tempo_t *tempos;
static uint32_t ntempos;

static void midi_tempos(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    if (!d) {
        fclose(f);
        return;
    }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        free(d);
        fclose(f);
        return;
    }
    fclose(f);
    if (sz < 14)
        goto done;
    size_t p = 8 + rd32(d + 4);
    while (p + 8 <= (size_t)sz) {
        if (memcmp(d + p, "MTrk", 4)) {
            p += 8 + rd32(d + p + 4);
            continue;
        }
        size_t len = rd32(d + p + 4), end = p + 8 + len, q = p + 8;
        uint32_t tick = 0;
        uint8_t status = 0, running = 0;
        while (q < end) {
            tick += vlq(d, end, &q);
            if (q >= end)
                break;
            uint8_t b = d[q];
            if (b & 0x80) {
                status = b;
                q++;
            } else {
                b = running;
            }
            if (!status)
                break;
            if (status == 0xFF) {
                uint8_t meta = d[q++];
                uint32_t ml = vlq(d, end, &q);
                if (meta == 0x51 && ml == 3) {
                    uint32_t us = (uint32_t)d[q] << 16 | (uint32_t)d[q + 1] << 8 | d[q + 2];
                    tempos = realloc(tempos, (ntempos + 1) * sizeof(*tempos));
                    tempos[ntempos++] = (tempo_t){tick, us};
                }
                if (meta == 0x2F)
                    break;
                q += ml;
                running = 0;
                continue;
            }
            running = status;
            uint8_t hi = status & 0xF0;
            if (hi == 0xC0 || hi == 0xD0) {
                q++;
                continue;
            }
            if (q + 1 >= end)
                break;
            q += 2;
        }
        p = end;
    }
done:
    free(d);
}

/* ------------------------------------------------------------------ args --- */
static const char *argval(int argc, char **argv, const char *key)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], key))
            return argv[i + 1];
    return NULL;
}

static int has(int argc, char **argv, const char *key)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], key))
            return 1;
    return 0;
}

static int intval(int argc, char **argv, const char *key, int def)
{
    const char *s = argval(argc, argv, key);
    return s ? atoi(s) : def;
}

static double dbval(int argc, char **argv, const char *key, double def)
{
    const char *s = argval(argc, argv, key);
    return s ? atof(s) : def;
}

static void setup(wr_engine_t *e, int argc, char **argv)
{
    wr_init(e, SR);
    wr_set_param(e, WR_P_VOLUME, intval(argc, argv, "--volume", 102));
    wr_set_param(e, WR_P_TREMOLO, intval(argc, argv, "--tremolo", 64));
    wr_set_param(e, WR_P_SPEAKER, intval(argc, argv, "--speaker", 90));
    wr_set_param(e, WR_P_DRIVE, intval(argc, argv, "--drive", 64));
    wr_set_param(e, WR_P_TUNE, intval(argc, argv, "--tune", 0));
}

/* ------------------------------------------------------------------ demo --- */
static void demo_advance(wr_engine_t *e, wav_t *w, uint32_t *pos, uint32_t target)
{
    int32_t buf[512];
    while (*pos < target) {
        uint32_t n = target - *pos > 512 ? 512 : target - *pos;
        wr_render(e, buf, n);
        wav_write(w, buf, n);
        *pos += n;
    }
}

static void demo(wr_engine_t *e, wav_t *w)
{
    /* Four bars of an original minor-key loop at 72 BPM. */
    static const int cm7[] = {48, 51, 55, 58};
    static const int fm7[] = {53, 56, 60, 63};
    static const int g7[] = {55, 59, 62, 65};
    static const int mel[] = {70, 72, 75, 72, 68, 67, 65, 63};
    uint32_t bar = (uint32_t)(SR * 4 * 60.0 / 72.0);
    uint32_t pos = 0;
    for (int b = 0; b < 4; b++) {
        const int *ch = b == 0 || b == 3 ? cm7 : b == 1 ? fm7 : g7;
        uint32_t start = pos;
        for (int i = 0; i < 4; i++)
            wr_note_on(e, ch[i], 78);
        for (int k = 0; k < 4; k++) {
            uint32_t at = start + (uint32_t)(bar * k / 4.0);
            uint32_t off = at + (uint32_t)(bar / 8.0);
            demo_advance(e, w, &pos, at);
            int n = mel[(b * 4 + k) % 8];
            wr_note_on(e, n, 66 + (k % 2) * 24);
            demo_advance(e, w, &pos, off);
            wr_note_off(e, n);
        }
        demo_advance(e, w, &pos, start + bar);
        for (int i = 0; i < 4; i++)
            wr_note_off(e, ch[i]);
    }
    demo_advance(e, w, &pos, pos + SR * 2);
}

/* ------------------------------------------------------------------ main --- */
int main(int argc, char **argv)
{
    if (argc < 2 || has(argc, argv, "--help") || has(argc, argv, "-h")) {
        puts("reed note  --note 60 --vel 100 --seconds 3 --out note.wav");
        puts("reed midi  --in song.mid --out song.wav [--volume 0..127] [--tremolo 0..127]");
        puts("           [--speaker 0..127] [--drive 0..127] [--tune -50..50] [--gain dB]");
        puts("reed demo  --out demo.wav");
        puts("reed bench");
        return 0;
    }

    if (!strcmp(argv[1], "bench")) {
        wr_engine_t e;
        wr_init(&e, SR);
        int notes[] = {48, 52, 55, 60, 64, 67, 72, 76};
        for (int i = 0; i < 8; i++)
            wr_note_on(&e, notes[i], 110);
        int32_t buf[512];
        clock_t t0 = clock();
        uint64_t samples = 0;
        while (samples < SR * 30ull) {
            wr_render(&e, buf, 512);
            samples += 512;
        }
        clock_t t1 = clock();
        double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
        printf("8 voices, 30 s audio in %.3f s: %.1fx realtime, %.1f ns/sample\n",
               secs, 30.0 / secs, secs * 1e9 / (double)samples);
        return 0;
    }

    wr_engine_t e;
    setup(&e, argc, argv);
    double gain_db = dbval(argc, argv, "--gain", 0.0);
    master_gain = pow(10.0, gain_db / 20.0);
    wav_t w;
    const char *out = argval(argc, argv, "--out");
    if (!out)
        out = "out.wav";
    wav_open(&w, out);
    int32_t buf[512];

    if (!strcmp(argv[1], "note")) {
        int note = intval(argc, argv, "--note", 60);
        int vel = intval(argc, argv, "--vel", 100);
        double secs = dbval(argc, argv, "--seconds", 3.0);
        if (!wr_note_on(&e, note, vel)) {
            fprintf(stderr, "note %d out of range (%d..%d)\n", note, WR_MIDI_LO, WR_MIDI_HI);
            return 1;
        }
        uint32_t total = (uint32_t)(secs * SR);
        uint32_t at_off = (uint32_t)(total * 0.66);
        for (uint32_t done = 0; done < total;) {
            uint32_t n = total - done > 512 ? 512 : total - done;
            if (done < at_off && done + n >= at_off)
                n = at_off - done;
            wr_render(&e, buf, n);
            wav_write(&w, buf, n);
            done += n;
            if (done == at_off)
                wr_note_off(&e, note);
        }
        printf("%s: %d frames, note %d vel %d\n", out, w.frames, note, vel);
    } else if (!strcmp(argv[1], "demo")) {
        demo(&e, &w);
        printf("%s: demo, %u frames\n", out, w.frames);
    } else if (!strcmp(argv[1], "midi")) {
        const char *in = argval(argc, argv, "--in");
        if (!in) {
            fprintf(stderr, "reed midi: --in FILE.mid is required\n");
            return 1;
        }
        midi_t m;
        midi_load(&m, in);
        midi_tempos(in);
        uint32_t division = m.division ? m.division : 480;
        uint32_t tempo = 500000, ti = 0;
        double pos_samples = 0.0; /* exact tick->sample composition */
        uint64_t produced = 0;
        uint32_t last_tick = 0;

        /* Event list already sorted. Convert each delta with the tempo map. */
        for (uint32_t i = 0; i < m.n; i++) {
            mev_t *ev = &m.ev[i];
            while (ti < ntempos && tempos[ti].tick <= ev->tick) {
                /* advance to the tempo event's tick first */
                if (tempos[ti].tick > last_tick) {
                    double ticks = tempos[ti].tick - last_tick;
                    pos_samples += ticks * (double)tempo / 1e6 * SR / division;
                    last_tick = tempos[ti].tick;
                }
                tempo = tempos[ti].us;
                ti++;
            }
            if (ev->tick > last_tick) {
                double ticks = ev->tick - last_tick;
                pos_samples += ticks * (double)tempo / 1e6 * SR / division;
                last_tick = ev->tick;
            }
            uint64_t target = (uint64_t)pos_samples;
            while (produced < target) {
                uint32_t n = target - produced > 512 ? 512 : (uint32_t)(target - produced);
                wr_render(&e, buf, n);
                wav_write(&w, buf, n);
                produced += n;
            }
            if (ev->type == EV_NOTE_ON)
                wr_note_on(&e, ev->a, ev->b);
            else if (ev->type == EV_NOTE_OFF)
                wr_note_off(&e, ev->a);
            else if (ev->type == EV_SUSTAIN)
                wr_set_sustain(&e, ev->a);
        }
        /* Tail: 3 s after the last event, or until quiet. */
        for (int i = 0; i < (SR * 3) / 512; i++) {
            wr_render(&e, buf, 512);
            wav_write(&w, buf, 512);
        }
        printf("%s: %u frames, %u events\n", out, w.frames, m.n);
        free(m.ev);
        free(tempos);
    } else {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        return 1;
    }

    wav_close(&w);
    return 0;
}
