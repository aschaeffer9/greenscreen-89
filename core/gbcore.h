/* Public API of the recompiled Tetris core. */
#ifndef GBCORE_H
#define GBCORE_H
#include <stdint.h>

#define GB_W 160
#define GB_H 144
#define GB_SAMPLE_RATE 48000

/* button bits for gb_set_buttons */
enum {
    BTN_RIGHT = 1, BTN_LEFT = 2, BTN_UP = 4, BTN_DOWN = 8,
    BTN_A = 16, BTN_B = 32, BTN_SELECT = 64, BTN_START = 128
};

/* rom must be the 32 KiB Tetris (World) (Rev A) image; returns 0 on success */
int gb_init(const uint8_t *rom, unsigned len);
/* run until the next frame is complete (~70224 cycles) */
void gb_run_frame(void);
/* the last completed frame, 160x144 shades 0..3 (0 = lightest) */
const uint8_t *gb_framebuffer(void);
void gb_set_buttons(uint8_t mask);
/* pull up to max stereo frames (interleaved L,R int16); returns frames copied */
int gb_audio_read(int16_t *dst, int max_frames);
int gb_audio_available(void);
/* run about n cycles (stops early at end of frame); returns 1 if a frame completed */
int gb_run_cycles(unsigned n);
uint64_t gb_cycles(void);
/* link cable */
typedef void (*gb_link_start_fn)(uint8_t byte_out, uint16_t pc, void *user);
void gb_link_attach(gb_link_start_fn fn, void *user);
int gb_link_master_waiting(void);
void gb_link_master_complete(uint8_t byte_in);
int gb_link_passive_ready(void);
uint8_t gb_link_passive_exchange(uint8_t byte_in);   /* returns the byte we sent back */
/* save states */
unsigned gb_state_size(void);
void gb_state_save(void *dst);
int gb_state_load(const void *src, unsigned len);
void gb_poke(uint16_t a, uint8_t v);
void gb_set_headless(int on);   /* skip drawing and sound (game logic and timing unchanged) */
int gb_lookahead_begin(void *buf);
void gb_lookahead_end(const void *buf, int saved_awp);
/* Tetris-specific additions (tetris_extras.c) */
int tetris_in_game(void);            /* playing and not paused */
int tetris_can_hard_drop(void);
int tetris_hard_drop(void);          /* teleports the piece; caller holds DOWN until it locks */
int tetris_piece_landed(void);
int tetris_link_fire_and_forget(uint16_t pc);
int tetris_is_2player(void);
int tetris_link_hold_while_waiting(void);
int tetris_single_busy(void);       /* mid single-player game (online status) */
/* debugging */
extern int force_interp;
extern unsigned long long gb_interp_count;
extern unsigned gb_interp_rom_hits;
uint16_t gb_pc(void);
uint8_t gb_peek(uint16_t a);
#endif
