/* Hardware side of the port: memory map, LCD (PPU), sound (APU), timer,
 * serial and joypad. The CPU itself is the generated cpu_rec.c. */
#include <string.h>
#include "cpu_macros.h"

CpuRegs cpu;
u64 cyc, evt;
int ime, ei_delay, halted, frame_done;
u16 halt_next;
u8 rom[0x8000], wram[0x2000], vram[0x2000], oam[0xA0], io[0x100];
int force_interp;
static int la_fast;              /* inside a lookahead (or headless): skip drawing and sound synthesis */
static int headless;             /* never shown or heard (the CPU opponent's Game Boy) */
unsigned long long gb_interp_count;

static u8 buttons;          /* active-high BTN_* mask */
static u8 fb_work[GB_H][GB_W], fb_out[GB_H * GB_W];

#ifdef GB_INTERP_ONLY
/* no game code is built in: every instruction is read from the user's ROM at run time */
#define STEP() gb_interp_one()
#else
#define STEP() do { blockfn f_ = (!force_interp && cpu.pc < 0x8000) ? gb_block_table[cpu.pc] : 0; \
                    if (f_) f_(); else gb_interp_one(); } while (0)
#endif

/* ---------------- timer / divider ---------------- */
static u64 div_base;        /* DIV counter = (cyc - div_base) */
static u64 tima_last;       /* cycle up to which TIMA has been advanced */
static const int tac_bit[4] = {9, 3, 5, 7};

static void timer_catchup(void) {
    if (!(io[0x07] & 4)) { tima_last = cyc; return; }
    int sh = tac_bit[io[0x07] & 3] + 1;
    u64 a = (tima_last - div_base) >> sh, b = (cyc - div_base) >> sh;
    tima_last = cyc;
    while (a < b) {
        a++;
        if (++io[0x05] == 0) { io[0x05] = io[0x06]; IFREG |= 4; }
    }
}
static u64 timer_next(void) {
    if (!(io[0x07] & 4)) return ~0ULL;
    int sh = tac_bit[io[0x07] & 3] + 1;
    u64 per = 1ULL << sh;
    u64 now = cyc - div_base;
    u64 left = (u64)(256 - io[0x05]) * per - (now & (per - 1));
    return cyc + left;
}

/* ---------------- serial / link cable ----------------
 * No cable: a master transfer completes after 8 bit-times receiving $FF.
 * With a cable attached (gb_link_attach), starting a master transfer calls
 * the link callback; the transfer completes when gb_link_master_complete()
 * delivers the other side's byte (never sooner than real hardware would). */
static u64 serial_done = ~0ULL;
static u64 serial_start;
static u8 serial_in = 0xFF;
static int master_waiting;
static gb_link_start_fn link_fn;
static void *link_user;
u16 serial_pc;            /* address of the instruction that last wrote SC (set by generated code) */
static u64 stop_cyc = ~0ULL;

/* ---------------- PPU ---------------- */
static int ly, mode, win_line, stat_line;
static u64 ppu_next;

static void render_line(int y) {
    u8 lcdc = io[0x40];
    u8 *row = fb_work[y];
    u8 bgcol[GB_W];
    u8 bgp = io[0x47];
    if (lcdc & 1) {
        int scy = io[0x42], scx = io[0x43];
        int wy = io[0x4A], wx = io[0x4B] - 7;
        int use_win = (lcdc & 0x20) && y >= wy && io[0x4B] <= 166;
        u16 bgmap = (lcdc & 0x08) ? 0x1C00 : 0x1800;
        u16 winmap = (lcdc & 0x40) ? 0x1C00 : 0x1800;
        int unsigned_tiles = lcdc & 0x10;
        int drew_win = 0;
        for (int x = 0; x < GB_W; x++) {
            int px, py; u16 map;
            if (use_win && x >= wx) { px = x - wx; py = win_line; map = winmap; drew_win = 1; }
            else { px = (x + scx) & 255; py = (y + scy) & 255; map = bgmap; }
            u8 tile = vram[map + (py >> 3) * 32 + (px >> 3)];
            u16 base = unsigned_tiles ? (u16)(tile * 16) : (u16)(0x1000 + (s8)tile * 16);
            u8 lo = vram[base + (py & 7) * 2], hi = vram[base + (py & 7) * 2 + 1];
            int bit = 7 - (px & 7);
            u8 c = (u8)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
            bgcol[x] = c;
            row[x] = (bgp >> (c * 2)) & 3;
        }
        if (drew_win) win_line++;
    } else {
        memset(bgcol, 0, sizeof bgcol);
        memset(row, 0, GB_W);
    }
    if (lcdc & 2) {
        int h = (lcdc & 4) ? 16 : 8;
        int idx[10], n = 0;
        for (int i = 0; i < 40 && n < 10; i++) {
            int sy = oam[i * 4] - 16;
            if (y >= sy && y < sy + h) idx[n++] = i;
        }
        /* DMG priority: smaller X wins, ties -> lower OAM index. Draw lowest priority first. */
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                int xi = oam[idx[i] * 4 + 1], xj = oam[idx[j] * 4 + 1];
                if (xj < xi || (xj == xi && idx[j] < idx[i])) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
            }
        for (int k = n - 1; k >= 0; k--) {
            u8 *s = &oam[idx[k] * 4];
            int sy = s[0] - 16, sx = s[1] - 8;
            u8 tile = s[2], fl = s[3];
            int line = y - sy;
            if (fl & 0x40) line = h - 1 - line;
            if (h == 16) tile &= 0xFE;
            u16 base = (u16)(tile * 16 + line * 2);
            u8 lo = vram[base], hi = vram[base + 1];
            u8 pal = (fl & 0x10) ? io[0x49] : io[0x48];
            for (int p = 0; p < 8; p++) {
                int x = sx + p;
                if (x < 0 || x >= GB_W) continue;
                int bit = (fl & 0x20) ? p : 7 - p;
                u8 c = (u8)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
                if (!c) continue;
                if ((fl & 0x80) && bgcol[x]) continue;
                row[x] = (pal >> (c * 2)) & 3;
            }
        }
    }
}

static void stat_update(void) {
    u8 st = io[0x41];
    int coinc = (ly == io[0x45]);
    int line = ((st & 0x40) && coinc) || ((st & 0x20) && mode == 2) ||
               ((st & 0x10) && mode == 1) || ((st & 0x08) && mode == 0);
    if (!(io[0x40] & 0x80)) line = 0;
    if (line && !stat_line) IFREG |= 2;
    stat_line = line;
}

static void present_frame(int blank) {
    if (blank) memset(fb_out, 0, sizeof fb_out);
    else memcpy(fb_out, fb_work, sizeof fb_out);
    frame_done = 1;
}

static void ppu_catchup(void) {
    while (cyc >= ppu_next) {
        if (!(io[0x40] & 0x80)) {           /* LCD off: keep frame pacing */
            present_frame(1);
            ppu_next += 70224;
            continue;
        }
        switch (mode) {
        case 2: mode = 3; ppu_next += 172; break;
        case 3: if (!la_fast) render_line(ly); mode = 0; ppu_next += 204; break;
        case 0:
            ly++;
            if (ly == 144) { mode = 1; IFREG |= 1; present_frame(0); ppu_next += 456; }
            else { mode = 2; ppu_next += 80; }
            break;
        case 1:
            ly++;
            if (ly == 154) { ly = 0; win_line = 0; mode = 2; ppu_next += 80; }
            else ppu_next += 456;
            break;
        }
        stat_update();
    }
}

/* ---------------- APU ---------------- */
typedef struct {
    int on, dac, length, len_en;
    int vol, env_init, env_add, env_per, env_timer;
    int freq, timer, pos, duty;
    int sw_per, sw_neg, sw_shift, sw_timer, sw_en, shadow;
    int wvol;
    int lfsr, nshift, width7, ndiv;
} Ch;
static Ch ch[4];
static int apu_on;
static u64 apu_cyc, fs_next;
static int fs_step;
static u64 samp_fp, samp_step_fp;   /* 16.16 fixed point cycles */
static int acc[4]; static int acc_n;
static float hp_l, hp_r, hp_charge;

#define ABUF 16384
static int16_t abuf[ABUF * 2];
static int arp, awp;

static const u8 duty_tab[4] = {0x01, 0x81, 0x87, 0x7E};

static int sq_period(Ch *c) { return (2048 - c->freq) * 4; }
static int wv_period(Ch *c) { return (2048 - c->freq) * 2; }
static int nz_period(Ch *c) { static const int d[8] = {8, 16, 32, 48, 64, 80, 96, 112}; return d[c->ndiv] << c->nshift; }

static int ch_level(int i) {
    Ch *c = &ch[i];
    if (!c->on || !c->dac) return 0;
    switch (i) {
    case 0: case 1: return ((duty_tab[c->duty] >> (7 - c->pos)) & 1) ? c->vol : 0;
    case 2: {
        u8 b = io[0x30 + (c->pos >> 1)];
        int s = (c->pos & 1) ? (b & 0xF) : (b >> 4);
        static const int sh[4] = {4, 0, 1, 2};
        return s >> sh[c->wvol];
    }
    default: return (~c->lfsr & 1) ? c->vol : 0;
    }
}

/* advance channel i by n cycles, adding level*cycles to acc */
static void ch_run(int i, int n) {
    Ch *c = &ch[i];
    if (!c->on) { return; }
    int lv = ch_level(i);
    if (i == 3 && c->nshift >= 14) { acc[i] += lv * n; return; }
    while (n > 0) {
        int s = c->timer < n ? c->timer : n;
        acc[i] += lv * s;
        c->timer -= s; n -= s;
        if (c->timer <= 0) {
            if (i < 2) { c->timer += sq_period(c); c->pos = (c->pos + 1) & 7; }
            else if (i == 2) { c->timer += wv_period(c); c->pos = (c->pos + 1) & 31; }
            else {
                c->timer += nz_period(c);
                int x = (c->lfsr ^ (c->lfsr >> 1)) & 1;
                c->lfsr = (c->lfsr >> 1) | (x << 14);
                if (c->width7) c->lfsr = (c->lfsr & ~0x40) | (x << 6);
            }
            lv = ch_level(i);
        }
    }
}

static int sweep_calc(Ch *c) {
    int d = c->shadow >> c->sw_shift;
    int nf = c->sw_neg ? c->shadow - d : c->shadow + d;
    if (nf > 2047) c->on = 0;
    return nf;
}

static void fs_clock(void) {
    int s = fs_step;
    fs_step = (fs_step + 1) & 7;
    if (!(s & 1)) {               /* length: steps 0,2,4,6 */
        for (int i = 0; i < 4; i++) {
            Ch *c = &ch[i];
            if (c->len_en && c->length > 0 && --c->length == 0) c->on = 0;
        }
    }
    if (s == 2 || s == 6) {       /* sweep */
        Ch *c = &ch[0];
        if (--c->sw_timer <= 0) {
            c->sw_timer = c->sw_per ? c->sw_per : 8;
            if (c->sw_en && c->sw_per) {
                int nf = sweep_calc(c);
                if (nf <= 2047 && c->sw_shift) { c->freq = c->shadow = nf; sweep_calc(c); }
            }
        }
    }
    if (s == 7) {                 /* envelope */
        for (int i = 0; i < 4; i++) {
            if (i == 2) continue;
            Ch *c = &ch[i];
            if (!c->env_per) continue;
            if (--c->env_timer <= 0) {
                c->env_timer = c->env_per;
                if (c->env_add && c->vol < 15) c->vol++;
                else if (!c->env_add && c->vol > 0) c->vol--;
            }
        }
    }
}

static void emit_sample(void) {
    float l = 0, r = 0;
    u8 pan = io[0x25];
    for (int i = 0; i < 4; i++) {
        float v = 0;
        if (ch[i].dac) v = ((float)acc[i] / (float)acc_n) - 7.5f;  /* DAC: 0..15 -> centred */
        if (pan & (0x10 << i)) l += v;
        if (pan & (0x01 << i)) r += v;
        acc[i] = 0;
    }
    acc_n = 0;
    l *= (float)(((io[0x24] >> 4) & 7) + 1);
    r *= (float)((io[0x24] & 7) + 1);
    if (!apu_on) { l = r = 0; }
    /* high-pass (DMG output capacitor) */
    float ol = l - hp_l; hp_l = l - ol * hp_charge;
    float or_ = r - hp_r; hp_r = r - or_ * hp_charge;
    int sl = (int)(ol * 56.0f), sr = (int)(or_ * 56.0f);
    if (sl > 32767) sl = 32767; if (sl < -32768) sl = -32768;
    if (sr > 32767) sr = 32767; if (sr < -32768) sr = -32768;
    int nw = (awp + 1) % ABUF;
    if (nw == arp) arp = (arp + 1) % ABUF;   /* overrun: drop oldest */
    abuf[awp * 2] = (int16_t)sl; abuf[awp * 2 + 1] = (int16_t)sr;
    awp = nw;
}

static void apu_catchup(void) {
    if (la_fast) return;          /* lookahead: sound is thrown away anyway */
    while (apu_cyc < cyc) {
        u64 samp_at = samp_fp >> 16;
        u64 to = cyc;
        if (fs_next < to) to = fs_next;
        if (samp_at < to) to = samp_at;
        int n = (int)(to - apu_cyc);
        if (n > 0) {
            for (int i = 0; i < 4; i++) {
                if (ch[i].on) ch_run(i, n);
                else if (ch[i].dac) acc[i] += 0;  /* silent but DAC on */
            }
            acc_n += n;
            apu_cyc = to;
        }
        if (apu_cyc >= fs_next) { fs_next += 8192; if (apu_on) fs_clock(); }
        if (apu_cyc >= (samp_fp >> 16)) {
            if (acc_n == 0) acc_n = 1;
            emit_sample();
            samp_fp += samp_step_fp;
        }
    }
}

static void ch_trigger(int i) {
    Ch *c = &ch[i];
    c->on = c->dac;
    if (c->length == 0) c->length = (i == 2) ? 256 : 64;
    if (i < 2) c->timer = sq_period(c);
    else if (i == 2) { c->timer = wv_period(c) + 6; c->pos = 0; }
    else { c->timer = nz_period(c); c->lfsr = 0x7FFF; }
    if (i != 2) { c->vol = c->env_init; c->env_timer = c->env_per ? c->env_per : 8; }
    if (i == 0) {
        c->shadow = c->freq;
        c->sw_timer = c->sw_per ? c->sw_per : 8;
        c->sw_en = c->sw_per || c->sw_shift;
        if (c->sw_shift) sweep_calc(c);
    }
}

static void apu_write(u8 r, u8 v) {
    apu_catchup();
    if (r >= 0x30 && r < 0x40) { io[r] = v; return; }
    if (r == 0x26) {
        int on = v & 0x80;
        if (!on && apu_on) {
            for (int a = 0x10; a < 0x26; a++) io[a] = 0;
            memset(ch, 0, sizeof ch);
        }
        if (on && !apu_on) { fs_step = 0; }
        apu_on = on;
        return;
    }
    if (!apu_on) return;
    io[r] = v;
    int i; Ch *c;
    switch (r) {
    case 0x10: c = &ch[0]; c->sw_per = (v >> 4) & 7; c->sw_neg = (v >> 3) & 1; c->sw_shift = v & 7; break;
    case 0x11: case 0x16:
        c = &ch[r == 0x11 ? 0 : 1]; c->duty = v >> 6; c->length = 64 - (v & 63); break;
    case 0x12: case 0x17: case 0x21:
        i = r == 0x12 ? 0 : r == 0x17 ? 1 : 3; c = &ch[i];
        c->env_init = v >> 4; c->env_add = (v >> 3) & 1; c->env_per = v & 7;
        c->dac = (v & 0xF8) != 0; if (!c->dac) c->on = 0; break;
    case 0x13: case 0x18: case 0x1D:
        i = r == 0x13 ? 0 : r == 0x18 ? 1 : 2; c = &ch[i];
        c->freq = (c->freq & 0x700) | v; break;
    case 0x14: case 0x19: case 0x1E: case 0x23:
        i = r == 0x14 ? 0 : r == 0x19 ? 1 : r == 0x1E ? 2 : 3; c = &ch[i];
        if (i < 3) c->freq = (c->freq & 0xFF) | ((v & 7) << 8);
        c->len_en = (v >> 6) & 1;
        if (v & 0x80) ch_trigger(i);
        break;
    case 0x1A: c = &ch[2]; c->dac = (v & 0x80) != 0; if (!c->dac) c->on = 0; break;
    case 0x1B: ch[2].length = 256 - v; break;
    case 0x1C: ch[2].wvol = (v >> 5) & 3; break;
    case 0x20: ch[3].length = 64 - (v & 63); break;
    case 0x22: c = &ch[3]; c->nshift = v >> 4; c->width7 = (v >> 3) & 1; c->ndiv = v & 7; break;
    }
}

static u8 apu_read(u8 r) {
    static const u8 mask[0x20] = {
        0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
        0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (r >= 0x30) return io[r];
    apu_catchup();
    if (r == 0x26) {
        u8 s = 0x70 | (apu_on ? 0x80 : 0);
        for (int i = 0; i < 4; i++) if (ch[i].on) s |= (u8)(1 << i);
        return s;
    }
    return io[r] | mask[r - 0x10];
}

/* ---------------- event scheduling ---------------- */
static void recompute_evt(void) {
    u64 e = ppu_next;
    if (serial_done < e) e = serial_done;
    if (stop_cyc < e) e = stop_cyc;
    u64 t = timer_next();
    if (t < e) e = t;
    evt = e;
}

void sync_events(void) {
    ppu_catchup();
    timer_catchup();
    if (cyc >= serial_done) {
        serial_done = ~0ULL;
        io[0x01] = serial_in; io[0x02] &= 0x7F; IFREG |= 8;
        serial_in = 0xFF;
    }
    if (frame_done) apu_catchup();
    recompute_evt();
}

/* ---------------- memory slow paths ---------------- */
u8 gb_rd_slow(u16 a) {
    if (a < 0xA000) return vram[a - 0x8000];
    if (a < 0xC000) return 0xFF;                       /* no cartridge RAM */
    if (a < 0xFE00) return wram[(a - 0xC000) & 0x1FFF];  /* echo */
    if (a < 0xFEA0) return oam[a - 0xFE00];
    if (a < 0xFF00) return 0x00;
    u8 r = a & 0xFF;
    switch (r) {
    case 0x00: {
        u8 p = io[0x00] | 0xCF;
        if (!(p & 0x10)) p &= (u8)~(buttons & 0x0F);
        if (!(p & 0x20)) p &= (u8)~(buttons >> 4);
        return p;
    }
    case 0x02: return io[0x02] | 0x7E;
    case 0x04: return (u8)((cyc - div_base) >> 8);
    case 0x05: timer_catchup(); return io[0x05];
    case 0x07: return io[0x07] | 0xF8;
    case 0x0F: ppu_catchup(); timer_catchup(); return IFREG | 0xE0;
    case 0x41: ppu_catchup(); return 0x80 | (io[0x41] & 0x78) | ((ly == io[0x45]) ? 4 : 0) | ((io[0x40] & 0x80) ? mode : 0);
    case 0x44: ppu_catchup(); return (u8)ly;
    case 0xFF: return IEREG;
    }
    if (r >= 0x10 && r < 0x40) return apu_read(r);
    if (r >= 0x4C && r < 0x80) return 0xFF;
    return io[r];
}

void gb_wr_slow(u16 a, u8 v) {
    if (a < 0x8000) return;                             /* ROM (no MBC) */
    if (a < 0xA000) { vram[a - 0x8000] = v; return; }
    if (a < 0xC000) return;
    if (a < 0xFE00) { wram[(a - 0xC000) & 0x1FFF] = v; return; }
    if (a < 0xFEA0) { oam[a - 0xFE00] = v; return; }
    if (a < 0xFF00) return;
    u8 r = a & 0xFF;
    if (r >= 0x10 && r < 0x40) { apu_write(r, v); return; }
    switch (r) {
    case 0x00: io[0x00] = v & 0x30; return;
    case 0x02:
        io[0x02] = v;
        serial_in = 0xFF;
        master_waiting = 0;
        if ((v & 0x81) == 0x81) {
            serial_start = cyc;
            if (link_fn) { serial_done = ~0ULL; master_waiting = 1; link_fn(io[0x01], serial_pc, link_user); serial_pc = 0; }
            else serial_done = cyc + 4096;
        } else serial_done = ~0ULL;
        evt = 0; return;
    case 0x04: timer_catchup(); div_base = cyc; tima_last = cyc; evt = 0; return;
    case 0x05: timer_catchup(); io[0x05] = v; evt = 0; return;
    case 0x06: io[0x06] = v; return;
    case 0x07: timer_catchup(); io[0x07] = v; evt = 0; return;
    case 0x0F: ppu_catchup(); IFREG = v & 0x1F; evt = 0; return;
    case 0x40: {
        ppu_catchup();
        u8 old = io[0x40];
        io[0x40] = v;
        if ((old & 0x80) && !(v & 0x80)) { ly = 0; mode = 0; ppu_next = cyc + 70224; }
        else if (!(old & 0x80) && (v & 0x80)) { ly = 0; mode = 2; win_line = 0; ppu_next = cyc + 80; }
        stat_update(); evt = 0; return;
    }
    case 0x41: ppu_catchup(); io[0x41] = (io[0x41] & 0x07) | (v & 0x78); stat_update(); evt = 0; return;
    case 0x44: return;
    case 0x45: ppu_catchup(); io[0x45] = v; stat_update(); evt = 0; return;
    case 0x46: {
        u16 src = (u16)(v << 8);
        for (int i = 0; i < 0xA0; i++) oam[i] = rd((u16)(src + i));
        io[0x46] = v; return;
    }
    case 0x42: case 0x43: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
        ppu_catchup(); io[r] = v; return;
    case 0xFF: IEREG = v; evt = 0; return;
    }
    io[r] = v;
}

/* ---------------- CPU dispatch ---------------- */
/* handle due events and interrupts; returns 1 when a frame is complete */
static int do_sync(void) {
    sync_events();
    if (ei_delay) { ei_delay--; if (ei_delay == 0) ime = 1; else evt = 0; }
    u8 pend = IEREG & IFREG & 0x1F;
    if (halted && pend) { halted = 0; cpu.pc = halt_next; }
    if (ime && pend) {
        int b = __builtin_ctz(pend);
        IFREG &= (u8)~(1 << b);
        ime = 0;
        cpu.sp--; wr(cpu.sp, cpu.pc >> 8); cpu.sp--; wr(cpu.sp, cpu.pc & 0xFF);
        cpu.pc = (u16)(0x40 + 8 * b);
        cyc += 20;
    }
    if (frame_done) { frame_done = 0; return 1; }
    return 0;
}

void gb_run_frame(void) {
    stop_cyc = ~0ULL; evt = 0;
    for (;;) {
        if (cyc >= evt && do_sync()) return;
        STEP();
    }
}

/* run about n cycles (stops early at the end of a frame); returns 1 if a frame completed */
int gb_run_cycles(unsigned n) {
    stop_cyc = cyc + n; evt = 0;
    for (;;) {
        if (cyc >= evt) {
            if (do_sync()) { stop_cyc = ~0ULL; return 1; }
            if (cyc >= stop_cyc) { stop_cyc = ~0ULL; evt = 0; return 0; }
        }
        STEP();
    }
}

/* ---------------- link cable API ---------------- */
void gb_link_attach(gb_link_start_fn fn, void *user) { link_fn = fn; link_user = user; if (!fn) master_waiting = 0; }
int gb_link_master_waiting(void) { return master_waiting; }
void gb_link_master_complete(uint8_t b) {
    if (!master_waiting) return;
    master_waiting = 0;
    serial_in = b;
    serial_done = cyc > serial_start + 4096 ? cyc : serial_start + 4096;
    evt = 0;
}
int gb_link_passive_ready(void) { return (io[0x02] & 0x81) == 0x80; }
uint8_t gb_link_passive_exchange(uint8_t b) {
    u8 r = io[0x01];
    io[0x01] = b; io[0x02] &= 0x7F; IFREG |= 8; evt = 0;
    return r;
}
uint64_t gb_cycles(void) { return cyc; }

/* ---------------- public API ---------------- */
int gb_init(const uint8_t *src, unsigned len) {
    if (len != 0x8000) return -1;
    memcpy(rom, src, 0x8000);
    memset(wram, 0, sizeof wram); memset(vram, 0, sizeof vram);
    memset(oam, 0, sizeof oam); memset(io, 0, sizeof io);
    memset(ch, 0, sizeof ch);
    /* DMG state after the boot ROM hands over at $0100 */
    cpu.a = 0x01; cpu.f = 0xB0; cpu.b = 0x00; cpu.c = 0x13; cpu.d = 0x00; cpu.e = 0xD8;
    cpu.h = 0x01; cpu.l = 0x4D; cpu.sp = 0xFFFE; cpu.pc = 0x0100;
    cyc = 0;
    div_base = (u64)0 - 0xABCC;         /* DIV = $AB at hand-over */
    div_base = 0; div_base -= 0xABCC;
    tima_last = 0;
    ime = 0; ei_delay = 0; halted = 0; frame_done = 0;
    io[0x40] = 0x91; io[0x41] = 0x00; io[0x47] = 0xFC; io[0x48] = 0xFF; io[0x49] = 0xFF;
    io[0x0F] = 0x01; io[0x00] = 0x30;
    /* LCD has been running during boot: start at the top of a frame (line 0) */
    ly = 0; mode = 2; win_line = 0; stat_line = 0; ppu_next = 80;
    serial_done = ~0ULL;
    /* sound registers as left by the boot ROM */
    apu_on = 1; fs_step = 0; apu_cyc = 0; fs_next = 8192;
    samp_step_fp = ((u64)4194304 << 16) / GB_SAMPLE_RATE; samp_fp = samp_step_fp;
    hp_charge = 0.996f; hp_l = hp_r = 0; acc_n = 0; memset(acc, 0, sizeof acc);
    arp = awp = 0;
    io[0x26] = 0xF1; io[0x24] = 0x77; io[0x25] = 0xF3;
    io[0x11] = 0x80; io[0x12] = 0xF3; io[0x10] = 0x80; io[0x14] = 0xBF;
    ch[0].duty = 2; ch[0].env_init = 0xF; ch[0].env_per = 3; ch[0].dac = 1;
    ch[0].on = 0;                       /* boot sound has finished */
    buttons = 0;
    recompute_evt();
    return 0;
}

const uint8_t *gb_framebuffer(void) { return fb_out; }
void gb_set_buttons(uint8_t m) { buttons = m; }

int gb_audio_available(void) { return (awp - arp + ABUF) % ABUF; }
int gb_audio_read(int16_t *dst, int max) {
    int n = 0;
    while (n < max && arp != awp) {
        dst[n * 2] = abuf[arp * 2]; dst[n * 2 + 1] = abuf[arp * 2 + 1];
        arp = (arp + 1) % ABUF; n++;
    }
    return n;
}
uint16_t gb_pc(void) { return cpu.pc; }
uint8_t gb_peek(uint16_t a) { return rd(a); }

/* debug: ROM addresses that had to be interpreted (should stay empty) */
unsigned gb_interp_rom_hits;
#ifndef GB_INTERP_ONLY
static u8 interp_seen[0x8000];
void gb_note_interp(u16 pc) { if (!interp_seen[pc]) { interp_seen[pc] = 1; gb_interp_rom_hits++; } }
#endif

/* ---------------- save states (used for app resume) ---------------- */
#define SV(x) { &(x), sizeof(x) }
static const struct { void *p; unsigned n; } state_vars[] = {
    SV(cpu), SV(cyc), SV(evt), SV(ime), SV(ei_delay), SV(halted), SV(frame_done), SV(halt_next),
    SV(wram), SV(vram), SV(oam), SV(io), SV(buttons), SV(fb_work), SV(fb_out),
    SV(div_base), SV(tima_last), SV(serial_done), SV(ly), SV(mode), SV(win_line), SV(stat_line),
    SV(ppu_next), SV(ch), SV(apu_on), SV(apu_cyc), SV(fs_next), SV(fs_step), SV(samp_fp),
    SV(samp_step_fp), SV(acc), SV(acc_n), SV(hp_l), SV(hp_r), SV(hp_charge),
};
#define STATE_MAGIC 0x54455431u /* "TET1" */
unsigned gb_state_size(void) {
    unsigned n = 8;
    for (unsigned i = 0; i < sizeof state_vars / sizeof state_vars[0]; i++) n += state_vars[i].n;
    return n;
}
void gb_state_save(void *dst) {
    uint8_t *p = dst; uint32_t m = STATE_MAGIC, sz = gb_state_size();
    memcpy(p, &m, 4); memcpy(p + 4, &sz, 4); p += 8;
    for (unsigned i = 0; i < sizeof state_vars / sizeof state_vars[0]; i++) { memcpy(p, state_vars[i].p, state_vars[i].n); p += state_vars[i].n; }
}
int gb_state_load(const void *src, unsigned len) {
    const uint8_t *p = src; uint32_t m, sz;
    if (len != gb_state_size()) return -1;
    memcpy(&m, p, 4); memcpy(&sz, p + 4, 4);
    if (m != STATE_MAGIC || sz != len) return -1;
    p += 8;
    for (unsigned i = 0; i < sizeof state_vars / sizeof state_vars[0]; i++) { memcpy(state_vars[i].p, p, state_vars[i].n); p += state_vars[i].n; }
    arp = awp = 0;
    return 0;
}

void gb_poke(uint16_t a, uint8_t v) { wr(a, v); }
/* snapshot helpers that leave the host audio ring untouched; samples made
 * while looking ahead are discarded */
/* lookahead runs with the cable unplugged and restores the link state after */
static struct { gb_link_start_fn fn; u64 done, start; u8 in; int waiting; } la_stack[4];
static int la_depth;
void gb_set_headless(int on) { headless = on; la_fast = la_depth > 0 || headless; }
int gb_lookahead_begin(void *buf) {
    gb_state_save(buf);
    if (la_depth < 4) {
        la_stack[la_depth].fn = link_fn; la_stack[la_depth].done = serial_done; la_stack[la_depth].start = serial_start;
        la_stack[la_depth].in = serial_in; la_stack[la_depth].waiting = master_waiting;
    }
    la_depth++;
    la_fast = 1;
    link_fn = 0;
    return awp;
}
void gb_lookahead_end(const void *buf, int saved_awp) {
    int a = arp;
    gb_state_load(buf, gb_state_size());
    arp = a; awp = saved_awp;
    if (la_depth > 0) la_depth--;
    la_fast = la_depth > 0 || headless;
    if (la_depth < 4) {
        link_fn = la_stack[la_depth].fn; serial_done = la_stack[la_depth].done; serial_start = la_stack[la_depth].start;
        serial_in = la_stack[la_depth].in; master_waiting = la_stack[la_depth].waiting;
    }
    evt = 0;
}
