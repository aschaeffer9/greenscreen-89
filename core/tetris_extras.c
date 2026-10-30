/* Hard drop for Game Boy Tetris, done the game's own way:
 * look ahead (on a save-state copy) with DOWN held to find where the piece
 * lands, put the piece one row above that spot and preload the soft-drop
 * counter, then let the original soft-drop code move it the last row,
 * detect the collision and lock it. Scoring (drop points), the landing
 * sound, line clears etc. all come from the original code. */
#include <stdlib.h>
#include "gbcore.h"

#define hGameState              0xFFE1
#define hGamePaused             0xFFAB
#define hPieceFallingState      0xFF98
#define hRowsShiftingDownState  0xFFE3
#define hTimer2                 0xFFA7
#define hNumTimesHoldingDown    0xFFE5
#define wCanPressDownToFall     0xC0C7
#define wPieceY                 0xC201   /* wSpriteSpecs + SPR_SPEC_BaseYOffset */

/* 1-player ($00) or 2-player ($1A) gameplay, not paused */
int tetris_in_game(void) {
    uint8_t s = gb_peek(hGameState);
    return (s == 0x00 || s == 0x1A) && gb_peek(hGamePaused) == 0;
}

int tetris_can_hard_drop(void) {
    return tetris_in_game() && gb_peek(hPieceFallingState) == 0 && gb_peek(hRowsShiftingDownState) == 0;
}

int tetris_piece_landed(void) { return gb_peek(hPieceFallingState) != 0 || !tetris_in_game(); }

int tetris_hard_drop(void) {
    if (!tetris_can_hard_drop()) return 0;
    static void *snap; static unsigned snap_n;
    if (!snap) { snap_n = gb_state_size(); snap = malloc(snap_n); if (!snap) return 0; }
    uint8_t y0 = gb_peek(wPieceY);
    uint8_t g0 = gb_peek(hGameState);
    int saved = gb_lookahead_begin(snap);
    gb_poke(wCanPressDownToFall, 0);
    int landed_y = -1;
    for (int i = 0; i < 240; i++) {
        gb_poke(hTimer2, 0);                              /* drop a row every frame, not every 3rd */
        gb_set_buttons(BTN_DOWN);
        gb_run_frame();
        if (gb_peek(hGameState) != g0) break;             /* e.g. topped out */
        if (gb_peek(hPieceFallingState) != 0) { landed_y = gb_peek(wPieceY); break; }
    }
    gb_lookahead_end(snap, saved);
    if (landed_y < 0) return 0;
    int rows = (landed_y - y0) / 8;
    if (rows < 0) rows = 0;
    /* resting position; next frame's soft-drop step collides and locks */
    gb_poke(wPieceY, (uint8_t)landed_y);
    gb_poke(hTimer2, 0);
    gb_poke(wCanPressDownToFall, 0);
    gb_poke(hNumTimesHoldingDown, (uint8_t)(rows > 0 ? rows : 0));
    return 1;
}

/* link: the master's bulk transfers during the 2-player round setup
 * (10 rows of starting blocks, then the 256-piece list) ignore the reply,
 * so they can be streamed without waiting a network round trip each */
int tetris_link_fire_and_forget(uint16_t pc) { return pc == 0x0923 || pc == 0x09A0; }
int tetris_is_2player(void) { return gb_peek(0xFFC5) != 0; }

/* Outside 2-player gameplay the game assumes a transfer finishes within
 * about a millisecond (it can queue the next byte and act on "transfer done"
 * as if that byte went out). There, the master Game Boy waits while its
 * transfer is in flight - as on a real cable. During play ($1A) the game
 * only sends again after the previous reply, so it tolerates lag freely. */
int tetris_link_hold_while_waiting(void) { return gb_peek(hGameState) != 0x1A; }
