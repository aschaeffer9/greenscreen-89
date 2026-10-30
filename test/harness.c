/* Headless test harness: runs the recompiled core with a scripted input
 * sequence and dumps frames / audio / a framebuffer hash per frame.
 * usage: harness rom.gb frames script.txt outdir [interp]
 * script lines: "<frame> <buttonmask-hex>" (mask stays until next line)
 * dump lines:   "dump <frame>" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/gbcore.h"

static unsigned long long fnv(const uint8_t *p, int n) {
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    static uint8_t romb[0x8000];
    if (!f || fread(romb, 1, 0x8000, f) != 0x8000) { fprintf(stderr, "rom\n"); return 1; }
    fclose(f);
    int frames = atoi(argv[2]);
    int *mask = calloc(frames + 1, sizeof(int));
    char *dump = calloc(frames + 1, 1);
    FILE *s = fopen(argv[3], "r");
    char line[256]; int cur = 0, last = 0;
    while (s && fgets(line, sizeof line, s)) {
        int fr; unsigned m;
        if (sscanf(line, "dump %d", &fr) == 1) { if (fr <= frames) dump[fr] = 1; continue; }
        if (sscanf(line, "%d %x", &fr, &m) == 2) {
            for (int i = last; i < fr && i <= frames; i++) mask[i] = cur;
            cur = (int)m; last = fr;
        }
    }
    for (int i = last; i <= frames; i++) mask[i] = cur;
    if (argc > 5) force_interp = 1;
    gb_init(romb, 0x8000);
    char path[512];
    snprintf(path, sizeof path, "%s/audio.raw", argv[4]);
    FILE *au = fopen(path, "wb");
    snprintf(path, sizeof path, "%s/hashes.txt", argv[4]);
    FILE *hs = fopen(path, "w");
    int16_t abuf[4096 * 2];
    for (int i = 0; i < frames; i++) {
        gb_set_buttons((uint8_t)mask[i]);
        gb_run_frame();
        const uint8_t *fb = gb_framebuffer();
        fprintf(hs, "%d %016llx\n", i, fnv(fb, GB_W * GB_H));
        int n;
        while ((n = gb_audio_read(abuf, 4096)) > 0) fwrite(abuf, 4, n, au);
        if (dump[i]) {
            snprintf(path, sizeof path, "%s/f%05d.pgm", argv[4], i);
            FILE *o = fopen(path, "wb");
            fprintf(o, "P5 160 144 3\n");
            for (int k = 0; k < GB_W * GB_H; k++) fputc(3 - fb[k], o);
            fclose(o);
        }
    }
    fclose(au); fclose(hs);
    fprintf(stderr, "frames=%d interp_instrs=%llu rom_interp_addrs=%u pc=%04x\n",
            frames, gb_interp_count, gb_interp_rom_hits, gb_pc());
    return 0;
}
