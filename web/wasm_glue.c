/* SPDX-License-Identifier: GPL-3.0-only
 * WebAssembly glue: one static engine and one output buffer, no libc.
 * The AudioWorklet copies the Q20 samples out of linear memory. */
#include "wurli.h"

static wr_engine_t eng;
static int32_t out_buf[2048];

__attribute__((export_name("wr_init_web")))
void wr_init_web(void)
{
    wr_init(&eng, WR_SR);
}

__attribute__((export_name("wr_note_on_web")))
int wr_note_on_web(int note, int vel)
{
    return wr_note_on(&eng, note, vel);
}

__attribute__((export_name("wr_note_off_web")))
void wr_note_off_web(int note)
{
    wr_note_off(&eng, note);
}

__attribute__((export_name("wr_sustain_web")))
void wr_sustain_web(int on)
{
    wr_set_sustain(&eng, on);
}

__attribute__((export_name("wr_param_web")))
void wr_param_web(int id, int value)
{
    wr_set_param(&eng, id, value);
}

__attribute__((export_name("wr_all_off_web")))
void wr_all_off_web(void)
{
    wr_all_notes_off(&eng);
}

__attribute__((export_name("wr_panic_web")))
void wr_panic_web(void)
{
    wr_panic(&eng);
}

__attribute__((export_name("wr_voices_web")))
int wr_voices_web(void)
{
    return wr_active_voices(&eng);
}

__attribute__((export_name("wr_out_ptr")))
int32_t *wr_out_ptr(void)
{
    return out_buf;
}

__attribute__((export_name("wr_render_web")))
void wr_render_web(int frames)
{
    if (frames <= 0)
        return;
    if (frames > (int)(sizeof(out_buf) / sizeof(out_buf[0])))
        frames = (int)(sizeof(out_buf) / sizeof(out_buf[0]));
    wr_render(&eng, out_buf, (uint32_t)frames);
}
