"""Two Game Boys linked over a simulated network, played by bots.
usage: linkbot.py ROM OUTDIR latency_ms jitter_ms loss rounds seed"""
import ctypes, sys, os
import numpy as np
from PIL import Image

L = ctypes.CDLL(os.path.join(os.path.dirname(__file__), '..', 'build', 'linksim.so'))
L.sim_peek.restype = ctypes.c_uint8
L.sim_fb.restype = ctypes.POINTER(ctypes.c_uint8)
L.sim_frames.restype = ctypes.c_uint
rom = open(sys.argv[1], 'rb').read()
out = sys.argv[2]; os.makedirs(out, exist_ok=True)
lat, jit, loss = float(sys.argv[3]), float(sys.argv[4]), float(sys.argv[5])
ROUNDS = int(sys.argv[6]) if len(sys.argv) > 6 else 2
SEED = int(sys.argv[7]) if len(sys.argv) > 7 else 1
L.sim_init(rom, int(lat * 1000), int(jit * 1000), ctypes.c_double(loss), SEED)

R, Lf, U, D, A, B, SEL, ST = 1, 2, 4, 8, 16, 32, 64, 128
frame = 0
log = []


def pk(i, a): return L.sim_peek(i, a)
def gs(i): return pk(i, 0xFFE1)


def step(a=0, b=0, n=1):
    global frame
    for _ in range(n):
        L.sim_step(a, b); frame += 1


def fb(i):
    return np.ctypeslib.as_array(L.sim_fb(i), shape=(144 * 160,)).reshape(144, 160).copy()


def snap(name):
    a, b = fb(0), fb(1)
    img = np.full((144, 330), 1, np.uint8)
    img[:, :160] = a; img[:, 170:] = b
    Image.fromarray(((3 - img) * 85).astype('uint8')).resize((660, 288), Image.NEAREST).save(f'{out}/{name}.png')


def stats():
    s = (ctypes.c_uint * 10)()
    res = []
    for i in range(2):
        L.sim_stats(i, s); res.append(list(s))
    return res


# ---------------- bot ----------------
def board(i):
    g = np.zeros((18, 10), bool)
    for y in range(18):
        for x in range(10):
            g[y, x] = pk(i, 0xC800 + y * 32 + 2 + x) != 0x2F
    return g


def evaluate(g):
    h = g.shape[0]; heights = []; holes = 0
    for x in range(10):
        col = g[:, x]; top = np.argmax(col) if col.any() else h
        heights.append(h - top); holes += int((~col[top:]).sum())
    bump = sum(abs(heights[i] - heights[i + 1]) for i in range(9))
    return -0.51 * sum(heights) - 0.72 * holes - 0.18 * bump


def lines(i):
    v = pk(i, 0xFF9E); return (v >> 4) * 10 + (v & 15)


TARGET = {}
def la_place(i, rot, shift):
    """in isolated lookahead: rotate/shift then hard drop; returns score"""
    L.sim_la_frame(i, 0)                     # align to a frame boundary
    for _ in range(rot):
        L.sim_la_frame(i, A); L.sim_la_frame(i, 0)
    for _ in range(abs(shift)):
        L.sim_la_frame(i, R if shift > 0 else Lf); L.sim_la_frame(i, 0)
    TARGET[i] = (pk(i, 0xC203), pk(i, 0xC202))
    if not L.sim_hard_drop(i):
        return -1e9
    for _ in range(6):
        L.sim_la_frame(i, D)
        if L.sim_landed(i): break
    for _ in range(30):
        L.sim_la_frame(i, 0)
        if pk(i, 0xFF98) == 0 and pk(i, 0xFFE3) == 0: break
    return evaluate(board(i))


def plan(i):
    best = None
    for rot in range(4):
        for shift in range(-5, 6):
            L.sim_la_begin(i)
            l0 = lines(i)
            sc = la_place(i, rot, shift)
            if gs(i) not in (0x1a,): sc -= 1000
            sc += 6 * abs(lines(i) - l0) ** 2
            L.sim_la_end(i)
            if best is None or sc > best[0]: best = (sc, rot, shift, TARGET[i])
    return best


class Bot:
    """closed loop: plan a target (rotation spec, X) by lookahead, then steer to it and hard drop"""
    def __init__(self, i):
        self.i = i; self.target = None; self.need_plan = True; self.prev_fall = 0; self.toggle = 0; self.hold = 0; self.tries = 0
    def buttons(self):
        i = self.i
        fall = pk(i, 0xFF98)
        if self.prev_fall and not fall: self.need_plan = True
        self.prev_fall = fall
        if gs(i) != 0x1a or pk(i, 0xFFAB):
            self.target = None; self.need_plan = True; return 0
        if self.hold:
            self.hold -= 1; return D
        if self.target is None:
            if self.need_plan and fall == 0 and pk(i, 0xFFE3) == 0 and pk(i, 0xC201) <= 0x28:
                self.need_plan = False
                self.target = plan(i)[3]; self.tries = 0
            return 0
        self.toggle ^= 1
        if self.toggle: return 0
        spec, x = pk(i, 0xC203), pk(i, 0xC202)
        self.tries += 1
        if spec != self.target[0] and self.tries < 20: return A
        if x < self.target[1] and self.tries < 40: return R
        if x > self.target[1] and self.tries < 40: return Lf
        self.target = None
        return 'HD'


bots = [Bot(0), Bot(1)]


def bot_frame():
    bs = []
    for bt in bots:
        b = bt.buttons()
        if b == 'HD':
            L.sim_hard_drop(bt.i); b = D
            bt.hold = 3
        bs.append(b)
    step(bs[0], bs[1])


def wait_state(pred, maxf, a=0, b=0):
    for _ in range(maxf):
        if pred(): return True
        step(a, b)
    return pred()


def press(i, btn, hold=2, gap=8):
    # hold until that Game Boy has really run `hold` frames with the button down
    f0 = L.sim_frames(i)
    for _ in range(600):
        if L.sim_frames(i) - f0 >= hold: break
        step(btn if i == 0 else 0, btn if i == 1 else 0)
    f0 = L.sim_frames(i)
    for _ in range(600):
        if L.sim_frames(i) - f0 >= gap: break
        step(0, 0)


# ---------------- script ----------------
step(0, 0, 330)
press(0, ST); press(1, ST)
step(0, 0, 40)
print('title states', hex(gs(0)), hex(gs(1)))
snap('01_title')
press(0, R)                       # A: highlight 2 PLAYER
press(0, ST)                      # A: start -> master
ok = wait_state(lambda: gs(0) in (0x2a, 0x2b) and gs(1) in (0x2a, 0x2b), 300)
print('music select both:', ok, hex(gs(0)), hex(gs(1)), 'roles', pk(0, 0xFFCB), pk(1, 0xFFCB))
snap('02_music')
press(0, ST)                      # master confirms music
ok = wait_state(lambda: gs(0) == 0x17 and gs(1) == 0x17, 600)
print('mario/luigi both:', ok, hex(gs(0)), hex(gs(1)))
press(0, R); press(1, R); press(1, R)   # A picks height 1, B picks height 2
step(0, 0, 30)
snap('03_marioluigi')
results = []
for rnd in range(ROUNDS):
    if gs(0) == 0x17: press(0, ST)   # master starts the round
    t0 = frame
    ok = wait_state(lambda: gs(0) == 0x1a and gs(1) == 0x1a, 3000)
    print(f'round {rnd+1}: in game both: {ok} after {frame - t0} frames ({(frame - t0) / 59.73:.1f}s)', hex(gs(0)), hex(gs(1)))
    pa = bytes(pk(0, 0xC300 + k) for k in range(256)); pb = bytes(pk(1, 0xC300 + k) for k in range(256))
    print('  piece lists identical:', pa == pb)
    snap(f'1{rnd}_ingame_start')
    sent = [[], []]; recv = [[], []]
    prev_lines = [lines(0), lines(1)]; prev_mult = [0, 0]
    t0 = frame
    last_print = frame
    while frame - t0 < 60 * 60 * 6:
        bot_frame()
        for i in (0, 1):
            ln = lines(i)
            k = prev_lines[i] - ln
            if k >= 2: sent[i].append({2: 1, 3: 2, 4: 4}[min(k, 4)])
            prev_lines[i] = ln
            m = pk(i, 0xFFD3)
            if m and not prev_mult[i]: recv[i].append(m)
            prev_mult[i] = m
        if gs(0) != 0x1a and gs(1) != 0x1a: break
        if frame - last_print > 1800:
            last_print = frame
            print(f'  t={(frame - t0) / 60:.0f}s lines A={lines(0)} B={lines(1)} states {hex(gs(0))} {hex(gs(1))}')
            snap(f'1{rnd}_play_{frame}')
    print(f'  game over after {(frame - t0) / 60:.0f}s: lines A={lines(0)} B={lines(1)} states {hex(gs(0))} {hex(gs(1))}')
    snap(f'1{rnd}_end')
    step(0, 0, 120)
    st_ = stats(); print('  wire: A sent', st_[0][8], 'B got', st_[1][9], '| B sent', st_[1][8], 'A got', st_[0][9])
    print('  garbage A->B sent', sent[0], 'B received', recv[1])
    print('  garbage B->A sent', sent[1], 'A received', recv[0])
    results.append((sent, recv))
    # let the result screens play out; tap through
    seen = []
    for k in range(2400):
        st_ = (gs(0), gs(1))
        if not seen or seen[-1] != st_: seen.append(st_)
        if st_[0] in (0x16, 0x17, 0x1a, 0x18, 0x19) and k > 60: break
        if k % 60 == 30: press(0, ST)
        else: step(0, 0)
    print('  result screens:', ' > '.join(f'{a:02x}/{b:02x}' for a, b in seen))
    wins = (pk(0, 0xFFD7), pk(1, 0xFFD7))
    print('  after results: states', hex(gs(0)), hex(gs(1)), 'win counters', [hex(pk(i, a)) for i in (0, 1) for a in (0xFFD7, 0xFFD8)])
    snap(f'1{rnd}_results')
for i, s in enumerate(stats()):
    print('side', 'AB'[i], 'xfers out/in', s[0], s[1], 'retransmits', s[2], 'dups', s[3])
print('packets sent', stats()[0][4], 'lost', stats()[0][5])
