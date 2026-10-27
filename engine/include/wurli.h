/* SPDX-License-Identifier: GPL-3.0-only
 * Reed FM-1 — portable fixed-point Wurlitzer 200A engine.
 *
 * 44.1 kHz, mono, Q20 output. No libc, no libm, no floating point: the same
 * source runs on the JieLi AC79 (pi32v2, no FPU) and in WebAssembly.
 *
 * Signal chain:
 *   modal reed (7 modes) -> electrostatic pickup overlap law -> hammer dwell
 *   and onset -> preamp stage -> Twin-T/LDR tremolo -> volume pot -> power amp
 *   -> speaker character.
 *
 * The voice (reed, hammer, pickup) is a fixed-point port of OpenWurli. The
 * amplifier and speaker are a calibrated behavioural model; see docs/PORTING.md.
 */
#ifndef WURLI_H
#define WURLI_H

#include <stdint.h>
#include "wurli_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WR_SR 44100
#define WR_BLOCK 32
#define WR_MAX_VOICES 10
#define WR_ACTIVE_VOICES 8

/* Parameters: 0..127, except WR_P_TUNE (-50..+50 cents). */
enum {
    WR_P_VOLUME = 0,
    WR_P_TREMOLO,
    WR_P_DRIVE,
    WR_P_SPEAKER,
    WR_P_TUNE,
    WR_P_COUNT
};

typedef struct {
    int32_t s[WR_NMODES], c[WR_NMODES];
    int32_t env[WR_NMODES], amp[WR_NMODES], amp_env[WR_NMODES];
    int32_t w, s_prev, m_prev;
    int32_t rcos[WR_NMODES], rsin[WR_NMODES];
    int32_t gain_q24;
    int32_t gain_q16;   /* linear fade, 1.0 = 1 << 16 */
    int32_t step_q16;
    uint8_t note;
    uint8_t active;
} wr_fade_t;

typedef struct {
    /* Reed: 7 quadrature modes, Q30. */
    int32_t s[WR_NMODES], c[WR_NMODES];
    int32_t env[WR_NMODES];
    int32_t amp[WR_NMODES];
    int32_t amp_env[WR_NMODES];
    int32_t rcos[WR_NMODES], rsin[WR_NMODES];

    /* Onset ramp. */
    uint32_t onset_pos, onset_inc, onset_len;
    uint8_t onset_soft;

    /* Pickup. */
    int32_t w, s_prev, m_prev;

    /* Attack noise. */
    uint32_t nstate;
    int32_t n_amp, n_s1, n_s2;
    uint16_t n_remain, n_fade;

    /* Damper. */
    uint32_t damp_gain, damp_step, damp_target;
    int32_t damp_progress;
    uint16_t damp_blocks;
    uint8_t damping;

    /* Housekeeping. */
    uint8_t active, gate, sustained;
    uint8_t note, vel;
    uint16_t renorm;
    uint32_t age;
    uint32_t rel_blocks;
    int32_t gain_q24;

    wr_fade_t fade;
} wr_voice_t;

typedef struct {
    wr_voice_t v[WR_MAX_VOICES];

    int32_t knob[WR_P_COUNT];
    int32_t vol_gain;      /* Q24 */
    int32_t drive_gain;    /* Q24, 0.5..2.0 */
    int32_t trem_depth;    /* Q30 */
    int32_t spk_char;      /* Q30 */

    int32_t pre_bias_tanh; /* exact tanh(bias) at Q24, for silence */
    int32_t trem_env;      /* Q30 */
    uint32_t trem_phase;
    int32_t trem_gain;     /* Q30 */
    int32_t spk_inv;       /* Q24 reciprocal of the speaker polynomial norm */
    int32_t pre_lp;        /* one-pole input HPF state, Q24 */
    int32_t amp_lp;        /* 33 Hz feedback shelf state, Q24 */
    int32_t dc_lp;         /* final 12 Hz DC blocker state, Q24 */
    int32_t spk_hp_z1, spk_hp_z2;
    int32_t spk_lp_z1, spk_lp_z2;

    uint32_t age;
    uint32_t rng;
    uint32_t rendered;
    uint8_t sustain;
    uint8_t inited;
} wr_engine_t;

void wr_init(wr_engine_t *e, int sample_rate);
void wr_set_param(wr_engine_t *e, int id, int value);
int wr_get_param(const wr_engine_t *e, int id);

int wr_note_on(wr_engine_t *e, int note, int vel);
void wr_note_off(wr_engine_t *e, int note);
void wr_set_sustain(wr_engine_t *e, int on);
void wr_all_notes_off(wr_engine_t *e);
void wr_panic(wr_engine_t *e);

/* Render `frames` mono samples in Q20 (1.0 = 1<<20), adding nothing else. */
void wr_render(wr_engine_t *e, int32_t *out, uint32_t frames);

/* Render stereo Q15 (interleaved L,R), duplicating the mono engine output. */
void wr_render_stereo(wr_engine_t *e, int16_t *out, uint32_t frames);

int wr_active_voices(const wr_engine_t *e);
int wr_version(void);

#ifdef __cplusplus
}
#endif

#endif
