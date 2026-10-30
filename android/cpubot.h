#ifndef CPUBOT_H
#define CPUBOT_H
#include <stdint.h>

/* the Game Boy the bot plays (any core instance) */
typedef struct {
    uint8_t (*peek)(uint16_t);
    void (*set_buttons)(uint8_t);
    void (*run_frame)(void);
    int (*la_begin)(void *);
    void (*la_end)(const void *, int);
    unsigned (*state_size)(void);
    int (*hard_drop)(void);
    int (*landed)(void);
} CpuBotCore;

typedef struct {
    CpuBotCore c;
    int level;                 /* 0 easy, 1 normal, 2 hard */
    uint32_t seed;
    void *scratch;
    int need_plan, prev_fall, phase, timer, tries, hold;
    int nrot;
    struct { int spec, x, presses; int cells[4][2]; } rot[4];
    int target_spec, target_x;
    /* what each piece's rotations look like, learned once per spawn shape:
     * x and cells are relative to the piece position it was learned at */
    struct { int valid, nrot, lx, ly; struct { int spec, dx, presses; int cells[4][2]; } rot[4]; } shape[256];
    int learn_runs;            /* stats: lookaheads spent learning */
} CpuBot;

void cpubot_init(CpuBot *b, CpuBotCore core, int level, uint32_t seed);
uint8_t cpubot_frame(CpuBot *b);
#endif
