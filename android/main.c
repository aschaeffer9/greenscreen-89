/* Greenscreen '89: Android shell around the Game Boy core.
 * Pure NativeActivity (no Java): software rendering into the window,
 * AAudio for sound (also used as the frame clock), touch + key input. */
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/configuration.h>
#include <android/window.h>
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "gbcore.h"
#include "touchui.h"
#include "link.h"
#include "net.h"
#include "cpubot.h"

/* VS CPU: a second, fully separate copy of the core (every symbol renamed
 * with a b_ prefix at build time) runs the bot's Game Boy */
#define DECL_B(ret, name, args) ret b_##name args;
DECL_B(int, gb_init, (const uint8_t *, unsigned))
DECL_B(int, gb_run_cycles, (unsigned))
DECL_B(void, gb_run_frame, (void))
DECL_B(void, gb_set_buttons, (uint8_t))
DECL_B(uint8_t, gb_peek, (uint16_t))
DECL_B(int, gb_audio_read, (int16_t *, int))
DECL_B(void, gb_link_attach, (gb_link_start_fn, void *))
DECL_B(int, gb_link_master_waiting, (void))
DECL_B(void, gb_link_master_complete, (uint8_t))
DECL_B(int, gb_link_passive_ready, (void))
DECL_B(uint8_t, gb_link_passive_exchange, (uint8_t))
DECL_B(int, gb_lookahead_begin, (void *))
DECL_B(void, gb_lookahead_end, (const void *, int))
DECL_B(unsigned, gb_state_size, (void))
DECL_B(uint64_t, gb_cycles, (void))
DECL_B(int, tetris_hard_drop, (void))
DECL_B(int, tetris_piece_landed, (void))
DECL_B(int, tetris_link_hold_while_waiting, (void))
DECL_B(void, gb_set_headless, (int))


#include "rom.h"
/* the game: the player's own ROM image, loaded at run time (never shipped) */
static uint8_t tetris_rom[ROM_SIZE];
static int rom_ready;           /* a verified ROM is loaded */
static int import_mode;         /* this instance was opened with a file ("Open with" / Share) */
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
#define LOGI(...) __android_log_print(4, "Greenscreen89", __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* app / thread plumbing                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    ANativeActivity *act;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    /* requests from the UI thread, acknowledged by the game thread */
    int win_req; ANativeWindow *win_new;
    int q_req; AInputQueue *q_new;
    int save_req;
    int resumed, destroy, running;
    ARect content; int have_content;
    /* owned by the game thread */
    ANativeWindow *win;
    AInputQueue *queue;
    ALooper *looper;
    char state_path[512];
    char rom_path[512];
} App;

static App app;

static int64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* input mapping                                                       */
/* ------------------------------------------------------------------ */
#define MAXP 8
#define HIST 8
typedef struct {
    int id, active;
    float x0, y0, ax, x, y;
    int64_t t0;
    int kind;          /* 0 gesture, 1 select, 2 start, 3 volume wheel, 4 mute button */
    int moved, dropping, upsent, vertical, was_dropping, vy_steps, reset_done;
    int lock_at_down; int64_t t_cross; float ay;
    float hx[HIST], hy[HIST]; int64_t ht[HIST]; int hn;   /* recent samples for flick speed */
} Ptr;
static Ptr ptrs[MAXP];
static int pending[8];          /* queued presses per button bit */
static uint8_t pulse_prev;      /* buttons pulsed last frame */
static uint8_t key_held;        /* from physical keys/gamepad */
static float dpi = 420.f;
static int64_t back_time;       /* last BACK press, for double-back exit */
static int hard_drop_req;       /* set by flick-down / key, consumed by the game loop */
static int hard_drop_hold;      /* frames to keep DOWN held after a hard drop */
static int64_t hint_until;      /* controls hint shown for a while after launch */
static Macro macro;             /* scripted presses from menu taps */
/* 2-player link */
enum { LUI_NONE, LUI_MENU, LUI_SEARCH, LUI_MSG, LUI_CPU, LUI_ROM, LUI_CTL, LUI_INVITE };
static int lui;                 /* link overlay state */
static char lui_msg[64];
static int64_t linked_toast_until;
static Link lk;
static int linked;
static int vs_cpu;              /* linked to the built-in CPU opponent */
static int vs_cpu_level;
static Link lkB;
static CpuBot cbot;
static int unlink_check_frames; /* after leaving a 2-player game, make sure the game got back to the title */
static int lock_count;          /* pieces locked so far (to ignore stale flicks) */
static int hard_drops;          /* stats / testing */

/* layout (pixels, window coordinates) */
typedef struct { int x, y, w, h; } Rc;
static struct {
    int W, H, scale_num, scale_den;
    Rc screen, bezel, bsel, bstart, hint, spk, wheel, bctl;
    int landscape;
} L;
static int layout_gen;
static struct { Rc panel, b1, b2, b3, b4, b5, b6; } O;   /* link overlay layout */
static char server_host[128];   /* a custom MQTT broker pasted by the user ("" = the built-in public brokers) */
/* sound: DMG-style volume wheel (0..10) and a mute switch */
#define VOL_MAX 10
static int vol_level = VOL_MAX, vol_muted;
/* control feel, set in the CONTROLS panel: 1..10 (5 = default), flick 0 = hard drop off */
#define SENS_MAX 10
static int sens_touch = 5, sens_flick = 5;
static int show_online = 1;    /* listed as online for 2-player invitations */
/* 2^((5 - v) / 4): <1 = more sensitive. A table, so the app needs nothing from libm */
static float sens_factor(int v) {
    static const float f[11] = { 2.3784f, 2.0000f, 1.6818f, 1.4142f, 1.1892f, 1.0000f, 0.8409f, 0.7071f, 0.5946f, 0.5000f, 0.4204f };
    return f[v < 0 ? 0 : v > 10 ? 10 : v];
}
/* speed / load readout for the CONTROLS panel */
static int64_t perf_t0, perf_work_ns; static int perf_frames, perf_loops; static float perf_speed = 100.f, perf_ms;
static int64_t vol_shown_until;    /* show the level on screen for a moment after a change */
static void save_settings(void);

static int in_rc(Rc r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
/* buttons get a generous invisible hit area */
static int in_btn(Rc r, float x, float y) {
    /* a little extra reach sideways and below, none towards the play field above */
    int m = (int)(dpi * 0.06f);
    Rc g = { r.x - m, r.y, r.w + 2 * m, r.h + m };
    return in_rc(g, x, y);
}

static int bit_index(int b) { int i = 0; while (!(b & 1)) { b >>= 1; i++; } return i; }
static void queue_press(int btn) { int i = bit_index(btn); if (pending[i] < 6) pending[i]++; }

static int in_game_unpaused(void) { return tetris_in_game(); }

static uint8_t held_from_pointers(void) {
    uint8_t m = 0;
    for (int i = 0; i < MAXP; i++) {
        Ptr *p = &ptrs[i];
        if (!p->active) continue;
        else if (p->dropping) m |= BTN_DOWN;
    }
    return m;
}

static void overlay_tap(float x, float y);
static void open_controls(void);
static int64_t insert_anim_t0;
static void finish_insert_anim(void);
static void unlink_session(const char *why);
#define RESET_HOLD_NS 1200000000LL
static void do_soft_reset(void);
/* START button: a tap presses Start, holding it ~1 s soft-resets the game */
static float start_hold_progress(int64_t t) {
    for (int i = 0; i < MAXP; i++) {
        Ptr *p = &ptrs[i];
        if (p->active && p->kind == 2 && !p->reset_done) {
            float f = (float)(t - p->t0 - 250000000LL) / (float)(RESET_HOLD_NS - 250000000LL);
            return f < 0 ? 0 : f > 1 ? 1 : f;
        }
    }
    return 0;
}
static void check_reset_hold(int64_t t) {
    for (int i = 0; i < MAXP; i++) {
        Ptr *p = &ptrs[i];
        if (p->active && p->kind == 2 && !p->reset_done && t - p->t0 >= RESET_HOLD_NS) {
            p->reset_done = 1;
            do_soft_reset();
        }
    }
}

static void hist_add(Ptr *p, float x, float y, int64_t t) {
    int i = p->hn % HIST;
    p->hx[i] = x; p->hy[i] = y; p->ht[i] = t; p->hn++;
}

static void ptr_down(int id, float x, float y, int64_t t) {
    Ptr *p = NULL;
    for (int i = 0; i < MAXP; i++) if (!ptrs[i].active) { p = &ptrs[i]; break; }
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->id = id; p->active = 1; p->x0 = p->ax = p->x = x; p->y0 = p->ay = p->y = y; p->t0 = t;
    p->lock_at_down = lock_count;
    if (lui == LUI_NONE && in_btn(L.wheel, x, y)) p->kind = 3;
    else if (lui == LUI_NONE && in_btn(L.spk, x, y)) p->kind = 4;
    else if (in_btn(L.bsel, x, y)) p->kind = 1;
    else if (in_btn(L.bstart, x, y)) p->kind = 2;
    else if (lui == LUI_NONE && rom_ready && L.bctl.w > 0 && in_btn(L.bctl, x, y)) p->kind = 5;
    else p->kind = 0;
    hist_add(p, x, y, t);
}

static Ptr *find_ptr(int id) {
    for (int i = 0; i < MAXP; i++) if (ptrs[i].active && ptrs[i].id == id) return &ptrs[i];
    return NULL;
}

static float absf(float v) { return v < 0 ? -v : v; }

static void ptr_move(int id, float x, float y, int64_t t) {
    Ptr *p = find_ptr(id);
    if (p && p->kind == 3) {                 /* roll the wheel: up = louder */
        float step = dpi * 0.07f;
        int changed = 0;
        while (p->ay - y >= step && vol_level < VOL_MAX) { vol_level++; p->ay -= step; changed = 1; }
        while (y - p->ay >= step && vol_level > 0) { vol_level--; p->ay += step; changed = 1; }
        if (p->ay - y >= step || y - p->ay >= step) p->ay = y;     /* at the end stop */
        if (changed) { vol_muted = 0; vol_shown_until = now_ns() + 1500000000LL; save_settings(); }
        return;
    }
    if (!p || p->kind) return;
    if (lui != LUI_NONE) return;
    hist_add(p, x, y, t);
    float tf = sens_factor(sens_touch), ff = sens_factor(sens_flick);
    float step = dpi * 0.16f * tf;          /* one column per ~4 mm of drag at the default setting */
    float drop = dpi * 0.22f * tf;
    p->x = x; p->y = y;
    float dy = y - p->y0, dx = x - p->x0;
    int playing = in_game_unpaused();
    /* decide early whether this is a vertical gesture so a flick doesn't drift sideways */
    if (!p->moved && !p->vertical && absf(dy) > dpi * 0.08f * tf && absf(dy) > 2.f * absf(dx)) p->vertical = 1;
    float hstep = p->vertical ? step * 2.f : step;
    while (x - p->ax >= hstep) { queue_press(BTN_RIGHT); p->ax += hstep; p->moved = 1; }
    while (p->ax - x >= hstep) { queue_press(BTN_LEFT); p->ax -= hstep; p->moved = 1; }
    if (!playing) {
        /* menus / name entry: vertical drags step the cursor up and down */
        if (absf(dy) > 1.2f * absf(dx) || p->vertical) {
            while (y - p->ay >= step) { queue_press(BTN_DOWN); p->ay += step; p->moved = 1; }
            while (p->ay - y >= step) { queue_press(BTN_UP); p->ay -= step; p->moved = 1; }
        }
        p->dropping = 0;
        return;
    }
    if (!p->t_cross && dy > dpi * 0.2f * ff) p->t_cross = t;
    /* soft drop: finger pulled down and kept there (a quick flick is a hard drop instead) */
    if (!p->dropping && dy > drop && dy > 1.2f * absf(dx) && t - p->t0 > 140000000LL) { p->dropping = 1; p->was_dropping = 1; p->moved = 1; }
    if (p->dropping && dy < drop * 0.5f) p->dropping = 0;   /* slid back up: stop dropping */
}

/* speed (inches/s, +down) over the last ~80 ms of the gesture */
static float recent_vy(Ptr *p, int64_t t) {
    int n = p->hn < HIST ? p->hn : HIST;
    int newest = (p->hn - 1) % HIST;
    int oldest = newest;
    for (int k = 1; k < n; k++) {
        int i = (p->hn - 1 - k) % HIST;
        if (t - p->ht[i] > 80000000LL) break;
        oldest = i;
    }
    int64_t dt = p->ht[newest] - p->ht[oldest];
    if (dt < 8000000LL) dt = 8000000LL;
    return (p->hy[newest] - p->hy[oldest]) / dpi / ((float)dt / 1e9f);
}

static void ptr_up(int id, float x, float y, int64_t t, int cancel) {
    Ptr *p = find_ptr(id);
    if (!p) return;
    {
        /* SELECT / START act on release, and only for a real tap: a swipe that
         * happens to start on a button (or wanders off it) presses nothing */
        float mdx = x - p->x0, mdy = y - p->y0, slop = dpi * 0.12f;
        int clean = mdx * mdx + mdy * mdy < slop * slop && t - p->t0 < 600000000LL;
        if (!cancel && clean && p->kind == 1) queue_press(BTN_SELECT);
        if (!cancel && clean && p->kind == 2 && !p->reset_done) queue_press(BTN_START);
    }
    if (!cancel && p->kind == 5) {
        float mdx = x - p->x0, mdy = y - p->y0, slop = dpi * 0.12f;
        if (mdx * mdx + mdy * mdy < slop * slop) open_controls();
    }
    if (!cancel && p->kind == 4) {
        vol_muted = !vol_muted;
        if (!vol_muted && vol_level == 0) vol_level = 3;
        vol_shown_until = now_ns() + 1500000000LL;
        save_settings();
    }
    if (!cancel && p->kind == 0 && insert_anim_t0) { finish_insert_anim(); p->active = 0; return; }   /* tap skips the animation */
    if (!cancel && p->kind == 0 && lui != LUI_NONE) {
        float dx = x - p->x0, dy = y - p->y0, slop = dpi * 0.15f;
        if (dx * dx + dy * dy < slop * slop) overlay_tap(p->x0, p->y0);
        p->active = 0;
        return;
    }
    if (!cancel && p->kind == 0) {
        float dx = x - p->x0, dy = y - p->y0;
        float slop = dpi * 0.12f;
        int64_t dur = t - p->t0;
        int playing = in_game_unpaused();
        float ff = 1.f;
        if (!p->moved && !p->vertical && dx * dx + dy * dy < slop * slop && dur < 400000000LL) {
            if (playing) {
                /* tap: right half = A (rotate right), left half = B (rotate left) */
                queue_press(p->x0 >= L.W / 2 ? BTN_A : BTN_B);
            } else if (macro.pos >= macro.n) {
                int gx = -1, gy = -1;
                if (in_rc(L.screen, p->x0, p->y0)) {
                    gx = (int)((p->x0 - L.screen.x) * GB_W / L.screen.w);
                    gy = (int)((p->y0 - L.screen.y) * GB_H / L.screen.h);
                }
                macro.n = macro.pos = macro.phase = 0;
                if (touchui_tap(&macro, gx, gy) == TOUCHUI_OPEN_LINK) { lui = LUI_MENU; lui_msg[0] = 0; }
            }
        } else if (playing && !p->was_dropping && sens_flick > 0 && (ff = sens_factor(sens_flick)) > 0
                   && p->lock_at_down == lock_count                 /* still the same piece */
                   && dy > dpi * 0.40f * ff && dy > 2.0f * absf(dx) /* a clear, straight pull */
                   && p->t_cross && p->t_cross - p->t0 < (int64_t)(130000000.0f / ff) /* fast right from the start */
                   && dur < (int64_t)(260000000.0f / ff)
                   && recent_vy(p, t) > 5.0f * ff) {               /* still moving fast at release */
            hard_drop_req = 1;
        }
    }
    p->active = 0;
}

static int handle_motion(AInputEvent *ev) {
    int32_t a = AMotionEvent_getAction(ev);
    int act = a & AMOTION_EVENT_ACTION_MASK;
    int idx = (a & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    int64_t t = AMotionEvent_getEventTime(ev);
    switch (act) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        ptr_down(AMotionEvent_getPointerId(ev, idx), AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx), t);
        break;
    case AMOTION_EVENT_ACTION_MOVE: {
        size_t n = AMotionEvent_getPointerCount(ev);
        for (size_t i = 0; i < n; i++)
            ptr_move(AMotionEvent_getPointerId(ev, i), AMotionEvent_getX(ev, i), AMotionEvent_getY(ev, i), t);
        break;
    }
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        ptr_move(AMotionEvent_getPointerId(ev, idx), AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx), t);
        ptr_up(AMotionEvent_getPointerId(ev, idx), AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx), t, 0);
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        for (int i = 0; i < MAXP; i++) ptrs[i].active = 0;
        break;
    }
    return 1;
}

static int key_to_btn(int code) {
    switch (code) {
    case AKEYCODE_DPAD_LEFT: case AKEYCODE_A: return BTN_LEFT;
    case AKEYCODE_DPAD_RIGHT: case AKEYCODE_D: return BTN_RIGHT;
    case AKEYCODE_DPAD_UP: case AKEYCODE_W: return BTN_UP;
    case AKEYCODE_DPAD_DOWN: case AKEYCODE_S: return BTN_DOWN;
    case AKEYCODE_X: case AKEYCODE_K: case AKEYCODE_BUTTON_A: case AKEYCODE_BUTTON_Y: return BTN_A;
    case AKEYCODE_Z: case AKEYCODE_J: case AKEYCODE_BUTTON_B: case AKEYCODE_BUTTON_X: return BTN_B;
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_BUTTON_START: return BTN_START;
    case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: case AKEYCODE_DEL: case AKEYCODE_TAB:
    case AKEYCODE_BUTTON_SELECT: return BTN_SELECT;
    }
    return 0;
}

static int handle_key(AInputEvent *ev) {
    int code = AKeyEvent_getKeyCode(ev);
    int action = AKeyEvent_getAction(ev);
    if (code == AKEYCODE_BACK || code == AKEYCODE_ESCAPE) {
        if (action == AKEY_EVENT_ACTION_UP && lui == LUI_ROM) { ANativeActivity_finish(app.act); return 1; }
        if (action == AKEY_EVENT_ACTION_UP && lui != LUI_NONE) {
            if (lui == LUI_SEARCH) net_stop();
            if (lui == LUI_CTL) save_settings();
            if (lui == LUI_INVITE) net_invite_answer(0);
            lui = lui == LUI_CPU ? LUI_MENU : LUI_NONE;
            return 1;
        }
        if (action == AKEY_EVENT_ACTION_UP && linked) {
            int64_t t = now_ns();
            if (back_time && t - back_time < 2000000000LL) { back_time = 0; unlink_session("UNLINKED"); }
            else back_time = t;
            return 1;
        }
        if (action == AKEY_EVENT_ACTION_UP && touchui_back_is_b()) {
            back_time = 0;
            queue_press(BTN_B);
        } else if (action == AKEY_EVENT_ACTION_UP) {
            int64_t t = now_ns();
            if (back_time && t - back_time < 2000000000LL) { ANativeActivity_finish(app.act); back_time = 0; }
            else {
                back_time = t;
                if (in_game_unpaused()) queue_press(BTN_START);   /* pause first */
            }
        }
        return 1;
    }
    /* hard drop: Space, or Up / R1 while playing (Up does nothing in-game on the original) */
    if (code == AKEYCODE_SPACE || code == AKEYCODE_BUTTON_R1 ||
        (in_game_unpaused() && (code == AKEYCODE_DPAD_UP || code == AKEYCODE_W))) {
        if (in_game_unpaused()) {
            if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(ev) == 0) hard_drop_req = 1;
            return 1;
        }
        if (code == AKEYCODE_SPACE || code == AKEYCODE_BUTTON_R1) {   /* menus: act as Start */
            if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(ev) == 0) queue_press(BTN_START);
            return 1;
        }
    }
    int b = key_to_btn(code);
    if (!b) return 0;   /* volume keys etc. go to the system */
    if (action == AKEY_EVENT_ACTION_DOWN) {
        if (AKeyEvent_getRepeatCount(ev) == 0) key_held |= (uint8_t)b;
    } else if (action == AKEY_EVENT_ACTION_UP) key_held &= (uint8_t)~b;
    return 1;
}

static void process_input(void) {
    if (!app.queue) return;
    AInputEvent *ev = NULL;
    while (AInputQueue_getEvent(app.queue, &ev) >= 0) {
        if (AInputQueue_preDispatchEvent(app.queue, ev)) continue;
        int handled = 0;
        int type = AInputEvent_getType(ev);
        if (type == AINPUT_EVENT_TYPE_MOTION) handled = handle_motion(ev);
        else if (type == AINPUT_EVENT_TYPE_KEY) handled = handle_key(ev);
        AInputQueue_finishEvent(app.queue, ev, handled);
    }
}

/* the game's own soft reset: A+B+Select+Start held together */
static void do_soft_reset(void) {
    if (linked) { linked = 0; gb_link_attach(NULL, NULL); net_stop(); touchui_set_linked(0); unlink_check_frames = 120; }
    if (vs_cpu) { vs_cpu = 0; b_gb_link_attach(NULL, NULL); }
    macro.n = macro.pos = macro.phase = 0;
    for (int k = 0; k < 3; k++) macro_push(&macro, BTN_A | BTN_B | BTN_SELECT | BTN_START);
    hard_drop_req = 0; hard_drop_hold = 0;
}

/* buttons for the next emulated frame: held state + queued one-frame taps */
static uint8_t frame_buttons(void) {
    if (macro.pos < macro.n) {
        uint8_t mm = 0;
        if (macro.phase == 0) { mm = macro.steps[macro.pos]; macro.phase = 1; }
        else { macro.phase = 0; macro.pos++; }
        memset(pending, 0, sizeof pending);
        pulse_prev = 0;
        return mm;
    }
    uint8_t pulse = 0;
    for (int i = 0; i < 8; i++) {
        int b = 1 << i;
        if (pending[i] > 0 && !(pulse_prev & b)) { pulse |= (uint8_t)b; pending[i]--; }
    }
    pulse_prev = pulse;
    uint8_t m = pulse | held_from_pointers() | key_held;
    if (hard_drop_req) {
        hard_drop_req = 0;
        if (tetris_hard_drop()) { hard_drop_hold = 4; hard_drops++; }
    }
    if (hard_drop_hold > 0) {
        /* let the original soft-drop code take the final step and lock the piece */
        if (tetris_piece_landed()) hard_drop_hold = 0;
        else { hard_drop_hold--; m = (uint8_t)((m & ~(BTN_LEFT | BTN_RIGHT | BTN_UP)) | BTN_DOWN); }
    }
    /* never send opposite directions together */
    if ((m & (BTN_LEFT | BTN_RIGHT)) == (BTN_LEFT | BTN_RIGHT)) m &= (uint8_t)~(BTN_LEFT | BTN_RIGHT);
    return m;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */
#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
static const uint32_t shade[4] = { RGB(0xC4, 0xCF, 0xA1), RGB(0x8B, 0x95, 0x6D), RGB(0x4D, 0x53, 0x3C), RGB(0x1F, 0x1F, 0x1F) };
#define C_BG     RGB(0x15, 0x16, 0x19)
#define C_BEZEL  RGB(0x0B, 0x0C, 0x0D)
#define C_EDGE   RGB(0x2C, 0x2E, 0x33)
#define C_PAD    RGB(0x1C, 0x1E, 0x22)
#define C_PADHI  RGB(0x26, 0x29, 0x2E)
#define C_DIM    RGB(0x5C, 0x60, 0x68)
#define C_TEXT   RGB(0xB8, 0xBC, 0xC4)
#define C_GREEN  RGB(0x5C, 0xD0, 0x6A)
#define C_BTN    RGB(0x2F, 0x32, 0x38)
#define C_BTNON  RGB(0x4A, 0x4F, 0x58)

typedef struct { uint32_t *px; int stride, w, h; } Canvas;
static void draw_insert_anim(Canvas *c, int64_t t);

static void fill(Canvas *c, int x, int y, int w, int h, uint32_t col) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint32_t *row = c->px + (size_t)(y + j) * c->stride + x;
        for (int i = 0; i < w; i++) row[i] = col;
    }
}

static void rrect(Canvas *c, Rc r, int rad, uint32_t col) {
    if (rad * 2 > r.h) rad = r.h / 2;
    if (rad * 2 > r.w) rad = r.w / 2;
    for (int j = 0; j < r.h; j++) {
        int inset = 0;
        int dy = j < rad ? rad - j : (j >= r.h - rad ? j - (r.h - rad - 1) : 0);
        if (dy) {
            int k = 0;
            while (k < rad && (rad - k) * (rad - k) + dy * dy > rad * rad) k++;
            inset = k;
        }
        fill(c, r.x + inset, r.y + j, r.w - 2 * inset, 1, col);
    }
}

/* 5x7 font, rows top->bottom, 5 bits each (MSB = left) */
static const struct { char ch; uint8_t rows[7]; } font[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'J', {7, 2, 2, 2, 2, 18, 12}}, {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}}, {'T', {31, 4, 4, 4, 4, 4, 4}}, {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}}, {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}}, {'Z', {31, 1, 2, 4, 8, 16, 31}}, {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'.', {0, 0, 0, 0, 0, 12, 12}}, {':', {0, 12, 12, 0, 12, 12, 0}}, {'-', {0, 0, 0, 14, 0, 0, 0}},
    {'/', {1, 1, 2, 4, 8, 16, 16}}, {'<', {2, 4, 8, 16, 8, 4, 2}}, {'>', {8, 4, 2, 1, 2, 4, 8}},
    {'^', {4, 14, 21, 4, 4, 4, 4}}, {'v', {4, 4, 4, 4, 21, 14, 4}}, {'(', {2, 4, 8, 8, 8, 4, 2}},
    {')', {8, 4, 2, 2, 2, 4, 8}}, {'+', {0, 4, 4, 31, 4, 4, 0}},
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}}, {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {31, 2, 4, 2, 1, 17, 14}}, {'4', {2, 6, 10, 18, 31, 2, 2}}, {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}}, {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}}, {'=', {0, 0, 31, 0, 31, 0, 0}}, {'!', {4, 4, 4, 4, 4, 0, 4}},
    {'?', {14, 17, 1, 2, 4, 0, 4}}, {',', {0, 0, 0, 0, 12, 4, 8}}, {'\'', {4, 4, 8, 0, 0, 0, 0}},
    {'|', {4, 4, 4, 4, 4, 4, 4}}, {'[', {14, 8, 8, 8, 8, 8, 14}}, {']', {14, 2, 2, 2, 2, 2, 14}},
    {'_', {0, 0, 0, 0, 0, 0, 31}}, {'*', {0, 21, 14, 31, 14, 21, 0}}, {'#', {10, 31, 10, 10, 31, 10, 0}},
};

static const uint8_t *glyph(char ch) {
    for (unsigned i = 0; i < sizeof font / sizeof font[0]; i++) if (font[i].ch == ch) return font[i].rows;
    return font[26].rows;
}

static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }

static void text(Canvas *c, int x, int y, const char *s, int sc, uint32_t col) {
    for (; *s; s++, x += 6 * sc) {
        const uint8_t *g = glyph(*s);
        for (int r = 0; r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (g[r] & (16 >> b)) fill(c, x + b * sc, y + r * sc, sc, sc, col);
    }
}
static void text_c(Canvas *c, int cx, int y, const char *s, int sc, uint32_t col) { text(c, cx - text_w(s, sc) / 2, y, s, sc, col); }

static void compute_layout(int W, int H) {
    memset(&L, 0, sizeof L);
    L.W = W; L.H = H;
    int cx = 0, cy = 0, cw = W, ch = H;
    if (app.have_content) {
        ARect r = app.content;
        if (r.right > r.left && r.bottom > r.top && r.right <= W && r.bottom <= H) {
            cx = r.left; cy = r.top; cw = r.right - r.left; ch = r.bottom - r.top;
        }
    }
    float dp = dpi / 160.f;
    int m = (int)(10 * dp);
    int bez = (int)(5 * dp);
    L.landscape = cw > ch;
    int btn_h = (int)(40 * dp), btn_w = (int)(100 * dp);
    /* biggest screen that fits; whole-pixel scaling unless that wastes >10% */
    int availw = cw - 2 * (m + bez), availh = ch - 2 * (m + bez);
    if (!L.landscape) availh -= 2 * (btn_h + m);      /* keep room for the buttons below */
    float f = (float)availw / GB_W;
    if ((float)availh / GB_H < f) f = (float)availh / GB_H;
    if (f < 1) f = 1;
    int si = (int)f;
    int sw, sh;
    if (si >= 1 && si >= 0.9f * f) { sw = GB_W * si; sh = GB_H * si; }
    else { sw = (int)(GB_W * f); sh = (int)(GB_H * f); }
    L.screen = (Rc){cx + (cw - sw) / 2, cy + (ch - sh) / 2, sw, sh};
    L.bezel = (Rc){L.screen.x - bez, L.screen.y - bez, L.screen.w + 2 * bez, L.screen.h + 2 * bez};
    if (!L.landscape) {
        int gap = (int)(20 * dp);
        int by = L.bezel.y + L.bezel.h + (int)(64 * dp);   /* well clear of the play field */
        L.bsel = (Rc){cx + cw / 2 - gap / 2 - btn_w, by, btn_w, btn_h};
        L.bstart = (Rc){cx + cw / 2 + gap / 2, by, btn_w, btn_h};
        int ch_ = (int)(28 * dp), cwid = (int)(120 * dp);
        L.bctl = (Rc){cx + (cw - cwid) / 2, by + btn_h + (int)(16 * dp), cwid, ch_};
        L.hint = (Rc){cx, L.bctl.y + ch_ + (int)(18 * dp), cw, 0};
        /* mute button on the left of the row, DMG-style volume wheel on the right */
        int ww = (int)(btn_h * 0.55f), wh = (int)(btn_h * 1.5f);
        L.spk = (Rc){L.bsel.x - gap - btn_h, by, btn_h, btn_h};
        L.wheel = (Rc){L.bstart.x + L.bstart.w + gap + (btn_h - ww) / 2, by + btn_h / 2 - wh / 2, ww, wh};
        if (L.spk.x < cx + 4) { L.spk.x = cx + 4; }
        if (L.wheel.x + L.wheel.w > cx + cw - 4) L.wheel.x = cx + cw - 4 - L.wheel.w;
    } else {
        int lw = L.bezel.x - cx, rw = cx + cw - (L.bezel.x + L.bezel.w);
        int bw = btn_w; if (bw > lw - 2 * m) bw = lw - 2 * m; if (bw > rw - 2 * m) bw = rw - 2 * m;
        if (bw < (int)(56 * dp)) bw = (int)(56 * dp);
        int by = L.bezel.y + L.bezel.h - btn_h;
        L.bsel = (Rc){cx + (lw - bw) / 2, by, bw, btn_h};
        L.bstart = (Rc){L.bezel.x + L.bezel.w + (rw - bw) / 2, by, bw, btn_h};
        L.hint = (Rc){0, 0, 0, 0};
        L.bctl = (Rc){cx + (lw - bw) / 2, L.bezel.y, bw, (int)(28 * dp)};
        int ww = (int)(btn_h * 0.55f), wh = (int)(btn_h * 1.5f);
        L.spk = (Rc){cx + (lw - btn_h) / 2, by - (int)(16 * dp) - btn_h, btn_h, btn_h};
        L.wheel = (Rc){L.bezel.x + L.bezel.w + (rw - ww) / 2, by - (int)(16 * dp) - wh, ww, wh};
    }
    layout_gen++;
}

static void draw_button(Canvas *c, Rc r, const char *label, int on, int tsc) {
    rrect(c, r, r.h / 2, on ? C_BTNON : C_BTN);
    while (tsc > 1 && text_w(label, tsc) > r.w - r.h / 2) tsc--;
    text_c(c, r.x + r.w / 2, r.y + (r.h - 7 * tsc) / 2, label, tsc, C_TEXT);
}

static const char *SPK_ON[8] = { "....#......", "...##...#..", "####..#..#.", "#####.#..#.", "#####.#..#.", "####..#..#.", "...##...#..", "....#......" };
static const char *SPK_OFF[8] = { "....#......", "...##......", "####..#...#", "#####..#.#.", "#####...#..", "####...#.#.", "...##.#...#", "....#......" };
static void draw_speaker(Canvas *c, Rc r, int muted, int pressed) {
    rrect(c, r, r.h / 2, pressed ? C_BTNON : C_BTN);
    const char **bm = muted ? SPK_OFF : SPK_ON;
    int px = r.h / 16; if (px < 2) px = 2;
    int w = 11 * px, h = 8 * px, x0 = r.x + (r.w - w) / 2, y0 = r.y + (r.h - h) / 2;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 11; x++)
            if (bm[y][x] == '#') fill(c, x0 + x * px, y0 + y * px, px, px, muted ? C_DIM : C_TEXT);
}
/* the ridged thumbwheel from the side of the original Game Boy */
static void draw_wheel(Canvas *c, Rc r, int level, int active) {
    rrect(c, r, r.w / 3, C_EDGE);
    Rc in = { r.x + 2, r.y + 2, r.w - 4, r.h - 4 };
    rrect(c, in, r.w / 3, active ? C_BTNON : C_BTN);
    int gap = r.h / 9; if (gap < 4) gap = 4;
    int phase = (level * gap / 3) % gap;         /* ridges roll as the level changes */
    for (int y = in.y + gap / 2 - phase + gap; y < in.y + in.h - gap / 2; y += gap) {
        /* ridges look rounder towards the middle of the wheel */
        float tpos = (float)(y - in.y) / (float)in.h;
        int inset = (int)(in.w * 0.18f * (1.0f - 4.0f * (tpos - 0.5f) * (tpos - 0.5f)) );
        inset = in.w / 5 - inset / 2; if (inset < 2) inset = 2;
        int th = gap / 4 > 1 ? gap / 4 : 1;
        fill(c, in.x + inset, y, in.w - 2 * inset, th, C_BEZEL);
    }
}

/* nearest-neighbour scale of the 160x144 frame into L.screen */
static int xmap[4096], ymap[4096], map_w, map_h;
static void draw_screen(Canvas *c) {
    const uint8_t *fb = gb_framebuffer();
    Rc r = L.screen;
    if (r.w > 4096 || r.h > 4096) return;
    if (map_w != r.w || map_h != r.h) {
        for (int i = 0; i < r.w; i++) xmap[i] = i * GB_W / r.w;
        for (int i = 0; i < r.h; i++) ymap[i] = i * GB_H / r.h;
        map_w = r.w; map_h = r.h;
    }
    int last_sy = -1;
    uint32_t *prev = NULL;
    for (int y = 0; y < r.h; y++) {
        int oy = r.y + y;
        if (oy < 0 || oy >= c->h) continue;
        uint32_t *dst = c->px + (size_t)oy * c->stride + r.x;
        int sy = ymap[y];
        if (sy == last_sy && prev) { memcpy(dst, prev, (size_t)r.w * 4); continue; }
        const uint8_t *src = fb + sy * GB_W;
        for (int x = 0; x < r.w; x++) dst[x] = shade[src[xmap[x]] & 3];
        last_sy = sy; prev = dst;
    }
}

static int any_ptr_kind(int k) { for (int i = 0; i < MAXP; i++) if (ptrs[i].active && ptrs[i].kind == k) return 1; return 0; }
static int any_start_ptr(void) { for (int i = 0; i < MAXP; i++) if (ptrs[i].active && ptrs[i].kind == 2) return 1; return 0; }
static void draw_overlay(Canvas *c, int tsc);
static int last_ui_key = -1;
static int full_redraws_left;

static void render(void) {
    ANativeWindow *w = app.win;
    if (!w) return;
    int W = ANativeWindow_getWidth(w), H = ANativeWindow_getHeight(w);
    if (W <= 0 || H <= 0) return;
    if (W != L.W || H != L.H) { compute_layout(W, H); full_redraws_left = 4; }
    int64_t t = now_ns();
    uint8_t held = held_from_pointers() | key_held;
    float start_prog = start_hold_progress(t);
    if (start_prog > 0 || any_start_ptr()) held |= BTN_START;
    if (any_ptr_kind(1)) held |= BTN_SELECT;
    int back_msg = back_time && t - back_time < 2000000000LL;
    int hint = hint_until > t;
    int ui_key = (!!(held & BTN_SELECT)) << 2 | (!!(held & BTN_START)) << 3 | hint << 4 | ((int)(start_prog * 20) << 5)
               | (vol_level << 10) | (vol_muted << 14) | (any_ptr_kind(3) << 15) | (any_ptr_kind(4) << 16) | (any_ptr_kind(5) << 17)
               | (rom_ready << 18) | (layout_gen << 19);
    if (ui_key != last_ui_key) { full_redraws_left = 4; last_ui_key = ui_key; }

    ANativeWindow_Buffer buf;
    ARect dirty;
    int full = full_redraws_left > 0;
    if (full) { dirty.left = 0; dirty.top = 0; dirty.right = W; dirty.bottom = H; }
    else { dirty.left = L.screen.x; dirty.top = L.screen.y; dirty.right = L.screen.x + L.screen.w; dirty.bottom = L.screen.y + L.screen.h; }
    if (ANativeWindow_lock(w, &buf, &dirty) != 0) return;
    int direct = buf.format == WINDOW_FORMAT_RGBA_8888 || buf.format == WINDOW_FORMAT_RGBX_8888;
    static uint32_t *back; static int back_w, back_h;
    Canvas c;
    if (direct) {
        c = (Canvas){ (uint32_t *)buf.bits, buf.stride, buf.width, buf.height };
    } else {
        /* unexpected format (e.g. RGB_565): draw into a 32-bit back buffer and convert */
        if (!back || back_w != buf.width || back_h != buf.height) {
            free(back); back_w = buf.width; back_h = buf.height;
            back = calloc((size_t)back_w * back_h, 4);
            if (!back) { ANativeWindow_unlockAndPost(w); return; }
            full_redraws_left = 4;
            dirty.left = 0; dirty.top = 0; dirty.right = buf.width; dirty.bottom = buf.height;
        }
        c = (Canvas){ back, back_w, back_w, back_h };
    }
    /* if the system could not preserve the old contents it widens the dirty rect */
    if (dirty.left > L.screen.x || dirty.top > L.screen.y ||
        dirty.right < L.screen.x + L.screen.w || dirty.bottom < L.screen.y + L.screen.h) full = 1;
    if (dirty.left <= 0 && dirty.top <= 0 && dirty.right >= W && dirty.bottom >= H) full = 1;
    if (full) {
        float dp = dpi / 160.f;
        int tsc = (int)(dp * 2.2f + 0.5f); if (tsc < 2) tsc = 2;
        fill(&c, 0, 0, c.w, c.h, C_BG);
        rrect(&c, L.bezel, (int)(8 * dp), C_EDGE);
        Rc in = { L.bezel.x + 2, L.bezel.y + 2, L.bezel.w - 4, L.bezel.h - 4 };
        rrect(&c, in, (int)(7 * dp), C_BEZEL);
        int bts = tsc; if (bts > 5) bts = 5;
        draw_speaker(&c, L.spk, vol_muted, any_ptr_kind(4));
        draw_wheel(&c, L.wheel, vol_level, any_ptr_kind(3));
        {
            int ls = bts > 3 ? bts - 1 : bts;
            text_c(&c, L.wheel.x + L.wheel.w / 2, L.wheel.y + L.wheel.h + ls * 3, "VOL", ls, C_DIM);
        }
        draw_button(&c, L.bsel, "SELECT", held & BTN_SELECT, bts);
        draw_button(&c, L.bstart, "START", held & BTN_START, bts);
        if (rom_ready && L.bctl.w > 0) draw_button(&c, L.bctl, "CONTROLS", any_ptr_kind(5), bts > 3 ? bts - 1 : bts);
        if (start_prog > 0) {
            Rc f = L.bstart; f.w = (int)(L.bstart.w * start_prog);
            Rc bar = { f.x + f.h / 4, f.y + f.h - f.h / 6 - 2, f.w - f.h / 2, f.h / 8 > 2 ? f.h / 8 : 2 };
            if (bar.w > 0) fill(&c, bar.x, bar.y, bar.w, bar.h, C_TEXT);
        }
        if (hint && L.hint.w > 0) {
            int hs = 4;
            while (hs > 1 && text_w("TAP MENU ITEMS - BACK = B", hs) > L.hint.w - 20) hs--;
            int y = L.hint.y;
            text_c(&c, L.hint.x + L.hint.w / 2, y, "TAP MENU ITEMS - BACK = B", hs, C_DIM); y += 12 * hs;
            text_c(&c, L.hint.x + L.hint.w / 2, y, "SWIPE < > TO MOVE", hs, C_DIM); y += 12 * hs;
            text_c(&c, L.hint.x + L.hint.w / 2, y, "TAP TO ROTATE", hs, C_DIM); y += 12 * hs;
            text_c(&c, L.hint.x + L.hint.w / 2, y, "HOLD v TO SOFT DROP", hs, C_DIM); y += 12 * hs;
            text_c(&c, L.hint.x + L.hint.w / 2, y, "FLICK v TO HARD DROP", hs, C_DIM); y += 12 * hs;
            text_c(&c, L.hint.x + L.hint.w / 2, y, "HOLD START TO RESET", hs, C_DIM);
        }
        if (full_redraws_left > 0) full_redraws_left--;
    }
    if (insert_anim_t0) { draw_insert_anim(&c, t); full_redraws_left = 2; }
    else draw_screen(&c);
    if (!insert_anim_t0) {
        float dp = dpi / 160.f;
        int tsc = (int)(dp * 1.6f + 0.5f); if (tsc < 2) tsc = 2;
        if (lui != LUI_NONE) draw_overlay(&c, tsc);
        else if (linked) {
            static const char *lvl[3] = { "CPU EASY", "CPU NORMAL", "CPU HARD" };
            const char *b = linked_toast_until > t
                ? (vs_cpu ? (tetris_is_2player() ? "VS CPU" : "VS CPU - TAP 2PLAYER") : (tetris_is_2player() ? "LINKED" : "LINKED - TAP 2PLAYER"))
                : (vs_cpu ? lvl[vs_cpu_level] : net_online_direct() ? "LINK P2P" : "LINK");
            int bs = linked_toast_until > t ? tsc : (tsc > 2 ? tsc - 1 : tsc);
            while (bs > 1 && text_w(b, bs) + 8 * bs > L.screen.w) bs--;
            int ww = text_w(b, bs) + 8 * bs, hh = 11 * bs;
            Rc r = { L.screen.x + L.screen.w - ww - bs * 2, L.screen.y + bs * 2, ww, hh };
            if (linked_toast_until > t) r.x = L.screen.x + (L.screen.w - ww) / 2;
            rrect(&c, r, hh / 2, linked_toast_until > t ? C_BTNON : C_BEZEL);
            text_c(&c, r.x + ww / 2, r.y + 2 * bs, b, bs, C_TEXT);
        }
    }
    if (vol_shown_until > t) {
        float dp = dpi / 160.f;
        int u = (int)(dp * 5); if (u < 6) u = 6;
        int bw = VOL_MAX * (u + u / 2) + u, bh = u * 3;
        Rc r = { L.screen.x + (L.screen.w - bw) / 2, L.screen.y + L.screen.h - bh - (int)(14 * dp), bw, bh };
        rrect(&c, r, u, C_BEZEL);
        for (int i = 0; i < VOL_MAX; i++)
            fill(&c, r.x + u + i * (u + u / 2), r.y + u, u, u, (!vol_muted && i < vol_level) ? C_TEXT : C_BTN);
    }
    if (back_msg) {
        float dp = dpi / 160.f;
        int tsc = (int)(dp * 1.6f + 0.5f); if (tsc < 2) tsc = 2;
        const char *msg = linked ? "PRESS BACK AGAIN TO UNLINK" : "PRESS BACK AGAIN TO EXIT";
        while (tsc > 1 && text_w(msg, tsc) + 16 * tsc > L.screen.w) tsc--;
        int ww = text_w(msg, tsc) + 16 * tsc, hh = 15 * tsc;
        Rc r = { L.screen.x + (L.screen.w - ww) / 2, L.screen.y + L.screen.h - hh - (int)(10 * dp), ww, hh };
        rrect(&c, r, hh / 2, C_BTNON);
        text_c(&c, r.x + ww / 2, r.y + 4 * tsc, msg, tsc, C_TEXT);
    }
    if (!direct) {
        if (buf.format == WINDOW_FORMAT_RGB_565) {
            int x0 = dirty.left < 0 ? 0 : dirty.left, y0 = dirty.top < 0 ? 0 : dirty.top;
            int x1 = dirty.right > c.w ? c.w : dirty.right, y1 = dirty.bottom > c.h ? c.h : dirty.bottom;
            if (full) { x0 = 0; y0 = 0; x1 = c.w; y1 = c.h; }
            for (int y = y0; y < y1; y++) {
                uint16_t *d = (uint16_t *)buf.bits + (size_t)y * buf.stride;
                const uint32_t *sp = back + (size_t)y * back_w;
                for (int x = x0; x < x1; x++) {
                    uint32_t v = sp[x];
                    d[x] = (uint16_t)(((v & 0xF8) << 8) | ((v >> 5) & 0x7E0) | ((v >> 19) & 0x1F));
                }
            }
        }
    }
    ANativeWindow_unlockAndPost(w);
}

/* ------------------------------------------------------------------ */
/* audio                                                               */
/* ------------------------------------------------------------------ */
static AAudioStream *astream;
static int audio_ok;

static void audio_open(void) {
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, GB_SAMPLE_RATE);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    if (AAudioStreamBuilder_openStream(b, &astream) != AAUDIO_OK) astream = NULL;
    AAudioStreamBuilder_delete(b);
    if (!astream) return;
    if (AAudioStream_getSampleRate(astream) != GB_SAMPLE_RATE || AAudioStream_getChannelCount(astream) != 2 ||
        AAudioStream_getFormat(astream) != AAUDIO_FORMAT_PCM_I16) {
        LOGI("audio format mismatch, running without sound");
        AAudioStream_close(astream); astream = NULL; return;
    }
    int burst = AAudioStream_getFramesPerBurst(astream);
    int want = 1700;
    if (burst > 0) want = ((want + burst - 1) / burst) * burst;
    AAudioStream_setBufferSizeInFrames(astream, want);
    if (AAudioStream_requestStart(astream) != AAUDIO_OK) { AAudioStream_close(astream); astream = NULL; return; }
    audio_ok = 1;
}

static void audio_close(void) {
    if (astream) { AAudioStream_requestStop(astream); AAudioStream_close(astream); }
    astream = NULL; audio_ok = 0;
}

/* Keep the sound alive. Once per frame while the app is in front:
 *  - a stream that is paused / stopped / still pausing gets started (requestStart
 *    fails while a pause is in progress, so a quick switch away and back used to
 *    leave it paused for good: silence);
 *  - a disconnected stream (headphones, Bluetooth, another app took the device)
 *    is closed and reopened;
 *  - a stream that stops playing for a second while we keep feeding it is reopened. */
static int64_t audio_last_progress_ns, audio_last_read;
static void audio_service(int64_t t) {
    if (!astream) {
        static int64_t next_try;
        if (t >= next_try) { audio_open(); next_try = t + 2000000000LL; audio_last_progress_ns = t; }
        return;
    }
    aaudio_stream_state_t st = AAudioStream_getState(astream);
    if (st == AAUDIO_STREAM_STATE_DISCONNECTED || st == AAUDIO_STREAM_STATE_CLOSING || st == AAUDIO_STREAM_STATE_CLOSED) {
        LOGI("audio stream state %d, reopening", (int)st);
        audio_close(); audio_open(); audio_last_progress_ns = t; return;
    }
    if (st != AAUDIO_STREAM_STATE_STARTED && st != AAUDIO_STREAM_STATE_STARTING) AAudioStream_requestStart(astream);
    int64_t rd = AAudioStream_getFramesRead(astream);
    if (rd != audio_last_read || !audio_last_progress_ns) { audio_last_read = rd; audio_last_progress_ns = t; }
    else if (t - audio_last_progress_ns > 1000000000LL) {
        LOGI("audio stream stuck, reopening");
        audio_close(); audio_open(); audio_last_progress_ns = t; audio_last_read = 0;
    }
}

/* push the samples of one frame; blocks while the device buffer is full,
 * which is what keeps the game running at the real Game Boy speed */
static int64_t next_frame_ns;
static void apply_volume(int16_t *buf, int frames) {
    int v = vol_muted ? 0 : vol_level;
    if (v >= VOL_MAX) return;
    int32_t g = (int32_t)(32768.0f * (float)(v * v) / (float)(VOL_MAX * VOL_MAX));   /* loudness feels ~quadratic */
    for (int i = 0; i < frames * 2; i++) buf[i] = (int16_t)((buf[i] * g) >> 15);
}
static void audio_push_and_pace(void) {
    static int16_t tmp[4096 * 2];
    int n;
    if (astream) {
        while ((n = gb_audio_read(tmp, 4096)) > 0) {
            apply_volume(tmp, n);
            aaudio_result_t r = AAudioStream_write(astream, tmp, n, 100000000LL);
            if (r < 0) {
                LOGI("audio write error %d, reopening", (int)r);
                audio_close(); audio_open();
                break;
            }
            if (r < n) break;            /* not draining (paused): drop the rest, audio_service restarts it */
        }
        if (astream) { next_frame_ns = 0; return; }
    }
    /* no audio device: drop samples and pace with the clock */
    while (gb_audio_read(tmp, 4096) > 0) {}
    const int64_t frame = 16742706LL;   /* 70224 / 4194304 s */
    int64_t t = now_ns();
    if (next_frame_ns == 0 || t - next_frame_ns > 100000000LL) next_frame_ns = t;
    next_frame_ns += frame;
    if (next_frame_ns > t) {
        struct timespec ts = { 0, (long)(next_frame_ns - t) };
        nanosleep(&ts, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* state persistence                                                   */
/* ------------------------------------------------------------------ */
static void save_state(void) {
    if (!app.state_path[0] || !rom_ready || import_mode) return;
    if (linked || tetris_is_2player()) return;     /* a link game can't be resumed alone */
    unsigned n = gb_state_size();
    void *b = malloc(n);
    if (!b) return;
    gb_state_save(b);
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", app.state_path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        int ok = fwrite(b, 1, n, f) == n;
        ok &= fflush(f) == 0;
        fsync(fileno(f));
        fclose(f);
        if (ok) rename(tmp, app.state_path);
    }
    free(b);
}

/* v3 uses MQTT brokers: a v2 relay address in the old ".server" file is ignored */
static void server_path(char *p, int n) { snprintf(p, (size_t)n, "%s.broker", app.state_path); }
static void save_server(void) {
    char p[600]; server_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (f) { fputs(server_host, f); fclose(f); }
}
static void save_settings(void) {
    if (!app.state_path[0] || import_mode) return;
    char p[600]; snprintf(p, sizeof p, "%s.settings", app.state_path);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "vol %d\nmute %d\ntouch %d\nflick %d\nonline %d\n", vol_level, vol_muted, sens_touch, sens_flick, show_online); fclose(f); }
}
static void load_settings(void) {
    char p[600]; snprintf(p, sizeof p, "%s.settings", app.state_path);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char key[16]; int v;
    while (fscanf(f, "%15s %d", key, &v) == 2) {
        if (!strcmp(key, "vol") && v >= 0 && v <= VOL_MAX) vol_level = v;
        else if (!strcmp(key, "mute")) vol_muted = v ? 1 : 0;
        else if (!strcmp(key, "touch") && v >= 1 && v <= SENS_MAX) sens_touch = v;
        else if (!strcmp(key, "flick") && v >= 0 && v <= SENS_MAX) sens_flick = v;
        else if (!strcmp(key, "online")) show_online = v ? 1 : 0;
    }
    fclose(f);
}
static void load_server(void) {
    server_host[0] = 0;
    char p[600]; server_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
        char b[128] = "";
        if (fgets(b, sizeof b, f)) { char h[128]; if (net_parse_host(b, h, sizeof h) == 0) snprintf(server_host, sizeof server_host, "%s", h); }
        fclose(f);
    }
}

static int load_state(void) {
    if (!app.state_path[0]) return 0;
    FILE *f = fopen(app.state_path, "rb");
    if (!f) return 0;
    unsigned n = gb_state_size();
    void *b = malloc(n);
    int ok = 0;
    if (b && fread(b, 1, n, f) == n && fgetc(f) == EOF) ok = gb_state_load(b, n) == 0;
    fclose(f);
    free(b);
    if (ok && tetris_is_2player()) { gb_init(tetris_rom, sizeof tetris_rom); ok = 0; }
    return ok;
}


/* ------------------------------------------------------------------ */
/* 2-player link session                                             */
/* ------------------------------------------------------------------ */
static void link_send_cb(void *ctx, const uint8_t *p, int n) { (void)ctx; net_send(p, n); }
static void on_link_start(uint8_t b, uint16_t pc, void *user) {
    (void)user;
    link_master_start(&lk, b, tetris_link_fire_and_forget(pc), now_ns());
}

/* VS CPU: the two links talk through an in-memory queue (delivered on the
 * next pump, never re-entrantly from inside a send) */
typedef struct { int to, n; uint8_t d[16]; } LoopPkt;
static LoopPkt loopq[256];
static int loopn;
static void loop_send(void *ctx, const uint8_t *p, int n) {
    if (loopn >= 256 || n > 16 || n <= 0) return;
    loopq[loopn].to = ctx ? 0 : 1;     /* ctx NULL = sent by the player's side */
    loopq[loopn].n = n;
    memcpy(loopq[loopn].d, p, (size_t)n);
    loopn++;
}
static void on_link_start_b(uint8_t b, uint16_t pc, void *user) {
    (void)user;
    link_master_start(&lkB, b, tetris_link_fire_and_forget(pc), now_ns());
}
static void loop_pump(int64_t t) {
    for (int guard = 0; loopn > 0 && guard < 8; guard++) {
        LoopPkt q[256]; int n = loopn;
        memcpy(q, loopq, sizeof(LoopPkt) * (size_t)n); loopn = 0;
        for (int i = 0; i < n; i++) link_on_packet(q[i].to ? &lkB : &lk, q[i].d, q[i].n, t);
    }
    link_poll(&lk, t);
    link_poll(&lkB, t);
}

static void link_pump(int64_t t) {
    if (vs_cpu) { loop_pump(t); return; }
    uint8_t buf[64]; int n;
    while ((n = net_recv(buf, sizeof buf)) > 0) link_on_packet(&lk, buf, n, t);
    link_poll(&lk, t);
}
static void link_ui_linked(void) {
    linked = 1;
    touchui_set_linked(1);
    lui = LUI_NONE;
    linked_toast_until = now_ns() + 4000000000LL;
    full_redraws_left = 4;
}
static void link_begin(void) {
    LinkCore c = { gb_link_master_waiting, gb_link_master_complete, gb_link_passive_ready, gb_link_passive_exchange, gb_cycles };
    link_init(&lk, c, link_send_cb, NULL, now_ns());
    gb_link_attach(on_link_start, NULL);
    link_ui_linked();
}
static uint8_t b_peek_fn(uint16_t a) { return b_gb_peek(a); }
static void start_vs_cpu(int level) {
    static int16_t tmp[4096 * 2];
    b_gb_init(tetris_rom, sizeof tetris_rom);
    b_gb_set_headless(1);          /* the bot's Game Boy is never shown or heard: skip drawing and sound */
    b_gb_link_attach(NULL, NULL);
    /* power the bot's Game Boy on and get it to the title screen */
    for (int f = 0; f < 330; f++) { b_gb_set_buttons(0); b_gb_run_frame(); while (b_gb_audio_read(tmp, 4096) > 0) {} }
    for (int tries = 0; tries < 6 && b_gb_peek(0xFFE1) != 0x07; tries++) {
        for (int f = 0; f < 4; f++) { b_gb_set_buttons(0x80); b_gb_run_frame(); while (b_gb_audio_read(tmp, 4096) > 0) {} }
        for (int f = 0; f < 30; f++) { b_gb_set_buttons(0); b_gb_run_frame(); while (b_gb_audio_read(tmp, 4096) > 0) {} }
    }
    int64_t t = now_ns();
    loopn = 0;
    LinkCore ca = { gb_link_master_waiting, gb_link_master_complete, gb_link_passive_ready, gb_link_passive_exchange, gb_cycles };
    LinkCore cb = { b_gb_link_master_waiting, b_gb_link_master_complete, b_gb_link_passive_ready, b_gb_link_passive_exchange, b_gb_cycles };
    link_init(&lk, ca, loop_send, NULL, t);
    link_init(&lkB, cb, loop_send, (void *)1, t);
    gb_link_attach(on_link_start, NULL);
    b_gb_link_attach(on_link_start_b, NULL);
    CpuBotCore bc = { b_peek_fn, b_gb_set_buttons, b_gb_run_frame, b_gb_lookahead_begin, b_gb_lookahead_end,
                      b_gb_state_size, b_tetris_hard_drop, b_tetris_piece_landed };
    cpubot_init(&cbot, bc, level, (uint32_t)t | 1u);
    vs_cpu = 1;
    vs_cpu_level = level;
    net_stop();
    link_ui_linked();
}
static void unlink_session(const char *why) {
    int was = linked;
    if (linked && !vs_cpu) link_send_bye(&lk, now_ns());
    gb_link_attach(NULL, NULL);
    if (vs_cpu) { b_gb_link_attach(NULL, NULL); vs_cpu = 0; loopn = 0; }
    net_stop();
    linked = 0;
    touchui_set_linked(0);
    if (was && tetris_is_2player()) { do_soft_reset(); unlink_check_frames = 120; }
    if (why) { snprintf(lui_msg, sizeof lui_msg, "%s", why); lui = LUI_MSG; }
    full_redraws_left = 4;
}
/* online presence: listed while the app is open (and the player allows it);
 * busy while playing a solo game or linked; invitations pop up when free */
static void presence_service(void) {
    int want = show_online && rom_ready && !import_mode;
    if (want) net_presence_start(app.act->vm, server_host);
    else if (net_kind() != NET_KIND_ONLINE) net_presence_stop();
    int busy = linked || vs_cpu || (net_kind() != NET_KIND_NONE && net_kind() != NET_KIND_ONLINE)
               || tetris_single_busy() || lui == LUI_CTL || lui == LUI_CPU || insert_anim_t0 != 0;
    net_presence_set_busy(busy);
    int inv = net_invite_pending();
    if (inv && busy && lui != LUI_INVITE) { net_invite_answer(0); inv = 0; }     /* never interrupt a game */
    if (inv && !linked && (lui == LUI_NONE || lui == LUI_MENU || lui == LUI_MSG)) { lui = LUI_INVITE; full_redraws_left = 4; }
    if (!inv && lui == LUI_INVITE) { snprintf(lui_msg, sizeof lui_msg, "THE INVITATION EXPIRED"); lui = LUI_MSG; full_redraws_left = 4; }
    if (lui == LUI_INVITE || lui == LUI_MENU || lui == LUI_SEARCH) full_redraws_left = 2;   /* live counters */
}
/* connection housekeeping, once per frame */
static void link_service(int64_t t) {
    if (!vs_cpu && net_kind() != NET_KIND_NONE) {
        net_poll(t);
        int st = net_state();
        if (!linked && st == NET_CONNECTED) link_begin();
        else if (linked && lk.peer_bye) unlink_session("OTHER PLAYER LEFT");
        else if (linked && st == NET_FAILED) unlink_session(net_status_text()[0] ? net_status_text() : "LINK LOST");
        else if (linked && link_silence_ns(&lk, t) > 6000000000LL) unlink_session("LINK LOST");
    }
    if (unlink_check_frames > 0 && --unlink_check_frames == 0 && tetris_is_2player()) {
        /* the soft reset can't run while the game waits on the cable: power-cycle instead */
        gb_init(tetris_rom, sizeof tetris_rom);
    }
}
static void sleep_until(int64_t due) {
    int64_t w = due - now_ns();
    if (w <= 0) return;
    struct timespec ts = { (time_t)(w / 1000000000LL), (long)(w % 1000000000LL) };
    nanosleep(&ts, NULL);
}
/* VS CPU: both Game Boys advance slice by slice, like two real ones on a cable.
 * Nothing goes over a network, so there's no need to sleep between slices: run
 * flat out until the player's Game Boy finishes a frame and let the sound
 * output pace the loop, exactly like single-player. Only while the player's
 * Game Boy is held waiting on the cable (menus) do we pace with the clock, so
 * the bot's Game Boy doesn't race ahead. */
static int run_vs_cpu_frame(void) {
    static int16_t tmp[4096 * 2];
    const unsigned slice_cyc = 70224 / 16;
    const int64_t slice_ns = 16742706LL / 16;
    int frame_done = 0;
    int64_t t0 = now_ns();
    for (int s = 1; s < 48 && !frame_done; s++) {
        int64_t t = now_ns();
        loop_pump(t);
        /* the bot's Game Boy */
        if (!(b_gb_link_master_waiting() && b_tetris_link_hold_while_waiting())) {
            uint64_t target = b_gb_cycles() + slice_cyc;
            for (int g = 0; g < 8 && b_gb_cycles() < target; g++) {
                if (b_gb_link_master_waiting() && b_tetris_link_hold_while_waiting()) break;
                if (b_gb_run_cycles((unsigned)(target - b_gb_cycles()))) {
#ifdef SIM_PROFILE
                    int64_t pb = now_ns();
#endif
                    b_gb_set_buttons(cpubot_frame(&cbot));
#ifdef SIM_PROFILE
                    prof_bot(now_ns() - pb);
#endif
                }
            }
            while (b_gb_audio_read(tmp, 4096) > 0) {}
        }
        loop_pump(now_ns());
        /* the player's Game Boy */
        int held = gb_link_master_waiting() && tetris_link_hold_while_waiting();
        if (!held) {
            uint64_t target = gb_cycles() + slice_cyc;
            for (int g = 0; g < 8 && gb_cycles() < target; g++) {
                if (gb_link_master_waiting() && tetris_link_hold_while_waiting()) { held = 1; break; }
                if (gb_run_cycles((unsigned)(target - gb_cycles()))) { frame_done = 1; break; }
            }
        }
        if (held && !frame_done) sleep_until(t0 + (int64_t)((double)s * (double)slice_ns));
    }
    loop_pump(now_ns());
    return frame_done;
}
/* one Game Boy frame in small slices, polling the network between them so a
 * byte from the other phone is answered within ~1 ms instead of a frame */
static int run_linked_frame(void) {
    if (vs_cpu) return run_vs_cpu_frame();
    int frame_done = 0;
    const int64_t slice_ns = 16742706LL / 16;
    int64_t t0 = now_ns();
    for (int s = 1; s < 48; s++) {
        link_pump(now_ns());
        /* menus: the master Game Boy waits while its byte is on the wire */
        int held = 0;
        while (gb_link_master_waiting() && tetris_link_hold_while_waiting() && linked) {
            if (now_ns() - t0 > 250000000LL) { held = 1; break; }   /* keep the screen alive */
            net_wait(1000000);
            link_pump(now_ns());
            if (net_state() == NET_FAILED) break;
        }
        if (held) break;
        if (gb_run_cycles(70224 / 16)) { frame_done = 1; break; }
        int64_t due = t0 + (int64_t)((double)s * (double)slice_ns * 0.96);
        int64_t w = due - now_ns();
        if (w > 0) net_wait(w);
    }
    link_pump(now_ns());
    return frame_done;
}


/* ------------------------------------------------------------------ */
/* first run: the player's own ROM                                     */
/* ------------------------------------------------------------------ */
static char rom_msg[48];
static int rom_perm_wait;              /* waiting for the storage permission to scan Downloads */
static int64_t rom_perm_since, rom_next_poll, import_finish_at;
static const char *ROM_DIRS[] = { "/storage/emulated/0/Download", "/storage/emulated/0/Documents", "/sdcard/Download" };


/* ------------------------------------------------------------------ */
/* the one-time "cartridge goes in" animation after a ROM is loaded    */
/* ------------------------------------------------------------------ */
/* insert_anim_t0: 0 = not playing (declared above) */
#define INSERT_ANIM_NS 2700000000LL
static void boot_game(void);
static void start_insert_anim(void) {
    insert_anim_t0 = now_ns();
    lui = LUI_NONE;
    full_redraws_left = 4;
}
static void finish_insert_anim(void) {
    if (!insert_anim_t0) return;
    insert_anim_t0 = 0;
    boot_game();
}
static float ease_out(float x) { if (x < 0) x = 0; if (x > 1) x = 1; return 1.f - (1.f - x) * (1.f - x) * (1.f - x); }
static void draw_insert_anim(Canvas *c, int64_t t) {
    static const char *cart[] = {
        "+-------------+",
        "|  _________  |",
        "| +---------+ |",
        "| |         | |",
        "| |  YOUR   | |",
        "| |   GAME  | |",
        "| |         | |",
        "| +---------+ |",
        "|  #  #  #  # |",
        "+-------------+",
        "  |||||||||||  ",
    };
    const int ncart = (int)(sizeof cart / sizeof cart[0]);
    Rc r = L.screen;
    fill(c, r.x, r.y, r.w, r.h, shade[0]);
    float e = (float)(t - insert_anim_t0) / 1e9f;
    int sc = r.w / (21 * 6); if (sc < 1) sc = 1;
    int lh = 8 * sc;
    int cx = r.x + r.w / 2;
    /* the slot near the bottom of the screen */
    int slot_y = r.y + r.h - r.h / 4;
    int seat_top = slot_y - (ncart - 1) * lh + lh / 2;          /* contacts end up inside the slot */
    int start_top = r.y - ncart * lh;
    float k = ease_out(e / 1.1f);
    int shake = (e > 1.1f && e < 1.25f) ? (int)(sc * ((int)(e * 60) % 2 ? 1 : -1)) : 0;
    int top = start_top + (int)((float)(seat_top - start_top) * k) + shake;
    for (int i = 0; i < ncart; i++) {
        int y = top + i * lh;
        if (y < r.y || y + 7 * sc > slot_y + lh / 2) continue;      /* clipped by the screen top and the slot */
        text_c(c, cx, y, cart[i], sc, shade[3]);
    }
    text_c(c, cx, slot_y, "=[_______________]=", sc, shade[2]);
    if (e > 1.1f && e < 1.6f) text_c(c, cx, slot_y + 2 * lh, "CLICK!", sc, shade[3]);
    if (e > 1.35f) {
        int on = e > 1.5f || ((int)(e * 20) % 2);
        text(c, r.x + 2 * sc * 6, r.y + lh, on ? "POWER *" : "POWER", sc, shade[e > 1.5f ? 3 : 2]);
    }
    if (e > 1.6f) {
        char msg[32]; int dots = (int)((e - 1.6f) / 0.15f); if (dots > 3) dots = 3;
        snprintf(msg, sizeof msg, "READING ROM%.*s%s", dots, "...", e > 2.2f ? " OK" : "");
        text_c(c, cx, slot_y + 2 * lh, msg, sc, shade[3]);
    }
}

static void boot_game(void) {
    memset(pending, 0, sizeof pending);
    memset(&macro, 0, sizeof macro);
    gb_init(tetris_rom, sizeof tetris_rom);
    if (!load_state()) gb_init(tetris_rom, sizeof tetris_rom);
    rom_ready = 1;
    lui = LUI_NONE;
    hint_until = now_ns() + 20000000000LL;
    full_redraws_left = 4;
}
static void rom_set_msg(int r) {
    snprintf(rom_msg, sizeof rom_msg, "%s",
             r == ROM_WRONG_VERSION ? "THAT TETRIS IS A DIFFERENT VERSION" :
             r == ROM_UNREADABLE ? "COULDN'T READ THAT FILE" :
             r == ROM_OK ? "ROM LOADED" : "NO TETRIS ROM FOUND THERE");
}
/* a verified ROM in tetris_rom: keep a private copy, then start */
static void rom_accept(void) {
    rom_save(app.rom_path, tetris_rom);
    if (import_mode) {
        snprintf(rom_msg, sizeof rom_msg, "ROM SAVED - STARTING");
        import_finish_at = now_ns() + 900000000LL;
        full_redraws_left = 4;
    } else start_insert_anim();
}
static void rom_scan_now(void) {
    int r = rom_scan((const char *const *)ROM_DIRS, (int)(sizeof ROM_DIRS / sizeof ROM_DIRS[0]), tetris_rom);
    if (r == ROM_OK) rom_accept();
    else { rom_set_msg(r); if (r == ROM_NOT_FOUND) snprintf(rom_msg, sizeof rom_msg, "NOT FOUND IN DOWNLOADS"); }
    full_redraws_left = 4;
}
static void rom_find_tap(void) {
#ifdef __ANDROID__
    if (!rom_have_storage_permission(app.act->vm, app.act->clazz)) {
        rom_request_storage_permission(app.act->vm, app.act->clazz);
        rom_perm_wait = 1; rom_perm_since = now_ns();
        snprintf(rom_msg, sizeof rom_msg, "ALLOW FILE ACCESS TO SEARCH");
        full_redraws_left = 4;
        return;
    }
#endif
    rom_scan_now();
}
/* once per frame while there's no ROM */
static void rom_service(int64_t t) {
    if (insert_anim_t0) { if (t - insert_anim_t0 > INSERT_ANIM_NS) finish_insert_anim(); return; }
    if (import_mode) {
        if (import_finish_at && t > import_finish_at) {
            import_finish_at = 0;
#ifdef __ANDROID__
            rom_launch_main(app.act->vm, app.act->clazz);
#endif
            ANativeActivity_finish(app.act);
        }
        return;
    }
    if (rom_perm_wait) {
#ifdef __ANDROID__
        if (rom_have_storage_permission(app.act->vm, app.act->clazz)) { rom_perm_wait = 0; rom_scan_now(); }
        else if (t - rom_perm_since > 60000000000LL) rom_perm_wait = 0;
#endif
    }
    if (t > rom_next_poll) {           /* imported by an "Open with" window meanwhile? */
        rom_next_poll = t + 500000000LL;
        if (rom_check_file(app.rom_path, tetris_rom) == ROM_OK) start_insert_anim();
    }
}
static void rom_tap(float x, float y) {
    if (import_mode) {
        if (!import_finish_at && in_rc(O.b6, x, y)) ANativeActivity_finish(app.act);
        return;
    }
    if (in_rc(O.b3, x, y)) rom_find_tap();
#ifdef __ANDROID__
    else if (in_rc(O.b4, x, y)) rom_open_files_app(app.act->vm, app.act->clazz);
#endif
}
static void draw_rom_panel(Canvas *c, Rc in, int cx, int bts) {
    int ms = bts;
    const char *l1 = "THE GAME ISN'T INCLUDED.", *l2 = "LOAD YOUR OWN COPY OF", *l3 = "TETRIS (WORLD) (REV A)";
    const char *h1 = "OR OPEN / SHARE YOUR .GB", *h2 = "OR .ZIP WITH GREENSCREEN '89";
    const char *w[] = { l1, l2, l3, h1, h2, rom_msg };
    for (int i = 0; i < 6; i++) while (ms > 1 && text_w(w[i], ms) > in.w - 16) ms--;
    int lh = 10 * ms;
    if (import_mode) {
        text_c(c, cx, O.b2.y, rom_msg[0] ? rom_msg : "CHECKING...", ms, C_TEXT);
        if (!import_finish_at && rom_msg[0]) draw_button(c, O.b6, "OK", 0, bts);
        return;
    }
    text_c(c, cx, O.b1.y + lh / 4, l1, ms, C_TEXT);
    text_c(c, cx, O.b1.y + lh * 7 / 4, l2, ms, C_TEXT);
    text_c(c, cx, O.b1.y + lh * 11 / 4, l3, ms, C_TEXT);
    draw_button(c, O.b3, "FIND IN DOWNLOADS", 0, bts);
    draw_button(c, O.b4, "OPEN FILES APP", 0, bts);
    text_c(c, cx, O.b5.y + 1 * ms, h1, ms, C_DIM);
    text_c(c, cx, O.b5.y + 1 * ms + lh, h2, ms, C_DIM);
    if (rom_msg[0]) text_c(c, cx, O.b6.y + O.b6.h / 2, rom_msg, ms, C_TEXT);
}


/* ------------------------------------------------------------------ */
/* CONTROLS: touch / hard-drop sensitivity                             */
/* ------------------------------------------------------------------ */
static void open_controls(void) {
    if (in_game_unpaused() && !linked) queue_press(BTN_START);   /* pause a solo game while adjusting */
    lui = LUI_CTL;
    full_redraws_left = 4;
}
/* a slider row: [-] bar [+]; returns the parts */
static void slider_parts(Rc row, Rc *minus, Rc *bar, Rc *plus) {
    int bw = row.h;
    *minus = (Rc){ row.x, row.y, bw, row.h };
    *plus = (Rc){ row.x + row.w - bw, row.y, bw, row.h };
    *bar = (Rc){ row.x + bw + bw / 3, row.y + row.h / 3, row.w - 2 * bw - 2 * (bw / 3), row.h / 3 };
}
static int slider_tap(Rc row, float x, float y, int *v, int lo, int hi) {
    Rc mi, bar, pl; slider_parts(row, &mi, &bar, &pl);
    int old = *v;
    if (in_rc(mi, x, y)) { if (*v > lo) (*v)--; }
    else if (in_rc(pl, x, y)) { if (*v < hi) (*v)++; }
    else if (y >= row.y && y < row.y + row.h && x >= bar.x - bar.h && x <= bar.x + bar.w + bar.h) {
        float f = (x - bar.x) / (float)(bar.w > 1 ? bar.w : 1);
        int nv = lo + (int)(f * (hi - lo) + 0.5f);
        *v = nv < lo ? lo : nv > hi ? hi : nv;
    }
    return *v != old;
}
static void draw_slider(Canvas *c, Rc row, int v, int lo, int hi, int ts) {
    Rc mi, bar, pl; slider_parts(row, &mi, &bar, &pl);
    draw_button(c, mi, "-", 0, ts);
    draw_button(c, pl, "+", 0, ts);
    rrect(c, bar, bar.h / 2, C_BTN);
    Rc on = bar; on.w = (int)((float)bar.w * (float)(v - lo) / (float)(hi - lo));
    if (on.w > 0) rrect(c, on, bar.h / 2, C_TEXT);
    int kx = bar.x + on.w, kr = bar.h;
    Rc knob = { kx - kr, bar.y + bar.h / 2 - kr, 2 * kr, 2 * kr };
    rrect(c, knob, kr, C_TEXT);
}
static void ctl_tap(float x, float y) {
    int changed = 0;
    changed |= slider_tap(O.b2, x, y, &sens_touch, 1, SENS_MAX);
    changed |= slider_tap(O.b4, x, y, &sens_flick, 0, SENS_MAX);
    Rc reset = O.b6, done = O.b6;
    reset.w = O.b6.w / 2 - O.b6.h / 6; done.x = O.b6.x + O.b6.w - reset.w; done.w = reset.w;
    if (in_rc(O.b5, x, y)) { show_online = !show_online; changed = 1; }
    if (in_rc(reset, x, y)) { sens_touch = 5; sens_flick = 5; changed = 1; }
    else if (in_rc(done, x, y)) { save_settings(); lui = LUI_NONE; full_redraws_left = 4; return; }
    if (changed) { save_settings(); full_redraws_left = 4; }
}
static void draw_ctl_panel(Canvas *c, Rc in, int cx, int bts) {
    char l1[40], l2[40], l3[48];
    snprintf(l1, sizeof l1, "SWIPE SENSITIVITY  %d", sens_touch);
    if (sens_flick) snprintf(l2, sizeof l2, "HARD DROP FLICK  %d", sens_flick);
    else snprintf(l2, sizeof l2, "HARD DROP FLICK  OFF");
    snprintf(l3, sizeof l3, "SPEED %d%%  LOAD %.1f MS", (int)(perf_speed + 0.5f), perf_ms);
    int ms = bts;
    const char *w[] = { l1, l2, l3 };
    for (int i = 0; i < 3; i++) while (ms > 1 && text_w(w[i], ms) > in.w - 16) ms--;
    text_c(c, cx, O.b1.y + O.b1.h / 2 - 3 * ms, l1, ms, C_TEXT);
    draw_slider(c, O.b2, sens_touch, 1, SENS_MAX, bts);
    text_c(c, cx, O.b3.y + O.b3.h / 2 - 3 * ms, l2, ms, C_TEXT);
    draw_slider(c, O.b4, sens_flick, 0, SENS_MAX, bts);
    draw_button(c, O.b5, show_online ? "SHOW ME ONLINE: YES" : "SHOW ME ONLINE: NO", show_online, bts);
    text_c(c, cx, O.b6.y + O.b6.h + 2 * ms, l3, ms, C_DIM);
    Rc reset = O.b6, done = O.b6;
    reset.w = O.b6.w / 2 - O.b6.h / 6; done.x = O.b6.x + O.b6.w - reset.w; done.w = reset.w;
    draw_button(c, reset, "RESET", 0, bts);
    draw_button(c, done, "DONE", 0, bts);
}

static void overlay_layout(void) {
    Rc r = L.screen;
    int m = r.w / 12;
    O.panel = (Rc){ r.x + m, r.y + r.h / 16, r.w - 2 * m, r.h - r.h / 8 };
    int bh = O.panel.h / 9, gap = bh / 5, bw = O.panel.w - 2 * m, bx = O.panel.x + m;
    O.b1 = (Rc){ bx, O.panel.y + O.panel.h / 6, bw, bh };
    O.b2 = (Rc){ bx, O.b1.y + bh + gap, bw, bh };
    O.b3 = (Rc){ bx, O.b2.y + bh + gap, bw, bh };
    O.b4 = (Rc){ bx, O.b3.y + bh + gap, bw, bh };
    O.b5 = (Rc){ bx, O.b4.y + bh + gap, bw, bh };
    O.b6 = (Rc){ bx, O.b5.y + bh + gap, bw, bh };
}

static void overlay_tap(float x, float y) {
    overlay_layout();
    if (lui == LUI_ROM) { rom_tap(x, y); return; }
    if (lui == LUI_CTL) { ctl_tap(x, y); return; }
    if (lui == LUI_INVITE) {
        if (in_rc(O.b4, x, y)) { net_invite_answer(1); lui = LUI_SEARCH; }
        else if (in_rc(O.b6, x, y)) { net_invite_answer(0); lui = LUI_NONE; }
        full_redraws_left = 4;
        return;
    }
    if (lui == LUI_MENU) {
        if (in_rc(O.b1, x, y)) lui = LUI_CPU;
        else if (in_rc(O.b2, x, y)) { net_start_lan(); lui = LUI_SEARCH; }
        else if (in_rc(O.b3, x, y)) { net_start_usb(app.act->vm, app.act->clazz); lui = LUI_SEARCH; }
        else if (in_rc(O.b4, x, y)) { net_start_online(app.act->vm, server_host); lui = LUI_SEARCH; }
        else if (in_rc(O.b5, x, y)) {
            char clip[256], host[128];
            if (net_clipboard_text(app.act->vm, app.act->clazz, clip, sizeof clip) == 0 && net_parse_host(clip, host, sizeof host) == 0
                && strcmp(host, server_host) != 0) {
                snprintf(server_host, sizeof server_host, "%s", host);
                save_server();
                snprintf(lui_msg, sizeof lui_msg, "SERVER SAVED");
            } else if (server_host[0]) {
                server_host[0] = 0;
                save_server();
                snprintf(lui_msg, sizeof lui_msg, "BACK TO PUBLIC SERVERS");
            } else snprintf(lui_msg, sizeof lui_msg, "COPY A SERVER ADDRESS FIRST");
            lui = LUI_MSG;
        }
        else if (in_rc(O.b6, x, y)) lui = LUI_NONE;
    } else if (lui == LUI_CPU) {
        if (in_rc(O.b2, x, y)) start_vs_cpu(0);
        else if (in_rc(O.b3, x, y)) start_vs_cpu(1);
        else if (in_rc(O.b4, x, y)) start_vs_cpu(2);
        else if (in_rc(O.b6, x, y)) lui = LUI_MENU;
    } else if (lui == LUI_SEARCH) {
        if (in_rc(O.b6, x, y)) { net_stop(); lui = LUI_NONE; }
    } else if (lui == LUI_MSG) {
        if (in_rc(O.b6, x, y) || in_rc(O.panel, x, y)) lui = LUI_NONE;
    }
}

static void draw_overlay(Canvas *c, int tsc) {
    overlay_layout();
    rrect(c, O.panel, O.panel.w / 16, C_BEZEL);
    Rc in = { O.panel.x + 3, O.panel.y + 3, O.panel.w - 6, O.panel.h - 6 };
    rrect(c, in, O.panel.w / 16, C_BG);
    const char *title = lui == LUI_ROM ? "YOUR TETRIS ROM" : lui == LUI_CTL ? "CONTROLS" : "2 PLAYER LINK";
    int ts = tsc;
    while (ts > 1 && text_w(title, ts) > in.w - 20) ts--;
    int cx = O.panel.x + O.panel.w / 2;
    text_c(c, cx, O.panel.y + O.panel.h / 14, title, ts, C_TEXT);
    int bts = ts > 4 ? 4 : ts;
    if (lui == LUI_ROM) { draw_rom_panel(c, in, cx, bts); return; }
    if (lui == LUI_CTL) { draw_ctl_panel(c, in, cx, bts); return; }
    if (lui == LUI_INVITE) {
        int ms = bts;
        const char *l1 = "SOMEONE ONLINE WANTS", *l2 = "TO PLAY 2-PLAYER!";
        while (ms > 1 && text_w(l1, ms) > in.w - 16) ms--;
        text_c(c, cx, O.b1.y + O.b1.h / 2, l1, ms, C_TEXT);
        text_c(c, cx, O.b1.y + O.b1.h / 2 + 10 * ms, l2, ms, C_TEXT);
        char cd[32]; snprintf(cd, sizeof cd, "ANSWER IN %d", net_invite_pending());
        text_c(c, cx, O.b3.y + O.b3.h / 4, cd, ms, C_DIM);
        draw_button(c, O.b4, "ACCEPT", 1, bts);
        draw_button(c, O.b6, "DECLINE", 0, bts);
        return;
    }
    if (lui == LUI_MENU) {
        draw_button(c, O.b1, "VS CPU", 0, bts);
        draw_button(c, O.b2, "SAME WI-FI", 0, bts);
        draw_button(c, O.b3, "USB CABLE", 0, bts);
        {   /* ONLINE, with who's free right now */
            int n = net_presence_available();
            char lab[40];
            if (n < 0) snprintf(lab, sizeof lab, show_online ? "ONLINE - CONNECTING" : "ONLINE");
            else if (n == 0) snprintf(lab, sizeof lab, "ONLINE - NO ONE FREE");
            else snprintf(lab, sizeof lab, "ONLINE - %d FREE", n);
            draw_button(c, O.b4, lab, 0, bts);
            if (n >= 0) {
                int d = O.b4.h / 3;
                Rc dot = { O.b4.x + O.b4.h / 2 - d / 2, O.b4.y + O.b4.h / 2 - d / 2, d, d };
                rrect(c, dot, d / 2, n > 0 ? C_GREEN : C_DIM);
            }
        }
        draw_button(c, O.b5, server_host[0] ? "PUBLIC SERVERS" : "CUSTOM SERVER", 0, bts);
        draw_button(c, O.b6, "CANCEL", 0, bts);
    } else if (lui == LUI_CPU) {
        int ms = bts;
        while (ms > 1 && text_w("CHOOSE OPPONENT", ms) > in.w - 20) ms--;
        text_c(c, cx, O.b1.y + O.b1.h / 2 - 3 * ms, "CHOOSE OPPONENT", ms, C_DIM);
        draw_button(c, O.b2, "EASY", 0, bts);
        draw_button(c, O.b3, "NORMAL", 0, bts);
        draw_button(c, O.b4, "HARD", 0, bts);
        draw_button(c, O.b6, "BACK", 0, bts);
    } else {
        const char *msg = lui == LUI_MSG ? lui_msg : net_status_text();
        if (lui == LUI_SEARCH && net_state() == NET_FAILED) msg = net_status_text();
        int ms = bts;
        while (ms > 1 && text_w(msg, ms) > in.w - 20) ms--;
        text_c(c, cx, O.b1.y + O.b1.h / 2, msg, ms, C_TEXT);
        if (lui == LUI_SEARCH && net_state() != NET_FAILED) {
            /* little activity dots */
            int k = (int)((now_ns() / 300000000LL) % 4);
            for (int i = 0; i < 3; i++)
                fill(c, cx - 4 * ms * 3 / 2 + i * 4 * ms, O.b2.y + O.b2.h / 3, ms * 2, ms * 2, i < k ? C_TEXT : C_BTN);
        }
        if (lui == LUI_MSG && !strcmp(lui_msg, "SERVER SAVED")) {
            int hs = ms; while (hs > 1 && text_w(server_host, hs) > in.w - 20) hs--;
            text_c(c, cx, O.b2.y + O.b2.h / 2, server_host, hs, C_DIM);
        }
        if (lui == LUI_MSG && !strcmp(lui_msg, "COPY A SERVER ADDRESS FIRST")) {
            const char *l1 = "AN MQTT BROKER (HOST:PORT, TLS)", *l2 = "THEN TAP CUSTOM SERVER";
            int hs = ms; while (hs > 1 && text_w(l1, hs) > in.w - 20) hs--;
            text_c(c, cx, O.b2.y + O.b2.h / 2, l1, hs, C_DIM);
            text_c(c, cx, O.b3.y + O.b3.h / 2, l2, hs, C_DIM);
        }
        draw_button(c, O.b6, lui == LUI_SEARCH && net_state() != NET_FAILED ? "CANCEL" : "OK", 0, bts);
    }
}

/* ------------------------------------------------------------------ */
/* game thread                                                         */
/* ------------------------------------------------------------------ */
#define LOOPER_ID_INPUT 1

/* drain the looper; input events are read as soon as their fd fires so the
 * fd stops being readable (otherwise pollOnce would keep returning it) */
static void pump_looper(void) {
    int ev, id, guard = 0;
    while ((id = ALooper_pollOnce(0, NULL, &ev, NULL)) >= 0 && guard++ < 64) {
        if (id == LOOPER_ID_INPUT) process_input();
    }
    process_input();
}

static void *game_thread(void *arg) {
    (void)arg;
    app.looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    load_server();
    load_settings();
    rom_ready = 0; import_mode = 0; rom_msg[0] = 0; rom_perm_wait = 0; import_finish_at = 0; rom_next_poll = 0;
#ifdef __ANDROID__
    if (rom_intent_has_file(app.act->vm, app.act->clazz)) {
        /* opened with a file: import it, then hand over to the normal app window */
        import_mode = 1;
        lui = LUI_ROM;
        int r = rom_from_intent(app.act->vm, app.act->clazz, tetris_rom);
        if (r == ROM_OK) rom_accept(); else rom_set_msg(r);
    } else
#endif
    if (rom_check_file(app.rom_path, tetris_rom) == ROM_OK) boot_game();
    else lui = LUI_ROM;
    int was_active = 0;
    pthread_mutex_lock(&app.mu);
    app.running = 1;
    pthread_cond_broadcast(&app.cv);
    for (;;) {
        /* service requests from the UI thread */
        if (app.win_req) {
            app.win = app.win_new;
            if (app.win) {
                ANativeWindow_setBuffersGeometry(app.win, 0, 0, WINDOW_FORMAT_RGBX_8888);
                L.W = L.H = 0;
            }
            app.win_req = 0;
            pthread_cond_broadcast(&app.cv);
        }
        if (app.q_req) {
            if (app.queue) AInputQueue_detachLooper(app.queue);
            app.queue = app.q_new;
            if (app.queue) AInputQueue_attachLooper(app.queue, app.looper, LOOPER_ID_INPUT, NULL, NULL);
            app.q_req = 0;
            pthread_cond_broadcast(&app.cv);
        }
        int active = app.resumed && app.win && !app.destroy;
        if (was_active && !active) {
            net_presence_stop();
            if (linked && net_kind() == NET_KIND_ONLINE) unlink_session("LINK ENDED");
            if (astream) AAudioStream_requestPause(astream);
            for (int i = 0; i < MAXP; i++) ptrs[i].active = 0;
            key_held = 0;
        }
        if (app.save_req) {
            save_state();
            app.save_req = 0;
            pthread_cond_broadcast(&app.cv);
        }
        if (app.destroy) break;
        if (!active) {
            was_active = 0;
            /* still drain input so the system doesn't think we're hung */
            pthread_mutex_unlock(&app.mu);
            pump_looper();
            pthread_mutex_lock(&app.mu);
            if (!(app.win_req || app.q_req || app.save_req || app.destroy || (app.resumed && app.win))) {
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&app.cv, &app.mu, &ts);
            }
            continue;
        }
        if (!was_active) {
            was_active = 1;
            if (!astream) audio_open(); else AAudioStream_requestStart(astream);
            audio_last_progress_ns = 0;
            /* coming back mid-game: pause so the piece doesn't fall unseen */
            if (rom_ready && in_game_unpaused() && !tetris_is_2player()) queue_press(BTN_START);
            full_redraws_left = 4;
        }
        pthread_mutex_unlock(&app.mu);

        pump_looper();
        if (!rom_ready) {
            rom_service(now_ns());
            render();
            struct timespec ts = { 0, 16000000 }; nanosleep(&ts, NULL);
            pthread_mutex_lock(&app.mu);
            continue;
        }
        presence_service();
        link_service(now_ns());
        check_reset_hold(now_ns());
        /* new buttons only once the Game Boy has finished the previous frame,
         * so a one-frame tap is never lost while the link holds the game */
        static int need_buttons = 1;
        struct timespec cpu0, cpu1; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu0);
        if (need_buttons) gb_set_buttons(frame_buttons());
        if (linked) need_buttons = run_linked_frame(); else { gb_run_frame(); need_buttons = 1; }
        {
            static int prev_fall;
            int fall = gb_peek(0xFF98);
            if (fall && !prev_fall) lock_count++;
            prev_fall = fall;
        }
        render();
        audio_service(now_ns());
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu1);
        {   /* SPEED / LOAD readout: emulated frames per real second, and CPU time per loop */
            int64_t t = now_ns();
            perf_work_ns += (int64_t)(cpu1.tv_sec - cpu0.tv_sec) * 1000000000LL + (cpu1.tv_nsec - cpu0.tv_nsec);
            perf_loops++;
            if (need_buttons) perf_frames++;
            if (!perf_t0) perf_t0 = t;
            if (t - perf_t0 > 1000000000LL) {
                perf_speed = (float)perf_frames / ((float)(t - perf_t0) / 1e9f) / 59.7275f * 100.f;
                perf_ms = (float)perf_work_ns / 1e6f / (float)(perf_loops ? perf_loops : 1);
                perf_t0 = t; perf_frames = 0; perf_loops = 0; perf_work_ns = 0;
                if (lui == LUI_CTL) full_redraws_left = 2;
            }
        }
        audio_push_and_pace();

        pthread_mutex_lock(&app.mu);
    }
    if (app.queue) { AInputQueue_detachLooper(app.queue); app.queue = NULL; }
    app.win = NULL;
    app.running = 0;
    pthread_cond_broadcast(&app.cv);
    pthread_mutex_unlock(&app.mu);
    audio_close();
    gb_link_attach(NULL, NULL);
    net_presence_stop();
    net_stop();
    net_thread_exit();
    linked = 0;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* NativeActivity callbacks (UI thread)                                */
/* ------------------------------------------------------------------ */
static void wait_until(int *flag) {
    while (*flag && app.running) pthread_cond_wait(&app.cv, &app.mu);
}

static void set_window(ANativeWindow *w) {
    pthread_mutex_lock(&app.mu);
    app.win_new = w; app.win_req = 1;
    pthread_cond_broadcast(&app.cv);
    wait_until(&app.win_req);
    pthread_mutex_unlock(&app.mu);
}
static void set_queue(AInputQueue *q) {
    pthread_mutex_lock(&app.mu);
    app.q_new = q; app.q_req = 1;
    pthread_cond_broadcast(&app.cv);
    wait_until(&app.q_req);
    pthread_mutex_unlock(&app.mu);
}

static void onResume(ANativeActivity *a) { (void)a; pthread_mutex_lock(&app.mu); app.resumed = 1; pthread_cond_broadcast(&app.cv); pthread_mutex_unlock(&app.mu); }
static void onPause(ANativeActivity *a) {
    (void)a;
    pthread_mutex_lock(&app.mu);
    app.resumed = 0; app.save_req = 1;
    pthread_cond_broadcast(&app.cv);
    /* wait (bounded) for the state to be written */
    for (int i = 0; i < 20 && app.save_req && app.running; i++) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&app.cv, &app.mu, &ts);
    }
    pthread_mutex_unlock(&app.mu);
}
static void onStop(ANativeActivity *a) { (void)a; }
static void onStart(ANativeActivity *a) { (void)a; }
static void *onSaveInstanceState(ANativeActivity *a, size_t *n) { (void)a; *n = 0; return NULL; }
static void onWindowFocusChanged(ANativeActivity *a, int f) { (void)a; (void)f; }
static void onWindowCreated(ANativeActivity *a, ANativeWindow *w) { (void)a; set_window(w); }
static void onWindowDestroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; set_window(NULL); }
static void onWindowResized(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; }
static void onRedrawNeeded(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; full_redraws_left = 4; }
static void onQueueCreated(ANativeActivity *a, AInputQueue *q) { (void)a; set_queue(q); }
static void onQueueDestroyed(ANativeActivity *a, AInputQueue *q) { (void)a; (void)q; set_queue(NULL); }
static void onContentRectChanged(ANativeActivity *a, const ARect *r) {
    (void)a;
    pthread_mutex_lock(&app.mu);
    app.content = *r; app.have_content = 1; L.W = L.H = 0;  /* force relayout */
    pthread_mutex_unlock(&app.mu);
}
static void onConfigurationChanged(ANativeActivity *a) {
    AConfiguration *cfg = AConfiguration_new();
    AConfiguration_fromAssetManager(cfg, a->assetManager);
    int d = AConfiguration_getDensity(cfg);
    AConfiguration_delete(cfg);
    pthread_mutex_lock(&app.mu);
    if (d > 0 && d < 2000) dpi = (float)d;
    L.W = L.H = 0;
    pthread_mutex_unlock(&app.mu);
}
static void onLowMemory(ANativeActivity *a) { (void)a; }
static void onDestroy(ANativeActivity *a) {
    (void)a;
    pthread_mutex_lock(&app.mu);
    app.destroy = 1;
    pthread_cond_broadcast(&app.cv);
    pthread_mutex_unlock(&app.mu);
    pthread_join(app.th, NULL);
    pthread_mutex_destroy(&app.mu);
    pthread_cond_destroy(&app.cv);
}

__attribute__((visibility("default")))
void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_size) {
    (void)saved; (void)saved_size;
    memset(&app, 0, sizeof app);
    memset(&L, 0, sizeof L);
    memset(ptrs, 0, sizeof ptrs);
    memset(pending, 0, sizeof pending);
    pulse_prev = 0; key_held = 0; back_time = 0; hard_drop_req = 0; hard_drop_hold = 0; memset(&macro, 0, sizeof macro);
    hint_until = now_ns() + 20000000000LL;
    app.act = a;
    a->callbacks->onStart = onStart;
    a->callbacks->onResume = onResume;
    a->callbacks->onSaveInstanceState = onSaveInstanceState;
    a->callbacks->onPause = onPause;
    a->callbacks->onStop = onStop;
    a->callbacks->onDestroy = onDestroy;
    a->callbacks->onWindowFocusChanged = onWindowFocusChanged;
    a->callbacks->onNativeWindowCreated = onWindowCreated;
    a->callbacks->onNativeWindowResized = onWindowResized;
    a->callbacks->onNativeWindowRedrawNeeded = onRedrawNeeded;
    a->callbacks->onNativeWindowDestroyed = onWindowDestroyed;
    a->callbacks->onInputQueueCreated = onQueueCreated;
    a->callbacks->onInputQueueDestroyed = onQueueDestroyed;
    a->callbacks->onContentRectChanged = onContentRectChanged;
    a->callbacks->onConfigurationChanged = onConfigurationChanged;
    a->callbacks->onLowMemory = onLowMemory;
    ANativeActivity_setWindowFlags(a, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    if (a->internalDataPath)
    {
        snprintf(app.state_path, sizeof app.state_path, "%s/tetris_state.bin", a->internalDataPath);
        snprintf(app.rom_path, sizeof app.rom_path, "%s/rom.gb", a->internalDataPath);
    }
    {
        AConfiguration *cfg = AConfiguration_new();
        AConfiguration_fromAssetManager(cfg, a->assetManager);
        int d = AConfiguration_getDensity(cfg);
        AConfiguration_delete(cfg);
        if (d > 0 && d < 2000) dpi = (float)d;
    }
    pthread_mutex_init(&app.mu, NULL);
    pthread_cond_init(&app.cv, NULL);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_create(&app.th, &at, game_thread, NULL);
    pthread_attr_destroy(&at);
    /* wait for the thread to come up so requests can be acknowledged */
    pthread_mutex_lock(&app.mu);
    while (!app.running) pthread_cond_wait(&app.cv, &app.mu);
    pthread_mutex_unlock(&app.mu);
}
