"""C CPU bots on both Game Boys, linked: checks they clear lines and finish rounds.
usage: cpubot_match.py ROM levelA levelB rounds"""
import ctypes, sys, os
sys.argv += []
L = ctypes.CDLL(os.path.join(os.path.dirname(__file__), '..', 'build', 'linksim.so'))
L.sim_peek.restype = ctypes.c_uint8; L.sim_frames.restype = ctypes.c_uint
rom = open(sys.argv[1], 'rb').read(); la, lb, rounds = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
L.sim_init(rom, 3000, 2000, ctypes.c_double(0.01), 5)
pk = lambda i, a: L.sim_peek(i, a); gs = lambda i: pk(i, 0xFFE1)
frame = 0
def step(a=0, b=0, n=1, bots=False):
    global frame
    for _ in range(n): L.sim_step_bots(1 if bots else 0, 1 if bots else 0, a, b); frame += 1
def press(i, btn):
    f0 = L.sim_frames(i)
    while L.sim_frames(i) - f0 < 2: step(btn if i == 0 else 0, btn if i == 1 else 0)
    f0 = L.sim_frames(i)
    while L.sim_frames(i) - f0 < 8: step()
def wait(pred, n):
    for _ in range(n):
        if pred(): return True
        step()
    return pred()
L.sim_cpubot_init(0, la, 11); L.sim_cpubot_init(1, lb, 22)
step(n=330); press(0, 128); press(1, 128); step(n=40)
press(0, 1); press(0, 128)
wait(lambda: gs(0) == 0x2b and gs(1) in (0x2a, 0x2b), 600); press(0, 128)
wait(lambda: gs(0) == 0x17 and gs(1) == 0x17, 900)
bcd = lambda v: (v >> 4) * 10 + (v & 15)
for r in range(rounds):
    wait(lambda: gs(0) == 0x17, 600)
    step(n=90)
    if gs(0) == 0x17: press(0, 128)
    ok = wait(lambda: gs(0) == 0x1a and gs(1) == 0x1a, 3000)
    t0 = frame
    while frame - t0 < 60 * 300 and (gs(0) == 0x1a or gs(1) == 0x1a):
        step(bots=True)
    print(f'round {r+1}: started={ok} lasted {(frame - t0) / 60:.0f}s, lines left A={bcd(pk(0, 0xFF9E))} B={bcd(pk(1, 0xFF9E))}, end states {gs(0):02x}/{gs(1):02x}', flush=True)
    for k in range(2400):
        if gs(0) in (0x16, 0x17) and k > 60: break
        if k % 60 == 30: press(0, 128)
        else: step()
    print('   wins A/B:', pk(0, 0xFFD7), pk(1, 0xFFD7))
