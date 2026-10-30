/* Touch layer for Tetris's menus: taps on menu items become the same button
 * presses a player would make (cursor moves, then A), so the original code,
 * sounds and screen flow are untouched. Coordinates are Game Boy pixels. */
#include <stdint.h>
#include "gbcore.h"
#include "touchui.h"

#define hGameState   0xFFE1
#define hGamePaused  0xFFAB
#define hGameType    0xFFC0
#define hMusicType   0xFFC1
#define hATypeLevel  0xFFC2
#define hBTypeLevel  0xFFC3
#define hBTypeHigh   0xFFC4
#define hIs2Player   0xFFC5

static int in(int x, int y, int x0, int y0, int x1, int y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; }

static int linked;
void touchui_set_linked(int on) { linked = on; }

/* grid cell hit test: cols of 16px starting at x0, two rows starting near y0 */
static int grid_hit_y(int gx, int gy, int x0, int cols, int y0) {
    if (gx < x0 - 2 || gx >= x0 + 16 * cols + 2) return -1;
    int r;
    if (gy >= y0 - 8 && gy < y0 + 11) r = 0; else if (gy >= y0 + 11 && gy < y0 + 32) r = 1; else return -1;
    int c = (gx - x0) / 16;
    if (c < 0) c = 0;
    if (c >= cols) c = cols - 1;
    return r * cols + c;
}
static int grid_hit(int gx, int gy, int x0, int cols) { return grid_hit_y(gx, gy, x0, cols, 48); }

static void nav_grid(Macro *m, int cur, int tgt, int cols) {
    int dr = tgt / cols - cur / cols, dc = tgt % cols - cur % cols;
    for (; dr > 0; dr--) macro_push(m, BTN_DOWN);
    for (; dr < 0; dr++) macro_push(m, BTN_UP);
    for (; dc > 0; dc--) macro_push(m, BTN_RIGHT);
    for (; dc < 0; dc++) macro_push(m, BTN_LEFT);
}

static int game_hit(int gx, int gy) {
    if (gy < 30 || gy >= 60) return -1;
    if (gx >= 14 && gx < 80) return 0;
    if (gx >= 80 && gx < 146) return 1;
    return -1;
}
static int music_hit(int gx, int gy) {
    int r;
    if (gy >= 86 && gy < 106) r = 0; else if (gy >= 106 && gy < 128) r = 1; else return -1;
    if (gx >= 14 && gx < 80) return r * 2;
    if (gx >= 80 && gx < 146) return r * 2 + 1;
    return -1;
}

void macro_push(Macro *m, uint8_t mask) { if (m->n < MACRO_MAX) m->steps[m->n++] = mask; }

int touchui_menu_state(void) {
    switch (gb_peek(hGameState)) {
    case 0x07: case 0x0e: case 0x0f: case 0x11: case 0x13: case 0x14: case 0x15: case 0x24: case 0x25:
    case 0x2a: case 0x2b: case 0x16: case 0x17:
        return 1;
    }
    return 0;
}

#define hRole        0xFFCB      /* $29 master (Mario), $55 passive (Luigi) */
#define hHigh1       0xFFAC
#define hHigh2       0xFFAD

/* gx/gy: tap in Game Boy pixels, or -1 when outside the game screen.
 * returns TOUCHUI_OPEN_LINK when the player asked for 2-player without a link */
int touchui_tap(Macro *m, int gx, int gy) {
    int st = gb_peek(hGameState);
    switch (st) {
    case 0x24: case 0x25:                     /* copyright screen */
        macro_push(m, BTN_START);
        return 0;
    case 0x07: {                              /* title */
        int two = gx >= 0 && in(gx, gy, 88, 104, 160, 126);
        if (two && !linked) return TOUCHUI_OPEN_LINK;
        if (two && !gb_peek(hIs2Player)) macro_push(m, BTN_RIGHT);
        if (!two && gb_peek(hIs2Player)) macro_push(m, BTN_LEFT);
        macro_push(m, BTN_START);
        return 0;
    }
    case 0x2a: case 0x2b: {                   /* 2-player music (the master chooses) */
        if (gb_peek(hRole) != 0x29) return 0;
        int mu = gx >= 0 ? music_hit(gx, gy) : -1;
        int curm = gb_peek(hMusicType) - 0x1c;
        if (mu < 0 || mu == curm) macro_push(m, BTN_START);
        else nav_grid(m, curm, mu, 2);
        return 0;
    }
    case 0x16: case 0x17: {                   /* Mario vs Luigi: each picks their own height */
        int master = gb_peek(hRole) == 0x29;
        int c = gx >= 0 ? grid_hit_y(gx, gy, 84, 3, master ? 48 : 104) : -1;
        int cur = gb_peek(master ? hHigh1 : hHigh2);
        if (c >= 0 && c != cur) nav_grid(m, cur, c, 3);
        else if (master) macro_push(m, BTN_START);
        return 0;
    }
    case 0x1b: case 0x1d: case 0x1e: case 0x1f: case 0x20: case 0x21:   /* 2-player results */
        macro_push(m, BTN_START);
        return 0;
    case 0x0e: case 0x0f: {                   /* game type / music type */
        int g = gx >= 0 ? game_hit(gx, gy) : -1;
        int mu = gx >= 0 ? music_hit(gx, gy) : -1;
        int curg = gb_peek(hGameType) == 0x77 ? 1 : 0;
        int curm = gb_peek(hMusicType) - 0x1c;
        if (st == 0x0e) {
            if (g >= 0) {
                if (g == curg) macro_push(m, BTN_A);             /* confirm -> music section */
                else macro_push(m, g ? BTN_RIGHT : BTN_LEFT);
            } else if (mu >= 0) {
                macro_push(m, BTN_A); macro_push(m, 0);
                nav_grid(m, curm, mu, 2);
            } else macro_push(m, BTN_A);
        } else {
            if (mu >= 0) {
                if (mu == curm) macro_push(m, BTN_A);            /* confirm -> level select */
                else nav_grid(m, curm, mu, 2);
            } else if (g >= 0) {
                macro_push(m, BTN_B); macro_push(m, 0);
                if (g != curg) macro_push(m, g ? BTN_RIGHT : BTN_LEFT);
            } else macro_push(m, BTN_A);
        }
        return 0;
    }
    case 0x11: {                              /* A-type level */
        int c = gx >= 0 ? grid_hit(gx, gy, 36, 5) : -1;
        int cur = gb_peek(hATypeLevel);
        if (c < 0 || c == cur) macro_push(m, BTN_A);
        else nav_grid(m, cur, c, 5);
        return 0;
    }
    case 0x13: case 0x14: {                   /* B-type level / high */
        int lv = gx >= 0 ? grid_hit(gx, gy, 12, 5) : -1;
        int hi = gx >= 0 ? grid_hit(gx, gy, 100, 3) : -1;
        int curl = gb_peek(hBTypeLevel), curh = gb_peek(hBTypeHigh);
        if (st == 0x13) {
            if (lv >= 0) {
                if (lv == curl) macro_push(m, BTN_A);           /* -> pick height */
                else nav_grid(m, curl, lv, 5);
            } else if (hi >= 0) {
                macro_push(m, BTN_A); macro_push(m, 0);
                nav_grid(m, curh, hi, 3);
            } else macro_push(m, BTN_A);
        } else {
            if (hi >= 0) {
                if (hi == curh) macro_push(m, BTN_A);           /* start */
                else nav_grid(m, curh, hi, 3);
            } else if (lv >= 0) {
                macro_push(m, BTN_B); macro_push(m, 0);
                nav_grid(m, curl, lv, 5);
            } else macro_push(m, BTN_A);
        }
        return 0;
    }
    case 0x00: case 0x1a:
        if (gb_peek(hGamePaused)) { macro_push(m, BTN_START); return 0; }
        break;
    }
    macro_push(m, BTN_A);
    return 0;
}

/* Android back: B wherever B means "back"; returns 0 if back isn't a B here */
int touchui_back_is_b(void) {
    switch (gb_peek(hGameState)) {
    case 0x0f: case 0x11: case 0x13: case 0x14: case 0x15: return 1;
    }
    return 0;
}
