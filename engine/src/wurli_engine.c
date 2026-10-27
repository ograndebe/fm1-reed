/* SPDX-License-Identifier: GPL-3.0-only
 * Reed FM-1 engine — fixed-point Wurlitzer 200A.
 *
 * Compiled for the JieLi AC79 (clang, pi32v2) and for WebAssembly with the
 * same C99 source. No floating point, no libc, no libm.
 *
 * Q formats:
 *   audio bus      Q20 (1.0 = 1 << 20)
 *   FX / gain      Q24 (1.0 = 1 << 24)
 *   reed / pickup  Q30 (1.0 = 1 << 30)
 *   phase          Q32 (2^32 = one cycle)
 *   biquad coeffs  Q28
 */
#include "wurli.h"

#define Q30_ONE 1073741824
#define Q24_ONE 16777216
#define Q20_ONE 1048576

#define PREAMP_VT_Q24 1342177          /* 0.08 V effective input nonlinearity */
#define INV_PREAMP_VT_Q24 209715200    /* 1/0.08, Q24 */
#define PREAMP_SCALE_Q24 571000        /* 6.17 * 0.08 / 14.5 */
#define PREAMP_BIAS_Q24 2516582        /* +0.15 (even-harmonic bias) */
#define PREAMP_TANH_BIAS_Q24 2497886   /* tanh(0.15) */
#define AMP_GAIN_Q24 765041050         /* 69.2 * 14.5/22, closed-loop amp */
#define A2_Q24 3355443                 /* 0.2 * character */
#define A3_Q24 10066330                /* 0.6 * character */
#define TREM_MAX_Q30 590558003         /* 0.55 depth ceiling */
#define OUTPUT_TRIM_Q24 26591104       /* +4 dB, matches OpenWurli plugin alignment */
#define NOISE_AMP_Q30 26843546         /* 0.025 */
#define NOISE_DECAY_Q30 1065504        /* exp(-1/(0.003*44100)) */
#define ONSET_END (256u << 16)
#define FADE_LEN 220u                  /* 5 ms */
#define DC_BLOCK_Q30 1833500           /* 1-exp(-2*pi*12/44100) */
#ifndef WR_CROSS_D2
#define WR_CROSS_D2 5                   /* power-amp crossover knee, Q24^2 */
#endif

static inline int32_t mul30(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 30);
}
static inline int32_t mul28(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 28);
}
static inline int32_t mul24(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 24);
}
static inline int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* sin(phase), phase Q32, table + linear interpolation. */
static inline int32_t sin_q30(uint32_t phase)
{
    uint32_t i = phase >> 22;
    uint32_t f = (phase >> 6) & 0xFFFFu;
    int32_t a = wr_sin_tab[i];
    int32_t b = wr_sin_tab[i + 1];
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)f) >> 16);
}
static inline int32_t cos_q30(uint32_t phase)
{
    return sin_q30(phase + 0x40000000u);
}

/* tanh(x), x Q24, table over [-4, +4]. */
static inline int32_t tanh_q24(int32_t x)
{
    if (x <= -(4 << 24))
        return -(1 << 24);
    if (x >= (4 << 24))
        return 1 << 24;
    uint32_t u = (uint32_t)(x + (4 << 24));
    uint32_t i = u >> 18;
    uint32_t f = (u >> 2) & 0xFFFFu;
    int32_t a = wr_tanh_tab[i];
    int32_t b = wr_tanh_tab[i + 1];
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)f) >> 16);
}

/* exp(-z), z Q16, table 0..16 in steps of 1/16. */
static inline int32_t expneg_q30(int32_t z)
{
    if (z <= 0)
        return Q30_ONE;
    if (z >= (16 << 16))
        return 0;
    uint32_t i = (uint32_t)z >> 12;
    uint32_t f = (uint32_t)z & 0xFFFu;
    int32_t a = wr_expneg_tab[i];
    int32_t b = wr_expneg_tab[i + 1];
    return a + (int32_t)(((int64_t)(b - a) * (int32_t)f) >> 12);
}

static uint32_t isqrt32(uint32_t n)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* ---------------------------------------------------------------- pickup --- */
/* One sample of the electrostatic pickup: displacement Q30 -> volts Q30.
 * lite = linear law + the same node recursion (no overlap-law harmonics). */
static inline int32_t pickup_step(int32_t beam, const wr_note_t *nt, int lite,
                                  int32_t *w, int32_t *s_prev, int32_t *m_prev)
{
    int32_t u = (int32_t)(((int64_t)nt->band * nt->swing_q16 * beam) >> 30) -
                wr_consts.rest_q16;
    int32_t s;

    if (lite) {
        int32_t d = u + wr_consts.rest_q16; /* displacement, mm Q16 */
        s = (int32_t)(((int64_t)nt->slope_q30 * d) >> 16);
        s = clamp32(s, -(1 << 29), 1 << 29);
    } else {
        const wr_pickup_law_t *law = &wr_laws[nt->law];
        int64_t pos64 = ((int64_t)(u - law->u0_q16) * law->inv_du_q16) >> 16;
        int32_t last = (law->n - 1) << 16;
        int32_t pos = pos64 < 0 ? 0 : pos64 > last ? last : (int32_t)pos64;
        uint32_t i = (uint32_t)pos >> 16;
        uint32_t f = (uint32_t)pos & 0xFFFFu;
        uint32_t a = 65536u - f;

        int32_t lin = (int32_t)(((int64_t)(int32_t)a * law->y[i] +
                                 (int64_t)(int32_t)f * law->y[i + 1]) >> 16);
        int32_t a3 = (int32_t)(((((int64_t)(int32_t)a * (int32_t)a) >> 16) *
                                (int32_t)a) >> 16) - (int32_t)a;
        int32_t f3 = (int32_t)(((((int64_t)(int32_t)f * (int32_t)f) >> 16) *
                                (int32_t)f) >> 16) - (int32_t)f;
        int32_t cub = (int32_t)(((int64_t)a3 * law->mh[i] +
                                 (int64_t)f3 * law->mh[i + 1]) >> 16);
        s = lin + cub - nt->c_rest;
    }

    int32_t m = Q30_ONE + mul30(s, wr_consts.reed_ratio);
    int32_t m_avg = (int32_t)(((int64_t)*m_prev + m) >> 1);

    int64_t wq = ((int64_t)*w * (m_avg - wr_consts.beta)) >> 30;
    int64_t num = wq - (int64_t)(s - *s_prev);
    int64_t wn = (num * wr_consts.inv_den) >> 30;
    if (wn > (4LL << 30))
        wn = 4LL << 30;
    else if (wn < -(4LL << 30))
        wn = -(4LL << 30);

    *w = (int32_t)wn;
    *s_prev = s;
    *m_prev = m;
    return mul30(*w, wr_consts.sens);
}

/* ---------------------------------------------------------------- voices --- */
static void voice_renorm(wr_voice_t *v)
{
    for (int m = 0; m < v->modes; m++) {
        int32_t s = v->s[m], c = v->c[m];
        uint32_t r2 = (uint32_t)(mul30(s, s) + mul30(c, c));
        uint32_t r = isqrt32(r2);
        if (r < 16384u)
            r = 16384u; /* 0.5 */
        if (r > 65536u)
            r = 65536u; /* 2.0 */
        uint32_t inv = (1u << 30) / r; /* Q15 */
        v->s[m] = (int32_t)(((int64_t)s * (int32_t)inv) >> 15);
        v->c[m] = (int32_t)(((int64_t)c * (int32_t)inv) >> 15);
    }
}

static void voice_start(wr_engine_t *e, wr_voice_t *v, int note, int vel)
{
    const wr_note_t *nt = &wr_notes[note - WR_MIDI_LO];
    int tune = clamp32(e->knob[WR_P_TUNE], -50, 50);
    int32_t tune_gain = wr_tune_tab[tune + 50];

    v->active = 1;
    v->gate = 1;
    v->sustained = 0;
    v->note = (uint8_t)note;
    v->vel = (uint8_t)vel;
    v->modes = (uint8_t)clamp32(e->profile.modes, 1, WR_NMODES);
    v->pickup = e->profile.pickup;
    v->noise_on = e->profile.noise;
    v->drag_on = e->profile.drag;
    v->age = ++e->age;
    v->rel_blocks = 0;
    v->damping = 0;
    v->damp_blocks = 0;
    v->damp_progress = 0;
    v->damp_step = 0;
    v->renorm = 0;
    v->w = 0;
    v->s_prev = 0;
    v->m_prev = Q30_ONE;
    v->n_s1 = v->n_s2 = 0;
    v->n_remain = v->noise_on ? 661 : 0; /* 15 ms */
    v->n_fade = 16;
    v->nstate = 0x9e3779b9u ^ ((uint32_t)note * 2654435761u) ^ e->age;
    v->fade.active = 0;

    {
        int32_t vq = (vel << 24) / 127; /* Q24 */
        int32_t v2 = mul24(vq, vq);     /* Q24 */
        v->n_amp = (int32_t)(((int64_t)NOISE_AMP_Q30 * v2) >> 24);
    }

    /* Velocity layer: 64 layers, MIDI velocity >> 1. */
    int layer = vel >> 1;
    if (layer > WR_NVEL - 1)
        layer = WR_NVEL - 1;
    int32_t drive = wr_vel_drive[note - WR_MIDI_LO][layer];
    v->gain_q24 = wr_out_gain[note - WR_MIDI_LO][layer];

    /* Hammer dwell: attenuation follows f_n * contact time, i.e. the mode
     * ratio and the velocity-dependent number of cycles, not the note. */
    int32_t cycles = ((254 - vel) << 16) / 127; /* Q16, 1.0 (ff) .. 2.0 (pp) */
    int32_t z0 = (int32_t)((((int64_t)cycles * cycles) >> 16) >> 7);
    for (int m = 0; m < WR_NMODES; m++) {
        if (m < v->modes) {
            int32_t ft = (int32_t)(((int64_t)nt->ratio[m] * cycles) >> 16);
            int32_t z = (int32_t)((((int64_t)ft * ft) >> 16) >> 7);
            int32_t dwell = (m == 0) ? Q30_ONE : expneg_q30(z - z0);
            int32_t amp = mul30(nt->amp[m], dwell);
            v->amp[m] = mul30(amp, drive);

            uint32_t inc = nt->inc[m];
            if (tune != 0) {
                inc = (uint32_t)(((uint64_t)inc * (uint32_t)tune_gain) >> 30);
                v->rcos[m] = cos_q30(inc);
                v->rsin[m] = sin_q30(inc);
            } else {
                v->rcos[m] = nt->cos[m];
                v->rsin[m] = nt->sin[m];
            }
        } else {
            v->amp[m] = 0;
            v->rcos[m] = Q30_ONE;
            v->rsin[m] = 0;
        }
        v->s[m] = 0;
        v->c[m] = Q30_ONE;
        v->env[m] = Q30_ONE;
    }

    /* Onset: 1 period at ff, 2 at pp; 2 ms floor. */
    {
        uint32_t period = ((uint32_t)WR_SR << 16) / (uint32_t)nt->f0_q16;
        uint32_t onset = (uint32_t)(((uint64_t)period * (uint32_t)cycles) >> 16);
        if (onset < (88u << 16))
            onset = 88u << 16;
        v->onset_len = onset >> 16;
        if (v->onset_len == 0)
            v->onset_len = 1;
        v->onset_inc = (256u << 16) / v->onset_len;
        v->onset_pos = 0;
        v->onset_soft = vel < 64 ? 1 : 0;
    }
}

static void voice_block(wr_voice_t *v, int32_t *mix, uint32_t n)
{
    const wr_note_t *nt = &wr_notes[v->note - WR_MIDI_LO];

    /* Block-rate envelopes. */
    for (int m = 0; m < v->modes; m++) {
        int32_t env = v->env[m];
        if (v->damping) {
            int32_t g = Q30_ONE + mul30(nt->damper[m] - Q30_ONE, v->damp_progress);
            env = mul30(env, g);
        }
        env = mul30(env, nt->decay[m]);
        v->env[m] = env;
        v->amp_env[m] = mul30(v->amp[m], env);
    }
    if (v->damping && v->damp_blocks) {
        v->damp_progress += v->damp_step;
        if (v->damp_progress >= Q30_ONE) {
            v->damp_progress = Q30_ONE;
            v->damp_blocks = 0;
        } else {
            v->damp_blocks--;
        }
    }
    if (nt->drag_q30 && v->drag_on) {
        int32_t d = mul30(nt->drag_q30, v->amp[0]);
        int32_t c = mul30(d, v->env[0]);
        v->env[0] = mul30(v->env[0], Q30_ONE - c);
        v->amp_env[0] = mul30(v->amp[0], v->env[0]);
    }

    for (uint32_t i = 0; i < n; i++) {
        int32_t on = Q30_ONE;
        if (v->onset_pos < ONSET_END) {
            uint32_t idx = v->onset_pos >> 16;
            uint32_t fr = v->onset_pos & 0xFFFFu;
            int32_t a = wr_onset_tab[idx];
            int32_t b = wr_onset_tab[idx + 1];
            on = a + (int32_t)(((int64_t)(b - a) * (int32_t)fr) >> 16);
            if (v->onset_soft)
                on = mul30(on, on);
            v->onset_pos += v->onset_inc;
        }

        int64_t beam = 0;
        for (int m = 0; m < v->modes; m++) {
            beam += mul30(v->amp_env[m], v->s[m]);
            int32_t s = v->s[m], c = v->c[m];
            v->s[m] = mul30(s, v->rcos[m]) + mul30(c, v->rsin[m]);
            v->c[m] = mul30(c, v->rcos[m]) - mul30(s, v->rsin[m]);
        }
        if (beam > (2LL << 30))
            beam = 2LL << 30;
        else if (beam < -(2LL << 30))
            beam = -(2LL << 30);
        beam = (beam * on) >> 30;

        /* Hammer attack noise, added to the reed displacement. */
        if (v->noise_on && v->n_remain) {
            uint32_t st = v->nstate * 1664525u + 1013904223u;
            v->nstate = st;
            int32_t x = (int32_t)st >> 1; /* Q30 */
            int32_t y = mul28(nt->noise_b0, x) + v->n_s1;
            v->n_s1 = -mul28(nt->noise_a1, y) + v->n_s2;
            v->n_s2 = mul28(nt->noise_b2, x) - mul28(nt->noise_a2, y);
            int32_t env = v->n_amp;
            if (v->n_fade) {
                int idx = (16 - v->n_fade) * 16;
                if (idx > WR_ONSET_N)
                    idx = WR_ONSET_N;
                env = mul30(env, wr_onset_tab[idx]);
                v->n_fade--;
            }
            beam += mul30(env, y);
            v->n_amp = mul30(v->n_amp, NOISE_DECAY_Q30);
            v->n_remain--;
        }

        int32_t out = pickup_step((int32_t)beam, nt, v->pickup == WR_PICKUP_LITE,
                                  &v->w, &v->s_prev, &v->m_prev);
        out = mul24(out, v->gain_q24) >> 10; /* Q30 -> Q20 */
        mix[i] += out;
    }

    if (++v->renorm >= 32) {
        v->renorm = 0;
        voice_renorm(v);
    }

    if (!v->gate) {
        v->rel_blocks++;
        int silent = 1;
        for (int m = 0; m < v->modes; m++)
            if (v->env[m] >= (1 << 14))
                silent = 0;
        if ((v->damping && silent) || v->rel_blocks > 13781u)
            v->active = 0; /* ~10 s */
    }
}

/* --------------------------------------------------------------- stealing --- */
static void fade_take(wr_fade_t *f, const wr_voice_t *v)
{
    f->note = v->note;
    f->modes = v->modes;
    f->pickup = v->pickup;
    f->gain_q24 = v->gain_q24;
    f->gain_q16 = 1 << 16;
    f->step_q16 = (1 << 16) / FADE_LEN;
    f->active = 1;
    f->w = v->w;
    f->s_prev = v->s_prev;
    f->m_prev = v->m_prev;
    for (int m = 0; m < WR_NMODES; m++) {
        f->s[m] = v->s[m];
        f->c[m] = v->c[m];
        f->env[m] = v->env[m];
        f->amp[m] = v->amp[m];
        f->amp_env[m] = 0;
        f->rcos[m] = v->rcos[m];
        f->rsin[m] = v->rsin[m];
    }
}

static void fade_block(wr_fade_t *f, int32_t *mix, uint32_t n)
{
    if (!f->active)
        return;
    const wr_note_t *nt = &wr_notes[f->note - WR_MIDI_LO];
    for (int m = 0; m < f->modes; m++) {
        f->env[m] = mul30(f->env[m], nt->decay[m]);
        f->amp_env[m] = mul30(f->amp[m], f->env[m]);
    }
    for (uint32_t i = 0; i < n; i++) {
        int64_t beam = 0;
        for (int m = 0; m < f->modes; m++) {
            beam += mul30(f->amp_env[m], f->s[m]);
            int32_t s = f->s[m], c = f->c[m];
            f->s[m] = mul30(s, f->rcos[m]) + mul30(c, f->rsin[m]);
            f->c[m] = mul30(c, f->rcos[m]) - mul30(s, f->rsin[m]);
        }
        if (beam > (2LL << 30))
            beam = 2LL << 30;
        else if (beam < -(2LL << 30))
            beam = -(2LL << 30);
        int32_t out = pickup_step((int32_t)beam, nt, f->pickup == WR_PICKUP_LITE,
                                  &f->w, &f->s_prev, &f->m_prev);
        out = mul24(out, f->gain_q24) >> 10;
        out = (int32_t)(((int64_t)out * f->gain_q16) >> 16);
        mix[i] += out;
        f->gain_q16 -= f->step_q16;
        if (f->gain_q16 < 0)
            f->gain_q16 = 0;
    }
    if (f->gain_q16 == 0)
        f->active = 0;
}

/* ------------------------------------------------------- runtime profile --- */
int wr_set_voice_limit(wr_engine_t *e, int voices)
{
    if (voices < 1 || voices > WR_MAX_VOICES)
        return 0;
    if (voices < e->voice_limit) {
        /* Do not cut sounding voices abruptly: let the slots above the new
         * ceiling finish in the 5 ms steal fade. */
        for (int i = voices; i < WR_MAX_VOICES; i++) {
            wr_voice_t *v = &e->v[i];
            if (v->active) {
                fade_take(&v->fade, v);
                v->active = 0;
            }
        }
    }
    e->voice_limit = (uint8_t)voices;
    e->profile.voices = (uint8_t)voices;
    return 1;
}

int wr_set_profile(wr_engine_t *e, const wr_profile_t *p)
{
    if (!p || p->voices < 1 || p->voices > WR_MAX_VOICES || p->modes < 1 ||
        p->modes > WR_NMODES || p->pickup > WR_PICKUP_LITE ||
        p->noise > 1 || p->drag > 1 || p->chain > WR_CHAIN_LITE)
        return 0;
    e->profile = *p;
    return wr_set_voice_limit(e, p->voices);
}

wr_profile_t wr_get_profile(const wr_engine_t *e)
{
    return e->profile;
}

int wr_get_voice_limit(const wr_engine_t *e)
{
    return e->voice_limit;
}

void wr_set_master_q24(wr_engine_t *e, int32_t gain_q24)
{
    if (gain_q24 < 0)
        gain_q24 = 0;
    if (gain_q24 > 2 * Q24_ONE)
        gain_q24 = 2 * Q24_ONE;
    e->master_q24 = gain_q24;
}

int32_t wr_get_master_q24(const wr_engine_t *e)
{
    return e->master_q24;
}

/* ------------------------------------------------------------------- FX --- */
static inline int32_t biquad(int32_t *z1, int32_t *z2,
                             int32_t b0, int32_t b1, int32_t b2,
                             int32_t a1, int32_t a2, int32_t x)
{
    int32_t y = mul28(b0, x) + *z1;
    *z1 = mul28(b1, x) - mul28(a1, y) + *z2;
    *z2 = mul28(b2, x) - mul28(a2, y);
    return y;
}

static void control_block(wr_engine_t *e)
{
    e->vol_gain = wr_vol_gain_tab[clamp32(e->knob[WR_P_VOLUME], 0, 127)];
    int32_t d = clamp32(e->knob[WR_P_DRIVE], 0, 127);
    e->drive_gain = (Q24_ONE >> 1) + (int32_t)(((int64_t)d * Q24_ONE) / 127);
    int32_t td = clamp32(e->knob[WR_P_TREMOLO], 0, 127);
    e->trem_depth = (int32_t)(((int64_t)td * Q30_ONE) / 127);
    int32_t sc = clamp32(e->knob[WR_P_SPEAKER], 0, 127);
    e->spk_char = (int32_t)(((int64_t)sc * Q30_ONE) / 127);

    /* Twin-T oscillator (5.63 Hz) -> CdS LDR envelope -> depth divider. */
    e->trem_phase += wr_consts.trem_phase_inc;
    int32_t lfo = sin_q30(e->trem_phase);
    if (lfo < 0)
        lfo = 0; /* half-wave, as the LED drive */
    int32_t coeff = lfo > e->trem_env ? wr_consts.trem_attack : wr_consts.trem_release;
    e->trem_env += mul30(coeff, lfo - e->trem_env);
    int32_t mod = mul30(e->trem_depth, Q30_ONE - e->trem_env);
    e->trem_gain = Q30_ONE - mul30(mod, TREM_MAX_Q30);

    int si = (sc * (WR_SPK_N - 1)) / 127;
    e->spk_inv = wr_spk_inv_tab[si];
}

static inline int32_t chain_sample(wr_engine_t *e, int32_t xq20)
{
    int32_t x = xq20 * 16; /* Q20 -> Q24, volts (multiplication: the value may be negative) */

    /* Preamp input coupling: 22 nF into ~21.5k, ~336 Hz. */
    int32_t lp = e->pre_lp;
    lp += mul30(wr_consts.preamp_hpf, x - lp);
    e->pre_lp = lp;
    int32_t hp = x - lp;

    int32_t pre = mul24(hp, e->drive_gain);
    pre = mul24(pre, INV_PREAMP_VT_Q24);
    int32_t y = tanh_q24(pre + PREAMP_BIAS_Q24) - e->pre_bias_tanh;
    y = mul24(y, PREAMP_SCALE_Q24); /* preamp out, 1.0 = 14.5 V rail */

    y = mul30(y, e->trem_gain);
    y = mul24(y, e->vol_gain);

    /* Power amp: 33 Hz feedback shelf, closed-loop 69x, rail soft clip. */
    lp = e->amp_lp;
    lp += mul30(wr_consts.amp_shelf, y - lp);
    e->amp_lp = lp;
    hp = y - lp;
    int32_t v = mul24(hp, AMP_GAIN_Q24);
    int32_t v2 = mul24(v, v);
    int32_t ratio;
    if (v2 > 1024) {
        ratio = Q24_ONE;
    } else {
        uint32_t inv = (1u << 30) / (uint32_t)(v2 + WR_CROSS_D2);
        ratio = (int32_t)(((int64_t)v2 * (int32_t)inv) >> 6);
    }
    int32_t a = tanh_q24(mul24(v, ratio));

    /* Speaker character: polynomial + cone limit + 30 Hz / 5.5 kHz. */
    int32_t c = e->spk_char;
    if (e->profile.chain == WR_CHAIN_FULL && c > 0) {
        int32_t x2 = mul24(a, a);
        int32_t x3 = mul24(x2, a);
        int32_t a2 = mul30(c, A2_Q24);
        int32_t a3 = mul30(c, A3_Q24);
        int32_t num = a + mul24(a2, x2) + mul24(a3, x3);
        int32_t shaped = mul30(num, e->spk_inv);
        int32_t lim = tanh_q24(shaped);
        int32_t f = biquad(&e->spk_hp_z1, &e->spk_hp_z2,
                           wr_consts.spk_hp_b0, wr_consts.spk_hp_b1, wr_consts.spk_hp_b2,
                           wr_consts.spk_hp_a1, wr_consts.spk_hp_a2, lim);
        f = biquad(&e->spk_lp_z1, &e->spk_lp_z2,
                   wr_consts.spk_lp_b0, wr_consts.spk_lp_b1, wr_consts.spk_lp_b2,
                   wr_consts.spk_lp_a1, wr_consts.spk_lp_a2, f);
        a = a + mul30(c, f - a);
    }

    /* Final DC blocker: the quantised 30 Hz speaker HPF can leave a small
     * residual DC, and the preamp bias is asymmetric by design. */
    e->dc_lp += mul30(DC_BLOCK_Q30, a - e->dc_lp);
    a -= e->dc_lp;
    a = mul24(a, e->master_q24);

    return clamp32(mul24(a, OUTPUT_TRIM_Q24), -(1 << 24), 1 << 24);
}

/* --------------------------------------------------------------- public --- */
void wr_init(wr_engine_t *e, int sample_rate)
{
    (void)sample_rate; /* 44100 only in v0.1; see docs/PORTING.md */
    uint8_t *p = (uint8_t *)e;
    for (uint32_t i = 0; i < sizeof(*e); i++)
        p[i] = 0;
    e->profile.voices = WR_DEFAULT_VOICES;
    e->profile.modes = WR_NMODES;
    e->profile.pickup = WR_PICKUP_FULL;
    e->profile.noise = 1;
    e->profile.drag = 1;
    e->profile.chain = WR_CHAIN_FULL;
    e->voice_limit = WR_DEFAULT_VOICES;
    e->master_q24 = Q24_ONE;
    e->knob[WR_P_VOLUME] = 102;   /* 0.8 */
    e->knob[WR_P_TREMOLO] = 64;   /* 0.5 */
    e->knob[WR_P_DRIVE] = 64;     /* 1.0 */
    e->knob[WR_P_SPEAKER] = 90;   /* 0.7 */
    e->knob[WR_P_TUNE] = 0;
    e->trem_env = 0;
    e->trem_phase = 0;
    e->rng = 0x12345678u;
    e->pre_bias_tanh = tanh_q24(PREAMP_BIAS_Q24);
    e->inited = 1;
    control_block(e);
}

void wr_set_param(wr_engine_t *e, int id, int value)
{
    if (id < 0 || id >= WR_P_COUNT)
        return;
    if (id == WR_P_TUNE) {
        if (value < -50)
            value = -50;
        if (value > 50)
            value = 50;
    } else {
        if (value < 0)
            value = 0;
        if (value > 127)
            value = 127;
    }
    e->knob[id] = value;
}

int wr_get_param(const wr_engine_t *e, int id)
{
    return (id >= 0 && id < WR_P_COUNT) ? e->knob[id] : 0;
}

int wr_note_on(wr_engine_t *e, int note, int vel)
{
    if (note < WR_MIDI_LO || note > WR_MIDI_HI || vel <= 0)
        return 0;
    if (vel > 127)
        vel = 127;

    wr_voice_t *slot = 0;
    wr_voice_t *reuse = 0;
    int limit = clamp32(e->voice_limit, 1, WR_MAX_VOICES);
    for (int i = 0; i < limit; i++) {
        if (e->v[i].active && e->v[i].note == note && e->v[i].gate) {
            reuse = &e->v[i];
            break;
        }
        if (!e->v[i].active && !slot)
            slot = &e->v[i];
    }

    if (!slot) {
        /* Prefer a released voice; else the quietest / oldest held one. */
        int32_t best = 0x7FFFFFFF;
        for (int i = 0; i < limit; i++) {
            wr_voice_t *c = &e->v[i];
            if (!c->active)
                continue;
            int32_t score = c->gate ? 0x40000000 : 0;
            score += (int32_t)(c->env[0] >> 4) + (int32_t)(c->age & 0xFFFF);
            if (score < best) {
                best = score;
                slot = c;
            }
        }
        if (!slot)
            slot = &e->v[0];
    }

    wr_voice_t *target = reuse ? reuse : slot;
    if (target->active) {
        fade_take(&target->fade, target);
        /* Keep the previous fade if it is still running? It has already been
         * copied; the new fade replaces it. */
    }
    voice_start(e, target, note, vel);
    return 1;
}

static void start_damper(wr_engine_t *e, wr_voice_t *v)
{
    (void)e;
    const wr_note_t *nt = &wr_notes[v->note - WR_MIDI_LO];
    if (v->note >= 92)
        return; /* top five reeds have no damper */
    v->damping = 1;
    v->damp_progress = 0;
    v->damp_blocks = (uint16_t)(nt->damper_ramp / WR_BLOCK);
    if (!v->damp_blocks)
        v->damp_blocks = 1;
    v->damp_step = Q30_ONE / v->damp_blocks;
}

void wr_note_off(wr_engine_t *e, int note)
{
    for (int i = 0; i < WR_MAX_VOICES; i++) {
        wr_voice_t *v = &e->v[i];
        if (v->active && v->gate && v->note == note) {
            v->gate = 0;
            v->rel_blocks = 0;
            if (e->sustain)
                v->sustained = 1;
            else
                start_damper(e, v);
        }
    }
}

void wr_set_sustain(wr_engine_t *e, int on)
{
    e->sustain = on ? 1 : 0;
    if (!e->sustain) {
        for (int i = 0; i < WR_MAX_VOICES; i++) {
            wr_voice_t *v = &e->v[i];
            if (v->active && v->sustained) {
                v->sustained = 0;
                start_damper(e, v);
            }
        }
    }
}

void wr_all_notes_off(wr_engine_t *e)
{
    e->sustain = 0;
    for (int i = 0; i < WR_MAX_VOICES; i++) {
        wr_voice_t *v = &e->v[i];
        if (v->active && v->gate) {
            v->gate = 0;
            v->sustained = 0;
            v->rel_blocks = 0;
            start_damper(e, v);
        }
    }
}

void wr_panic(wr_engine_t *e)
{
    for (int i = 0; i < WR_MAX_VOICES; i++) {
        e->v[i].active = 0;
        e->v[i].fade.active = 0;
    }
    e->sustain = 0;
    e->pre_lp = e->amp_lp = e->dc_lp = 0;
    e->spk_hp_z1 = e->spk_hp_z2 = e->spk_lp_z1 = e->spk_lp_z2 = 0;
}

void wr_render(wr_engine_t *e, int32_t *out, uint32_t frames)
{
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > WR_BLOCK)
            n = WR_BLOCK;
        control_block(e);

        int32_t mix[WR_BLOCK];
        for (uint32_t i = 0; i < n; i++)
            mix[i] = 0;

        for (int i = 0; i < WR_MAX_VOICES; i++) {
            wr_voice_t *v = &e->v[i];
            if (v->active)
                voice_block(v, mix, n);
            if (v->fade.active)
                fade_block(&v->fade, mix, n);
        }

        for (uint32_t i = 0; i < n; i++) {
#ifdef WR_DRY
            out[done + i] = mix[i];
#else
            int32_t y = chain_sample(e, mix[i]);
            out[done + i] = y >> 4; /* Q24 -> Q20 */
#endif
        }
        done += n;
        e->rendered += n;
    }
}

void wr_render_stereo(wr_engine_t *e, int16_t *out, uint32_t frames)
{
    int32_t buf[64];
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > 64)
            n = 64;
        wr_render(e, buf, n);
        for (uint32_t i = 0; i < n; i++) {
            int32_t s = buf[i];
            if (s > Q20_ONE)
                s = Q20_ONE;
            else if (s < -Q20_ONE)
                s = -Q20_ONE;
            int16_t v = (int16_t)(s >> 5); /* Q20 -> Q15 */
            out[(done + i) * 2] = v;
            out[(done + i) * 2 + 1] = v;
        }
        done += n;
    }
}

int wr_active_voices(const wr_engine_t *e)
{
    int n = 0;
    for (int i = 0; i < WR_MAX_VOICES; i++)
        if (e->v[i].active)
            n++;
    return n;
}

int wr_version(void)
{
    return 0x000100; /* 0.1.0 */
}
