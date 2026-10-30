"""Drive the recompiled core with a search bot to exercise long play sessions:
line clears, level-ups, game over, high-score entry, endings.
Uses save states to try every (rotation, column) placement for each piece."""
import ctypes, sys, os, hashlib
import numpy as np
from PIL import Image

lib = ctypes.CDLL(sys.argv[1])
rom = open(sys.argv[2], 'rb').read()
outdir = sys.argv[3]
os.makedirs(outdir, exist_ok=True)
lib.gb_framebuffer.restype = ctypes.POINTER(ctypes.c_uint8)
lib.gb_peek.restype = ctypes.c_uint8
lib.gb_state_size.restype = ctypes.c_uint
SZ = lib.gb_state_size()
lib.gb_init(rom, len(rom))

R, L, U, D, A, B, SEL, ST = 1, 2, 4, 8, 16, 32, 64, 128
frame_no = 0
audio = []
abuf = (ctypes.c_int16 * 8192)()


def step(mask=0, n=1, record=True):
    global frame_no
    for _ in range(n):
        lib.gb_set_buttons(mask)
        lib.gb_run_frame()
        if record:
            frame_no += 1
            k = lib.gb_audio_read(abuf, 4096)
            audio.append(np.ctypeslib.as_array(abuf)[:k * 2].copy())
        else:
            lib.gb_audio_read(abuf, 4096)


def fb():
    return np.ctypeslib.as_array(lib.gb_framebuffer(), shape=(144 * 160,)).reshape(144, 160).copy()


def save():
    b = ctypes.create_string_buffer(SZ)
    lib.gb_state_save(b)
    return b


def load(b):
    assert lib.gb_state_load(b, SZ) == 0


def peek(a):
    return lib.gb_peek(a)


def snap(name):
    Image.fromarray((3 - fb()) * 85).resize((320, 288), Image.NEAREST).save(f'{outdir}/{name}.png')


def board():
    """10x18 occupancy from the game's screen buffer ($C800, 32 wide, playfield cols 2..11)."""
    g = np.zeros((18, 10), bool)
    for y in range(18):
        for x in range(10):
            g[y, x] = peek(0xC800 + y * 32 + 2 + x) != 0x2F
    return g


def evaluate(g):
    h = g.shape[0]
    heights = []
    holes = 0
    for x in range(10):
        col = g[:, x]
        top = np.argmax(col) if col.any() else h
        heights.append(h - top)
        holes += int((~col[top:]).sum())
    bump = sum(abs(heights[i] - heights[i + 1]) for i in range(9))
    return -0.51 * sum(heights) - 0.36 * holes * 2 - 0.18 * bump - (50 if max(heights) > 14 else 0)


def piece_state():
    return peek(0xFF98)  # hPieceFallingState


def play_piece(rot, shift, record=True):
    """rotate, shift, hold down until the piece hits bottom, then wait for the
    line-clear processing to finish. returns frames used."""
    n = 0
    for _ in range(rot):
        step(A, 1, record); step(0, 1, record); n += 2
    d = R if shift > 0 else L
    for _ in range(abs(shift)):
        step(d, 1, record); step(0, 1, record); n += 2
    for i in range(600):
        step(D, 1, record); n += 1
        if peek(0xFFE1) != 0 or peek(0xFF98) != 0:
            break
    for i in range(600):
        if peek(0xFFE1) != 0:
            break
        if peek(0xFF98) == 0 and peek(0xFFE3) == 0:
            break
        step(0, 1, record); n += 1
    return n


def bcd(v):
    return (v >> 4) * 10 + (v & 15)


def lines():
    return bcd(peek(0xFF9E)) + 100 * bcd(peek(0xFF9F))


def lines_from_screen():
    return peek(0xFF9E) | (peek(0xFF9F) << 8)


def main():
    # boot -> title
    step(0, 300); snap('01_copyright')
    step(ST, 4); step(0, 60); snap('02_title')
    step(ST, 4); step(0, 30); snap('03_gametype')
    step(ST, 4); step(0, 30); snap('04_level')
    # pick level 9 to reach level-ups and speed quickly? keep 0..; choose level 5
    step(R, 2); step(0, 4)
    step(ST, 4); step(0, 4)
    pieces = 0
    hashes = []
    while frame_no < 60 * 60 * 6 and peek(0xFFE1) == 0:
        base = save()
        global base_lines
        base_lines = lines()
        best = None
        for rot in range(4):
            for shift in range(-5, 6):
                load(base)
                play_piece(rot, shift, record=False)
                if peek(0xFFE1) != 0:
                    sc = -1e9
                else:
                    sc = evaluate(board()) + 3.0 * 0
                    # reward clears: compare line counter
                    sc += 8.0 * (lines() - base_lines)
                if best is None or sc > best[0]:
                    best = (sc, rot, shift)
        load(base)
        play_piece(best[1], best[2])
        pieces += 1
        if pieces % 25 == 0:
            snap(f'10_play_{pieces:04d}')
            print('pieces', pieces, 'frame', frame_no, 'gamestate', hex(peek(0xFFE1)),
                  'lines', lines(), flush=True)
    snap('20_after_play')
    print('end: frame', frame_no, 'pieces', pieces, 'state', hex(peek(0xFFE1)))
    np.concatenate(audio).astype('<i2').tofile(f'{outdir}/audio.raw')


base_lines = 0
if __name__ == '__main__':
    main()
