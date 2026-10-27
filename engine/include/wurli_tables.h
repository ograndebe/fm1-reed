/* SPDX-License-Identifier: GPL-3.0-only
 * Reed FM-1 — generated fixed-point tables (interface).
 * The data in wurli_tables.c is generated from OpenWurli by tools/tablegen.
 * Do not edit the .c by hand. */
#ifndef WURLI_TABLES_H
#define WURLI_TABLES_H

#include <stdint.h>

#define WR_NNOTE 64
#define WR_NVEL 64
#define WR_NMODES 7
#define WR_NLAW 4
#define WR_MIDI_LO 33
#define WR_MIDI_HI 96
#define WR_TANH_N 512
#define WR_EXP_N 256
#define WR_ONSET_N 256
#define WR_TUNE_N 101
#define WR_VOL_N 128
#define WR_SIN_N 1024
#define WR_SPK_N 33
#define WR_GAIN_DB_N 31

typedef struct {
    int32_t u0_q16;          /* first knot, mm, Q16 */
    int32_t du_q16;          /* knot spacing, mm, Q16 */
    int32_t inv_du_q16;      /* 1/du, Q16 */
    int32_t n;               /* knots */
    const int32_t *y;        /* capacitance, Q30 */
    const int32_t *mh;       /* spline second derivative * du^2/6, Q30 */
} wr_pickup_law_t;

typedef struct {
    uint32_t inc[WR_NMODES];     /* phase increment, cycles/sample, Q32 */
    int32_t cos[WR_NMODES];      /* rotation, Q30 */
    int32_t sin[WR_NMODES];      /* rotation, Q30 */
    int32_t amp[WR_NMODES];      /* mode amplitude, Q30 */
    int32_t decay[WR_NMODES];    /* per-32-sample decay multiplier, Q30 */
    int32_t damper[WR_NMODES];   /* per-32-sample damper multiplier, Q30 */
    int32_t ratio[WR_NMODES];    /* f_n/f_1, Q16 */
    int32_t f0_q16;              /* detuned fundamental, Hz, Q16 */
    int32_t swing_q16;           /* ff swing at the pickup, mm, Q16 */
    int32_t drag_q30;            /* air-drag per block per unit envelope, Q30 */
    int32_t c_rest;              /* pickup law at rest, Q30 */
    int32_t slope_q30;           /* dC/du at rest, per mm, Q30 (lite pickup) */
    uint32_t damper_ramp;        /* damper ramp, samples */
    int8_t law;                  /* index into wr_laws */
    int8_t band;                 /* +1 pickup above reed, -1 below */
    int32_t noise_b0, noise_b2, noise_a1, noise_a2; /* attack-noise BPF, Q28 */
} wr_note_t;

typedef struct {
    int32_t beta;            /* pi * pickup corner / fs, Q30 */
    int32_t inv_den;         /* 1 / (1 + beta), Q30 */
    int32_t sens;            /* pickup sensitivity, Q30 */
    int32_t reed_ratio;      /* C_reed / C_total, Q30 */
    int32_t rest_q16;        /* rest offset, mm, Q16 */
    int32_t fs;              /* sample rate */
    int32_t preamp_hpf;      /* one-pole HPF coefficient, Q30 */
    int32_t amp_shelf;       /* power-amp feedback shelf, Q30 */
    int32_t spk_hp_b0, spk_hp_b1, spk_hp_b2, spk_hp_a1, spk_hp_a2; /* Q28 */
    int32_t spk_lp_b0, spk_lp_b1, spk_lp_b2, spk_lp_a1, spk_lp_a2; /* Q28 */
    int32_t trem_attack;     /* LDR attack per control block, Q30 */
    int32_t trem_release;    /* LDR release per control block, Q30 */
    uint32_t trem_phase_inc; /* 5.63 Hz, Q32 per control block */
} wr_consts_t;

extern const wr_note_t wr_notes[WR_NNOTE];
extern const int32_t wr_vel_drive[WR_NNOTE][WR_NVEL];
extern const int32_t wr_out_gain[WR_NNOTE][WR_NVEL];
extern const wr_pickup_law_t wr_laws[WR_NLAW];
extern const wr_consts_t wr_consts;
extern const int32_t wr_tanh_tab[WR_TANH_N + 1];
extern const int32_t wr_expneg_tab[WR_EXP_N + 1];
extern const int32_t wr_onset_tab[WR_ONSET_N + 1];
extern const int32_t wr_tune_tab[WR_TUNE_N];
extern const int32_t wr_sin_tab[WR_SIN_N + 1];
extern const int32_t wr_spk_inv_tab[WR_SPK_N];
extern const int32_t wr_gain_db_tab[WR_GAIN_DB_N];
extern const int32_t wr_vol_gain_tab[WR_VOL_N];

#endif
