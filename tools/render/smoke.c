/* SPDX-License-Identifier: GPL-3.0-only */
/* Quick smoke renderer used during development (not the shipping CLI). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "wurli.h"

int main(int argc, char **argv)
{
    int note = argc > 1 ? atoi(argv[1]) : 60;
    int vel = argc > 2 ? atoi(argv[2]) : 100;
    int secs = argc > 3 ? atoi(argv[3]) : 3;
    wr_engine_t e;
    wr_init(&e, 44100);
    wr_note_on(&e, note, vel);
    int n = 44100 * secs;
    int32_t *buf = malloc(sizeof(int32_t) * n);
    wr_render(&e, buf, n);
    int32_t peak = 0;
    double rms = 0;
    for (int i = 0; i < n; i++) {
        int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > peak) peak = a;
        rms += (double)buf[i] * buf[i];
    }
    fprintf(stderr, "note %d vel %d: peak %.4f (%.1f dBFS) rms %.4f (%.1f dBFS)\n",
            note, vel, peak / 1048576.0, 20 * __builtin_log10(peak / 1048576.0 + 1e-12),
            __builtin_sqrt(rms / n) / 1048576.0,
            20 * __builtin_log10(__builtin_sqrt(rms / n) / 1048576.0 + 1e-12));
    FILE *f = fopen("/tmp/reed-note.wav", "wb");
    uint32_t v;
    fwrite("RIFF", 1, 4, f); v = 36 + n * 4; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t a = 1, ch = 2, ba = 4, bits = 16; uint32_t sr = 44100, br = 44100 * 4;
    fwrite(&a, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); v = n * 4; fwrite(&v, 4, 1, f);
    for (int i = 0; i < n; i++) {
        int32_t s = buf[i];
        if (s > 1048576) s = 1048576;
        if (s < -1048576) s = -1048576;
        int16_t o = (int16_t)(s >> 5);
        int16_t z[2] = {o, o};
        fwrite(z, 2, 2, f);
    }
    fclose(f);
    free(buf);
    return 0;
}
