/* Two complete Game Boys (two independent copies of the recompiled core)
 * joined by the virtual link cable over a simulated network with latency,
 * jitter and packet loss. Driven from Python (test/linkbot.py). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../core/gbcore.h"
#include "../android/link.h"

/* the second core's symbols are renamed with objcopy */
#define DECL_B(ret, name, args) ret b_##name args;
DECL_B(int, gb_init, (const uint8_t *, unsigned))
DECL_B(int, gb_run_cycles, (unsigned))
DECL_B(void, gb_run_frame, (void))
DECL_B(void, gb_set_buttons, (uint8_t))
DECL_B(const uint8_t *, gb_framebuffer, (void))
DECL_B(uint8_t, gb_peek, (uint16_t))
DECL_B(void, gb_poke, (uint16_t, uint8_t))
DECL_B(int, gb_audio_read, (int16_t *, int))
DECL_B(void, gb_link_attach, (gb_link_start_fn, void *))
DECL_B(int, gb_link_master_waiting, (void))
DECL_B(void, gb_link_master_complete, (uint8_t))
DECL_B(int, gb_link_passive_ready, (void))
DECL_B(uint8_t, gb_link_passive_exchange, (uint8_t))
DECL_B(int, gb_lookahead_begin, (void *))
DECL_B(void, gb_lookahead_end, (const void *, int))
DECL_B(unsigned, gb_state_size, (void))
DECL_B(int, tetris_hard_drop, (void))
DECL_B(int, tetris_piece_landed, (void))
DECL_B(uint64_t, gb_cycles, (void))
DECL_B(int, tetris_link_hold_while_waiting, (void))
DECL_B(int, gb_link_master_waiting, (void))

typedef struct {
    int (*run_cycles)(unsigned); void (*run_frame)(void); void (*set_buttons)(uint8_t);
    const uint8_t *(*fb)(void); uint8_t (*peek)(uint16_t); void (*poke)(uint16_t, uint8_t);
    int (*audio_read)(int16_t *, int); void (*link_attach)(gb_link_start_fn, void *);
    int (*la_begin)(void *); void (*la_end)(const void *, int);
    int (*hard_drop)(void); int (*landed)(void); uint64_t (*cycles)(void);
    int (*hold)(void); int (*mwait)(void);
} Core;

static const Core coreA = { gb_run_cycles, gb_run_frame, gb_set_buttons, gb_framebuffer, gb_peek, gb_poke,
    gb_audio_read, gb_link_attach, gb_lookahead_begin, gb_lookahead_end, tetris_hard_drop, tetris_piece_landed, gb_cycles, tetris_link_hold_while_waiting, gb_link_master_waiting };
static const Core coreB = { b_gb_run_cycles, b_gb_run_frame, b_gb_set_buttons, b_gb_framebuffer, b_gb_peek, b_gb_poke,
    b_gb_audio_read, b_gb_link_attach, b_gb_lookahead_begin, b_gb_lookahead_end, b_tetris_hard_drop, b_tetris_piece_landed, b_gb_cycles, b_tetris_link_hold_while_waiting, b_gb_link_master_waiting };

/* ---- simulated network ---- */
typedef struct { int64_t at; int to; int n; uint8_t d[16]; } Pkt;
#define NQ 4096
static Pkt q[NQ]; static int qn;
static int64_t lat_ns, jit_ns; static double loss; static unsigned rng = 12345;
static unsigned long sent_pkts, lost_pkts;
static double frand(void) { rng = rng * 1103515245u + 12345u; return (double)((rng >> 8) & 0xFFFFFF) / 16777216.0; }

typedef struct { int id; Link link; const Core *c; uint64_t frames; int frame_flag; } Side;
static Side side[2];
static int64_t now;
static int link_enabled = 1;

static void net_send(void *ctx, const uint8_t *p, int n) {
    Side *s = ctx;
    sent_pkts++;
    if (frand() < loss) { lost_pkts++; return; }
    if (qn >= NQ || n > 16) return;
    Pkt *k = &q[qn++];
    k->at = now + lat_ns + (int64_t)(frand() * (double)jit_ns);
    k->to = 1 - s->id; k->n = n; memcpy(k->d, p, (size_t)n);
}

static void on_master_start(uint8_t b, uint16_t pc, void *user) {
    Side *s = user;
    link_master_start(&s->link, b, tetris_link_fire_and_forget(pc), now);
}

static void deliver(int to) {
    for (int i = 0; i < qn;) {
        if (q[i].to == to && q[i].at <= now) {
            link_on_packet(&side[to].link, q[i].d, q[i].n, now);
            q[i] = q[--qn];
        } else i++;
    }
}

/* ---- API ---- */
void sim_init(const uint8_t *rom, int lat_us, int jit_us, double loss_p, unsigned seed) {
    gb_init(rom, 0x8000); b_gb_init(rom, 0x8000);
    lat_ns = (int64_t)lat_us * 1000; jit_ns = (int64_t)jit_us * 1000; loss = loss_p; rng = seed;
    qn = 0; now = 0; sent_pkts = lost_pkts = 0;
    LinkCore la = { gb_link_master_waiting, gb_link_master_complete, gb_link_passive_ready, gb_link_passive_exchange, gb_cycles };
    LinkCore lb = { b_gb_link_master_waiting, b_gb_link_master_complete, b_gb_link_passive_ready, b_gb_link_passive_exchange, b_gb_cycles };
    for (int i = 0; i < 2; i++) { side[i].id = i; side[i].c = i ? &coreB : &coreA; side[i].frames = 0; }
    link_init(&side[0].link, la, net_send, &side[0], 0);
    link_init(&side[1].link, lb, net_send, &side[1], 0);
    gb_link_attach(on_master_start, &side[0]);
    b_gb_link_attach(on_master_start, &side[1]);
}

/* advance both Game Boys by one frame of real time (16 slices); returns
 * bit0/bit1 = side A/B completed a frame during this call */
#define SLICES 16
int sim_step(uint8_t ba, uint8_t bb) {
    int done = 0;
    const int64_t frame_ns = 16742706LL;
    const unsigned slice_cyc = 70224 / SLICES;
    side[0].c->set_buttons(ba); side[1].c->set_buttons(bb);
    for (int s = 0; s < SLICES; s++) {
        now += frame_ns / SLICES;
        for (int i = 0; i < 2; i++) {
            deliver(i);
            link_poll(&side[i].link, now);
            if (side[i].c->mwait() && side[i].c->hold()) { link_poll(&side[i].link, now); continue; }   /* cable busy */
            uint64_t target = side[i].c->cycles() + slice_cyc;
            while (side[i].c->cycles() < target) {
                if (side[i].c->mwait() && side[i].c->hold()) break;
                if (side[i].c->run_cycles((unsigned)(target - side[i].c->cycles()))) { side[i].frames++; done |= 1 << i; }
            }
            link_poll(&side[i].link, now);
        }
    }
    int16_t tmp[4096 * 2];
    while (gb_audio_read(tmp, 4096) > 0) {}
    while (b_gb_audio_read(tmp, 4096) > 0) {}
    return done;
}

uint8_t sim_peek(int i, uint16_t a) { return side[i].c->peek(a); }
void sim_poke(int i, uint16_t a, uint8_t v) { side[i].c->poke(a, v); }
const uint8_t *sim_fb(int i) { return side[i].c->fb(); }
int sim_hard_drop(int i) { return side[i].c->hard_drop(); }
int sim_landed(int i) { return side[i].c->landed(); }
/* isolated lookahead for bots (cable unplugged, state restored after) */
static uint8_t *la_buf[2]; static int la_awp[2];
void sim_la_begin(int i) { if (!la_buf[i]) la_buf[i] = malloc(gb_state_size() + 64); la_awp[i] = side[i].c->la_begin(la_buf[i]); }
void sim_la_frame(int i, uint8_t b) { side[i].c->set_buttons(b); side[i].c->run_frame(); int16_t t[8192]; side[i].c->audio_read(t, 4096); }
void sim_la_end(int i) { side[i].c->la_end(la_buf[i], la_awp[i]); }
void sim_stats(int i, unsigned *out) {
    Link *l = &side[i].link;
    out[0] = l->xfers_out; out[1] = l->xfers_in; out[2] = l->retransmits; out[3] = l->dups;
    out[8] = l->garb_sent; out[9] = l->garb_recv; out[4] = (unsigned)sent_pkts; out[5] = (unsigned)lost_pkts; out[6] = (unsigned)side[i].frames; out[7] = (unsigned)qn;
}
void sim_debug(void) {
    for (int i = 0; i < 2; i++) {
        Link *l = &side[i].link;
        int outn = 0; for (int k = 0; k < LINK_WIN; k++) outn += l->out[k].active;
        fprintf(stderr, "side %c: gs=%02x SB=%02x SC=%02x IE=%02x IF=%02x handled=%02x read=%02x role=%02x mw=%d | waiting=%d wait_seq=%u tx_seq=%u rx_next=%u head_have=%d outstanding=%d\n",
            'A' + i, side[i].c->peek(0xFFE1), side[i].c->peek(0xFF01), side[i].c->peek(0xFF02), side[i].c->peek(0xFFFF), side[i].c->peek(0xFF0F),
            side[i].c->peek(0xFFCC), side[i].c->peek(0xFFD0), side[i].c->peek(0xFFCB), i ? b_gb_link_master_waiting() : gb_link_master_waiting(),
            l->waiting, l->wait_seq, l->tx_seq, l->rx_next, l->rx_have[l->rx_next & 255], outn);
    }
}

unsigned sim_frames(int i) { return (unsigned)side[i].frames; }

/* ---- CPU bot (android/cpubot.c) driving either side ---- */
#include "../android/cpubot.h"
DECL_B(unsigned, gb_state_size, (void))
static uint8_t a_peek(uint16_t x) { return gb_peek(x); }
static CpuBot bots[2];
void sim_cpubot_init(int i, int level, unsigned seed) {
    CpuBotCore ca = { gb_peek, gb_set_buttons, gb_run_frame, gb_lookahead_begin, gb_lookahead_end, gb_state_size, tetris_hard_drop, tetris_piece_landed };
    CpuBotCore cb = { b_gb_peek, b_gb_set_buttons, b_gb_run_frame, b_gb_lookahead_begin, b_gb_lookahead_end, b_gb_state_size, b_tetris_hard_drop, b_tetris_piece_landed };
    (void)a_peek;
    cpubot_init(&bots[i], i ? cb : ca, level, seed);
}
/* step both Game Boys one frame of time, with bots choosing buttons at their own frame boundaries */
static uint8_t bot_btn[2];
int sim_step_bots(int use_a, int use_b, uint8_t ba, uint8_t bb) {
    const int64_t frame_ns = 16742706LL;
    const unsigned slice_cyc = 70224 / SLICES;
    int done = 0;
    if (!use_a) bot_btn[0] = ba;
    if (!use_b) bot_btn[1] = bb;
    side[0].c->set_buttons(bot_btn[0]); side[1].c->set_buttons(bot_btn[1]);
    for (int s = 0; s < SLICES; s++) {
        now += frame_ns / SLICES;
        for (int i = 0; i < 2; i++) {
            deliver(i);
            link_poll(&side[i].link, now);
            if (side[i].c->mwait() && side[i].c->hold()) { link_poll(&side[i].link, now); continue; }
            uint64_t target = side[i].c->cycles() + slice_cyc;
            while (side[i].c->cycles() < target) {
                if (side[i].c->mwait() && side[i].c->hold()) break;
                if (side[i].c->run_cycles((unsigned)(target - side[i].c->cycles()))) {
                    side[i].frames++; done |= 1 << i;
                    if ((i == 0 && use_a) || (i == 1 && use_b)) { bot_btn[i] = cpubot_frame(&bots[i]); side[i].c->set_buttons(bot_btn[i]); }
                }
            }
            link_poll(&side[i].link, now);
        }
    }
    int16_t tmp[4096 * 2];
    while (gb_audio_read(tmp, 4096) > 0) {}
    while (b_gb_audio_read(tmp, 4096) > 0) {}
    return done;
}
#ifdef CPUBOT_VERIFY
int cpubot_verify_checked, cpubot_verify_bad;
void sim_verify_stats(int *out) { out[0] = cpubot_verify_checked; out[1] = cpubot_verify_bad; out[2] = bots[0].learn_runs; out[3] = bots[1].learn_runs; }
#endif
