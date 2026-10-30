/* CPU opponent for 2-player link games: plays a Game Boy through its
 * buttons only, like a person would.
 *  - when a new piece appears, it learns the piece's four rotations by
 *    pressing A in a throwaway lookahead and reading the piece's sprites;
 *  - it drops every rotation at every column on a copy of the board,
 *    scores the result (height, holes, bumpiness, lines) and picks a spot;
 *  - then it steers there with real presses at its difficulty's pace and
 *    drops (hard drop on HARD, soft drop on NORMAL, mostly lets it fall on EASY). */
#include <string.h>
#include <stdlib.h>
#include "cpubot.h"

#define GS        0xFFE1
#define PAUSED    0xFFAB
#define FALL      0xFF98
#define SHIFTING  0xFFE3
#define PIECE_Y   0xC201
#define PIECE_X   0xC202
#define PIECE_SPEC 0xC203
#define OAM_PIECE 0xC010     /* the falling piece's 4 sprites (shadow OAM) */
#define BOARD     0xC800     /* game screen buffer, 32 tiles wide, play field at columns 2..11 */
#define EMPTY_TILE 0x2F

enum { B_IDLE, B_THINK, B_STEER, B_DROP, B_WAIT_LOCK };

static uint32_t rnd(CpuBot *b) { b->seed = b->seed * 1664525u + 1013904223u; return b->seed >> 8; }

static void read_board(CpuBot *b, uint8_t g[18][10]) {
    for (int y = 0; y < 18; y++)
        for (int x = 0; x < 10; x++)
            g[y][x] = b->c.peek((uint16_t)(BOARD + y * 32 + 2 + x)) != EMPTY_TILE;
}

/* read the falling piece's cells as (column,row) from its sprites */
static int read_cells(CpuBot *b, int cells[4][2]) {
    for (int i = 0; i < 4; i++) {
        int sy = b->c.peek((uint16_t)(OAM_PIECE + i * 4)), sx = b->c.peek((uint16_t)(OAM_PIECE + i * 4 + 1));
        cells[i][0] = (sx - 8) / 8 - 2;
        cells[i][1] = (sy - 16) / 8;
        if (sy == 0 || sx == 0) return 0;
    }
    return 1;
}

/* learn the 4 orientations (spec id + cells) by rotating in a lookahead */
static int learn_rotations_la(CpuBot *b);
/* rotations come from the per-shape cache when this spawn shape has been seen before;
 * otherwise learned once in a lookahead (a few frames of extra work, 7 times a match) */
static int learn_rotations(CpuBot *b) {
    int spec0 = b->c.peek(PIECE_SPEC), x0 = b->c.peek(PIECE_X), y0 = b->c.peek(PIECE_Y);
    /* a cached shape can be reused when the piece sits a whole number of tiles from where it was learned */
    int usable = b->shape[spec0].valid && ((x0 - b->shape[spec0].lx) & 7) == 0 && ((y0 - b->shape[spec0].ly) & 7) == 0;
    int XS = usable ? (x0 - b->shape[spec0].lx) / 8 : 0, YS = usable ? (y0 - b->shape[spec0].ly) / 8 : 0;
#ifdef CPUBOT_VERIFY
    extern int cpubot_verify_checked, cpubot_verify_bad;
    if (usable) {
        learn_rotations_la(b);
        int bad = b->nrot != b->shape[spec0].nrot;
        for (int r = 0; r < b->nrot && !bad; r++) {
            if (b->rot[r].spec != b->shape[spec0].rot[r].spec || b->rot[r].x != x0 + b->shape[spec0].rot[r].dx) bad = 1;
            for (int i = 0; i < 4; i++)
                if (b->rot[r].cells[i][0] != b->shape[spec0].rot[r].cells[i][0] + XS ||
                    b->rot[r].cells[i][1] != b->shape[spec0].rot[r].cells[i][1] + YS) bad = 1;
        }
        cpubot_verify_checked++; cpubot_verify_bad += bad;
    }
#endif
    if (usable) {
        b->nrot = b->shape[spec0].nrot;
        for (int r = 0; r < b->nrot; r++) {
            b->rot[r].spec = b->shape[spec0].rot[r].spec;
            b->rot[r].presses = b->shape[spec0].rot[r].presses;
            b->rot[r].x = x0 + b->shape[spec0].rot[r].dx;   /* dx is relative, so any offset works */
            for (int i = 0; i < 4; i++) {
                b->rot[r].cells[i][0] = b->shape[spec0].rot[r].cells[i][0] + XS;
                b->rot[r].cells[i][1] = b->shape[spec0].rot[r].cells[i][1] + YS;
            }
        }
        return b->nrot > 0;
    }
    int ok = learn_rotations_la(b);
    b->learn_runs++;
    if (ok && !b->shape[spec0].valid) {
        b->shape[spec0].lx = x0; b->shape[spec0].ly = y0; XS = 0; YS = 0;
        b->shape[spec0].nrot = b->nrot;
        for (int r = 0; r < b->nrot; r++) {
            b->shape[spec0].rot[r].spec = b->rot[r].spec;
            b->shape[spec0].rot[r].presses = b->rot[r].presses;
            b->shape[spec0].rot[r].dx = b->rot[r].x - x0;
            for (int i = 0; i < 4; i++) {
                b->shape[spec0].rot[r].cells[i][0] = b->rot[r].cells[i][0] - XS;
                b->shape[spec0].rot[r].cells[i][1] = b->rot[r].cells[i][1] - YS;
            }
        }
        b->shape[spec0].valid = 1;
    }
    return ok;
}
static int learn_rotations_la(CpuBot *b) {
    if (!b->scratch) { b->scratch = malloc(b->c.state_size() + 64); if (!b->scratch) return 0; }
    int awp = b->c.la_begin(b->scratch);
    int n = 0;
    for (int r = 0; r < 4; r++) {
        if (r > 0) {
            int spec0 = b->c.peek(PIECE_SPEC);
            for (int tries = 0; tries < 3 && b->c.peek(PIECE_SPEC) == spec0; tries++) {
                b->c.set_buttons(0x10); b->c.run_frame();          /* A */
                b->c.set_buttons(0); b->c.run_frame();
            }
            if (b->c.peek(PIECE_SPEC) == spec0) break;               /* can't rotate (e.g. O piece) */
        }
        int spec = b->c.peek(PIECE_SPEC), seen = 0;
        for (int k = 0; k < n; k++) if (b->rot[k].spec == spec) seen = 1;
        if (seen) break;
        if (!read_cells(b, b->rot[n].cells)) break;
        b->rot[n].spec = spec;
        b->rot[n].x = b->c.peek(PIECE_X);
        b->rot[n].presses = r;
        n++;
    }
    b->c.la_end(b->scratch, awp);
    b->nrot = n;
    return n > 0;
}

static float evaluate(uint8_t g[18][10], int lines, int level) {
    int heights[10], holes = 0, agg = 0, bump = 0, maxh = 0;
    for (int x = 0; x < 10; x++) {
        int top = 18;
        for (int y = 0; y < 18; y++) if (g[y][x]) { top = y; break; }
        heights[x] = 18 - top;
        agg += heights[x];
        if (heights[x] > maxh) maxh = heights[x];
        for (int y = top + 1; y < 18; y++) if (!g[y][x]) holes++;
    }
    for (int x = 0; x < 9; x++) bump += abs(heights[x] - heights[x + 1]);
    float lv = (float)lines;
    if (level >= 2) lv = (float)(lines * lines);   /* HARD values multi-line clears (they send garbage) */
    return -0.51f * (float)agg - 0.76f * (float)holes - 0.18f * (float)bump + 0.76f * lv
           - (maxh > 13 ? 4.0f * (float)(maxh - 13) : 0.0f);
}

typedef struct { float score; int rot, dx; } Cand;

static void choose(CpuBot *b) {
    uint8_t g[18][10];
    read_board(b, g);
    Cand best[6]; int nb = 0;
    for (int r = 0; r < b->nrot; r++) {
        int (*c)[2] = b->rot[r].cells;
        for (int dx = -9; dx <= 9; dx++) {
            int ok = 1;
            for (int i = 0; i < 4; i++) { int x = c[i][0] + dx; if (x < 0 || x > 9) ok = 0; }
            if (!ok) continue;
            /* the sideways path at the current height must be clear */
            int step = dx > 0 ? 1 : -1;
            for (int s = 0; s != dx && ok; s += step)
                for (int i = 0; i < 4; i++) {
                    int x = c[i][0] + s + step, y = c[i][1];
                    if (y >= 0 && y < 18 && g[y][x]) ok = 0;
                }
            if (!ok) continue;
            int drop = 0;
            for (;;) {
                int fits = 1;
                for (int i = 0; i < 4; i++) {
                    int x = c[i][0] + dx, y = c[i][1] + drop + 1;
                    if (y >= 18 || (y >= 0 && g[y][x])) fits = 0;
                }
                if (!fits) break;
                drop++;
            }
            uint8_t t[18][10]; memcpy(t, g, sizeof t);
            int over = 0;
            for (int i = 0; i < 4; i++) {
                int x = c[i][0] + dx, y = c[i][1] + drop;
                if (y < 0) over = 1; else t[y][x] = 1;
            }
            int lines = 0;
            for (int y = 17; y >= 0; y--) {
                int full = 1;
                for (int x = 0; x < 10; x++) if (!t[y][x]) full = 0;
                if (full) {
                    lines++;
                    memmove(t[1], t[0], (size_t)y * 10);
                    memset(t[0], 0, 10);
                    y++;
                }
            }
            float sc = over ? -1e6f : evaluate(t, lines, b->level);
            /* keep the top few */
            Cand cd = { sc, r, dx };
            int pos = nb;
            while (pos > 0 && best[pos - 1].score < sc) pos--;
            if (pos < 6) {
                int end = nb < 6 ? nb : 5;
                for (int k = end; k > pos; k--) best[k] = best[k - 1];
                best[pos] = cd;
                if (nb < 6) nb++;
            }
        }
    }
    if (!nb) { b->target_spec = -1; return; }
    int pick = 0;
    if (b->level == 0) {                 /* EASY: often settles for a worse spot */
        uint32_t r = rnd(b) % 100;
        int lim = nb < 4 ? nb : 4;
        if (r < 30 && lim > 1) pick = 1 + (int)(rnd(b) % (uint32_t)(lim - 1));
    } else if (b->level == 1) {          /* NORMAL: occasionally a little sloppy */
        if (rnd(b) % 100 < 10 && nb > 1) pick = 1;
    }
    b->target_spec = b->rot[best[pick].rot].spec;
    b->target_x = b->rot[best[pick].rot].x + 8 * best[pick].dx;
}

void cpubot_init(CpuBot *b, CpuBotCore core, int level, uint32_t seed) {
    void *keep = b->scratch;
    memset(b, 0, sizeof *b);
    b->scratch = keep;
    b->c = core; b->level = level; b->seed = seed ? seed : 1;
    b->need_plan = 1;
}

/* buttons for the bot's Game Boy for its next frame (call at frame boundaries) */
uint8_t cpubot_frame(CpuBot *b) {
    int st = b->c.peek(GS);
    int fall = b->c.peek(FALL);
    if (b->prev_fall && !fall) b->need_plan = 1;          /* previous piece finished locking */
    b->prev_fall = fall;
    if (st != 0x1A || b->c.peek(PAUSED)) { b->phase = B_IDLE; b->need_plan = 1; return 0; }

    /* human-ish pacing per difficulty */
    static const int think[3] = { 28, 14, 6 }, pace[3] = { 9, 5, 3 };

    switch (b->phase) {
    case B_IDLE:
        if (b->need_plan && fall == 0 && b->c.peek(SHIFTING) == 0 && b->c.peek(PIECE_Y) <= 0x28) {
            b->need_plan = 0;
            if (learn_rotations(b)) { choose(b); b->phase = b->target_spec >= 0 ? B_THINK : B_IDLE; }
            b->timer = think[b->level] + (int)(rnd(b) % 6);
            b->tries = 0;
        }
        return 0;
    case B_THINK:
        if (--b->timer > 0) return 0;
        b->phase = B_STEER; b->timer = 0;
        /* fall through */
    case B_STEER: {
        if (fall != 0) { b->phase = B_IDLE; return 0; }    /* it landed on its own */
        if (b->timer > 0) { b->timer--; return 0; }
        b->timer = pace[b->level];
        int spec = b->c.peek(PIECE_SPEC), x = b->c.peek(PIECE_X);
        if (++b->tries > 40) { b->phase = B_DROP; b->timer = 0; return 0; }
        if (spec != b->target_spec) return 0x10;          /* A: rotate */
        if (x < b->target_x) return 0x01;                 /* RIGHT */
        if (x > b->target_x) return 0x02;                 /* LEFT */
        b->phase = B_DROP; b->timer = 0;
        return 0;
    }
    case B_DROP:
        if (fall != 0) { b->phase = B_IDLE; return 0; }      /* landed while dawdling */
        if (b->level == 2) {
            if (b->c.hard_drop()) b->hold = 4;
            b->phase = B_WAIT_LOCK;
            return 0x08;
        }
        if (b->level == 0 && b->timer == 0) { b->timer = 20 + (int)(rnd(b) % 40); }   /* EASY: dawdles before dropping */
        if (b->level == 0 && --b->timer > 0) return 0;
        b->phase = B_WAIT_LOCK; b->hold = 400; return 0x08;
    case B_WAIT_LOCK:
        /* hold DOWN until this piece locks, then let go so the next one needs a fresh press */
        if (fall != 0 || b->c.landed()) { b->hold = 0; b->phase = B_IDLE; return 0; }
        if (b->hold > 0) { b->hold--; return 0x08; }
        b->phase = B_IDLE;
        return 0;
    }
    return 0;
}
