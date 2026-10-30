#!/usr/bin/env python3
"""Static recompiler: Game Boy Tetris ROM -> C.

Every instruction start listed in the (complete) vinheim3 disassembly becomes a
labelled block of C. Control flow with a known target becomes a direct goto;
returns / jp hl / computed targets go through a switch on PC. Anything not in
the static set (e.g. the OAM-DMA stub copied into HRAM) falls back to an
interpreter generated from the same instruction templates.
"""
import re, sys, glob

# `gen.py --interp OUT` writes only the generic instruction interpreter: it is
# built from the instruction templates alone, needs no ROM, and contains no game code.
INTERP_ONLY = len(sys.argv) == 3 and sys.argv[1] == '--interp'
if INTERP_ONLY:
    ROM, DISASM, OUT = b'', None, sys.argv[2]
else:
    ROM = open(sys.argv[1], 'rb').read()
    DISASM = sys.argv[2]
    OUT = sys.argv[3]

# ---- collect instruction addresses from the disassembly ----
addrs = set()
for f in ([] if INTERP_ONLY else glob.glob(DISASM + '/code/*.s')):
    for line in open(f):
        m = re.match(r'^\s*([A-Za-z_]+)\b.*;\s*\$([0-9a-f]{4})\s*$', line)
        if not m:
            continue
        mn = m.group(1).lower()
        if mn in ('db', 'dw', 'ds', 'incbin', 'section'):
            continue
        addrs.add(int(m.group(2), 16))
addrs = sorted(a for a in addrs if a < 0x8000)

R = ['B', 'C', 'D', 'E', 'H', 'L', None, 'A']  # None = (HL)
RP = ['BC', 'DE', 'HL', 'SP']
CC = ['!(F&FZ)', '(F&FZ)', '!(F&FC)', '(F&FC)']

LEN = [1]*256
for op in range(256):
    if op in (0x01, 0x11, 0x21, 0x31, 0x08, 0xC2, 0xC3, 0xC4, 0xCA, 0xCC, 0xCD,
              0xD2, 0xD4, 0xDA, 0xDC, 0xEA, 0xFA):
        LEN[op] = 3
    elif op in (0x06, 0x0E, 0x16, 0x1E, 0x26, 0x2E, 0x36, 0x3E, 0x10, 0x18, 0x20, 0x28,
                0x30, 0x38, 0xC6, 0xCE, 0xD6, 0xDE, 0xE6, 0xEE, 0xF6, 0xFE, 0xE0,
                0xF0, 0xE8, 0xF8, 0xCB):
        LEN[op] = 2


def rget(i):
    return 'rd(HL)' if R[i] is None else R[i]


def rset(i, v):
    return f'wr(HL,{v});' if R[i] is None else f'{R[i]}=(u8)({v});'


def rpset(i, v):
    if i == 3:
        return f'SP=(u16)({v});'
    hi, lo = RP[i][0], RP[i][1]
    return f'{{u16 t_=(u16)({v}); {hi}=t_>>8; {lo}=t_&0xFF;}}'


class Ctx:
    """mode 's' = static (constants known), 'i' = interpreter."""
    def __init__(self, mode, addr=None):
        self.mode = mode
        self.addr = addr
        if mode == 's':
            op = ROM[addr]
            self.n8 = f'0x{ROM[addr+1]:02X}' if addr + 1 < 0x8000 else '0'
            self.n16 = f'0x{ROM[addr+1] | (ROM[addr+2] << 8):04X}' if addr + 2 < 0x8000 else '0'
            self.e8 = ROM[addr+1] - 256 if ROM[addr+1] >= 128 else ROM[addr+1]
            self.next = f'0x{(addr + LEN[op]) & 0xFFFF:04X}'
            self.this = f'0x{addr:04X}'
        else:
            self.n8 = 'n8'
            self.n16 = 'n16'
            self.e8 = None
            self.next = 'nx'
            self.this = 'PC'

    # --- flow helpers ---
    # static: instructions inside a block function; leaving = set PC, goto out
    # interp: one instruction per call
    def fall(self, c):
        if self.mode == 's':
            return f'cyc+={c}; if(cyc>=evt){{PC={self.next}; goto out;}}'
        return f'cyc+={c}; PC=nx; goto out;'

    def jump_const(self, t, c):
        return f'cyc+={c}; PC=0x{t:04X}; goto out;'

    def jump_expr(self, e, c):
        return f'PC=(u16)({e}); cyc+={c}; goto out;'

    def jr_target(self):
        if self.mode == 's':
            return (self.addr + 2 + self.e8) & 0xFFFF
        return None

    def jump_rel(self, c):
        if self.mode == 's':
            return self.jump_const(self.jr_target(), c)
        return self.jump_expr('nx+(s8)n8', c)

    def jump_abs(self, c):
        if self.mode == 's':
            return self.jump_const(int(self.n16, 16), c)
        return self.jump_expr('n16', c)


def push(v):
    return f'{{u16 p_=(u16)({v}); SP--; wr(SP,p_>>8); SP--; wr(SP,p_&0xFF);}}'


POP = '(SP+=2, (u16)(rd((u16)(SP-2)) | (rd((u16)(SP-1))<<8)))'

ALU = ['ADD8', 'ADC8', 'SUB8', 'SBC8', 'AND8', 'XOR8', 'OR8', 'CP8']


def body(op, x):
    """Return C code for one instruction (non-CB). x: Ctx."""
    n8, n16 = x.n8, x.n16
    if op == 0x00: return x.fall(4)
    if op & 0xCF == 0x01: return rpset(op >> 4, n16) + x.fall(12)
    if op == 0x02: return 'wr(BC,A);' + x.fall(8)
    if op == 0x12: return 'wr(DE,A);' + x.fall(8)
    if op == 0x22: return 'wr(HL,A); SETHL(HL+1);' + x.fall(8)
    if op == 0x32: return 'wr(HL,A); SETHL(HL-1);' + x.fall(8)
    if op & 0xCF == 0x03:
        i = op >> 4; return rpset(i, f'{RP[i]}+1') + x.fall(8)
    if op & 0xCF == 0x0B:
        i = op >> 4; return rpset(i, f'{RP[i]}-1') + x.fall(8)
    if op & 0xC7 == 0x04:
        i = (op >> 3) & 7
        if R[i] is None: return '{u8 v_=rd(HL); INC8(v_); wr(HL,v_);}' + x.fall(12)
        return f'INC8({R[i]});' + x.fall(4)
    if op & 0xC7 == 0x05:
        i = (op >> 3) & 7
        if R[i] is None: return '{u8 v_=rd(HL); DEC8(v_); wr(HL,v_);}' + x.fall(12)
        return f'DEC8({R[i]});' + x.fall(4)
    if op & 0xC7 == 0x06:
        i = (op >> 3) & 7
        return rset(i, n8) + x.fall(12 if R[i] is None else 8)
    if op == 0x07: return 'A=(u8)((A<<1)|(A>>7)); F=(A&1)?FC:0;' + x.fall(4)
    if op == 0x0F: return 'F=(A&1)?FC:0; A=(u8)((A>>1)|(A<<7));' + x.fall(4)
    if op == 0x17: return '{u8 c_=(F&FC)?1:0; F=(A&0x80)?FC:0; A=(u8)((A<<1)|c_);}' + x.fall(4)
    if op == 0x1F: return '{u8 c_=(F&FC)?0x80:0; F=(A&1)?FC:0; A=(u8)((A>>1)|c_);}' + x.fall(4)
    if op == 0x08: return f'wr({n16},SP&0xFF); wr((u16)({n16}+1),SP>>8);' + x.fall(20)
    if op & 0xCF == 0x09:
        i = op >> 4; return f'ADDHL({RP[i]});' + x.fall(8)
    if op == 0x0A: return 'A=rd(BC);' + x.fall(8)
    if op == 0x1A: return 'A=rd(DE);' + x.fall(8)
    if op == 0x2A: return 'A=rd(HL); SETHL(HL+1);' + x.fall(8)
    if op == 0x3A: return 'A=rd(HL); SETHL(HL-1);' + x.fall(8)
    if op == 0x10: return x.fall(4)  # STOP: unused by Tetris
    if op == 0x18: return x.jump_rel(12)
    if op in (0x20, 0x28, 0x30, 0x38):
        c = CC[(op >> 3) & 3]
        return f'if({c}){{ {x.jump_rel(12)} }}' + x.fall(8)
    if op == 0x27: return 'DAA();' + x.fall(4)
    if op == 0x2F: return 'A=~A; F|=FN|FH;' + x.fall(4)
    if op == 0x37: return 'F=(F&FZ)|FC;' + x.fall(4)
    if op == 0x3F: return 'F=(F&(FZ|FC))^FC;' + x.fall(4)
    if op == 0x76:
        return (f'if(!(IEREG&IFREG&0x1F)){{ halted=1; halt_next={x.next}; PC={x.this}; '
                f'if(cyc<evt) cyc=evt; goto out; }}' + x.fall(4))
    if 0x40 <= op < 0x80:
        d, s = (op >> 3) & 7, op & 7
        c = 8 if (R[d] is None or R[s] is None) else 4
        return rset(d, rget(s)) + x.fall(c)
    if 0x80 <= op < 0xC0:
        s = op & 7
        return f'{ALU[(op >> 3) & 7]}({rget(s)});' + x.fall(8 if R[s] is None else 4)
    if op in (0xC0, 0xC8, 0xD0, 0xD8):
        c = CC[(op >> 3) & 3]
        return f'if({c}){{ {x.jump_expr(POP, 20)} }}' + x.fall(8)
    if op & 0xCF == 0xC1:
        i = (op >> 4) & 3
        if i == 3: return f'{{u16 v_={POP}; A=v_>>8; F=v_&0xF0;}}' + x.fall(12)
        return rpset(i, POP) + x.fall(12)
    if op in (0xC2, 0xCA, 0xD2, 0xDA):
        c = CC[(op >> 3) & 3]
        return f'if({c}){{ {x.jump_abs(16)} }}' + x.fall(12)
    if op == 0xC3: return x.jump_abs(16)
    if op in (0xC4, 0xCC, 0xD4, 0xDC):
        c = CC[(op >> 3) & 3]
        return f'if({c}){{ {push(x.next)} {x.jump_abs(24)} }}' + x.fall(12)
    if op & 0xCF == 0xC5:
        i = (op >> 4) & 3
        v = '((A<<8)|F)' if i == 3 else RP[i]
        return push(v) + x.fall(16)
    if op & 0xC7 == 0xC6:
        return f'{ALU[(op >> 3) & 7]}({n8});' + x.fall(8)
    if op & 0xC7 == 0xC7:
        t = op & 0x38
        if x.mode == 's':
            return push(x.next) + x.jump_const(t, 16)
        return push('nx') + x.jump_expr(str(t), 16)
    if op == 0xC9: return x.jump_expr(POP, 16)
    if op == 0xD9: return 'ime=1; ei_delay=0; evt=0;' + x.jump_expr(POP, 16)
    if op == 0xCD: return push(x.next) + x.jump_abs(24)
    if op == 0xE0:
        hint = f'serial_pc={x.this};' if (x.mode == 'i' or n8 == '0x02') else ''
        return hint + f'wr((u16)(0xFF00|{n8}),A);' + x.fall(12)
    if op == 0xF0: return f'A=rd((u16)(0xFF00|{n8}));' + x.fall(12)
    if op == 0xE2: return 'wr((u16)(0xFF00|C),A);' + x.fall(8)
    if op == 0xF2: return 'A=rd((u16)(0xFF00|C));' + x.fall(8)
    if op == 0xE8: return f'SP=ADDSPE({n8});' + x.fall(16)
    if op == 0xF8: return f'SETHL(ADDSPE({n8}));' + x.fall(12)
    if op == 0xE9: return x.jump_expr('HL', 4)
    if op == 0xF9: return 'SP=HL;' + x.fall(8)
    if op == 0xEA: return f'wr({n16},A);' + x.fall(16)
    if op == 0xFA: return f'A=rd({n16});' + x.fall(16)
    if op == 0xF3: return 'ime=0; ei_delay=0;' + x.fall(4)
    if op == 0xFB: return 'if(!ime){ei_delay=2; evt=0;}' + x.fall(4)
    if op == 0xCB: raise Exception('cb')
    # illegal opcodes: hardware locks up; treat as a 4-cycle no-op
    return x.fall(4)


ROT = ['RLC', 'RRC', 'RL', 'RR', 'SLA', 'SRA', 'SWAP', 'SRL']


def cbbody(cb, x):
    i = cb & 7
    hl = R[i] is None
    if cb < 0x40:
        if hl: return f'{{u8 m_=rd(HL); {ROT[cb >> 3]}(m_); wr(HL,m_);}}' + x.fall(16)
        return f'{ROT[cb >> 3]}({R[i]});' + x.fall(8)
    b = (cb >> 3) & 7
    if cb < 0x80:
        return f'F=(F&FC)|FH|(({rget(i)}&{1 << b})?0:FZ);' + x.fall(12 if hl else 8)
    if cb < 0xC0:
        if hl: return f'wr(HL,rd(HL)&{(~(1 << b)) & 0xFF});' + x.fall(16)
        return f'{R[i]}&={(~(1 << b)) & 0xFF};' + x.fall(8)
    if hl: return f'wr(HL,rd(HL)|{1 << b});' + x.fall(16)
    return f'{R[i]}|={1 << b};' + x.fall(8)


addrset = set(addrs)

# control-flow opcodes end a block
CF = set([0x18, 0x20, 0x28, 0x30, 0x38, 0xC0, 0xC2, 0xC3, 0xC4, 0xC8, 0xC9, 0xCA, 0xCC, 0xCD,
          0xD0, 0xD2, 0xD4, 0xD8, 0xD9, 0xDA, 0xDC, 0xE9, 0x76] + [0xC7 + 8 * i for i in range(8)])

blocks = []
cur = []
for a in addrs:
    if cur:
        pa = cur[-1]
        if a != pa + LEN[ROM[pa]] or ROM[pa] in CF:
            blocks.append(cur); cur = []
    cur.append(a)
if cur:
    blocks.append(cur)

out = []
w = out.append
w('/* generated by gen.py --interp: generic SM83 interpreter (no game code). do not edit. */' if INTERP_ONLY else
  '/* generated by gen.py from the Tetris ROM + vinheim3 disassembly. do not edit. */')
w('#include "cpu_macros.h"')
w(f'/* {len(addrs)} statically recompiled instructions in {len(blocks)} blocks */')
for blk in blocks:
    b0 = blk[0]
    w(f'static void B_{b0:04X}(void) {{')
    w('  REGS_LOAD();')
    if len(blk) > 1:
        w('  switch (PC) { ' + ' '.join(f'case 0x{a:04X}: goto L_{a:04X};' for a in blk[1:]) + ' default: break; }')
    for i, a in enumerate(blk):
        op = ROM[a]
        x = Ctx('s', a)
        mnem = f'{op:02X}' + (f' {ROM[a+1]:02X}' if LEN[op] > 1 else '') + (f' {ROM[a+2]:02X}' if LEN[op] > 2 else '')
        code = cbbody(ROM[a+1], x) if op == 0xCB else body(op, x)
        w(f'L_{a:04X}: /* {mnem} */ {code}')
    last = blk[-1]
    if ROM[last] not in CF or ROM[last] == 0x76 or ROM[last] in (0x20, 0x28, 0x30, 0x38, 0xC0, 0xC2, 0xC4, 0xC8, 0xCA, 0xCC, 0xD0, 0xD2, 0xD4, 0xD8, 0xDA, 0xDC):
        w(f'  PC=0x{(last + LEN[ROM[last]]) & 0xFFFF:04X};')
    w('out:')
    w('  REGS_SAVE();')
    w('}')

if not INTERP_ONLY:
    w('const blockfn gb_block_table[0x8000] = {')
    for blk in blocks:
        for a in blk:
            w(f'  [0x{a:04X}] = B_{blk[0]:04X},')
    w('};')

# interpreter: one instruction
w('void gb_interp_one(void) {')
w('  REGS_LOAD();')
w('  gb_interp_count++; if (PC < 0x8000) gb_note_interp(PC);')
w('  u8 op = rd(PC); u8 n8 = rd((u16)(PC+1)); u16 n16 = (u16)(n8 | (rd((u16)(PC+2))<<8));')
w('  u16 nx;')
w('  (void)n16;')
w('  if (op == 0xCB) { nx = (u16)(PC+2); switch (n8) {')
for cb in range(256):
    w(f'    case 0x{cb:02X}: {cbbody(cb, Ctx("i"))}')
w('  } }')
w('  switch (op) {')
for op in range(256):
    if op == 0xCB:
        continue
    w(f'  case 0x{op:02X}: nx=(u16)(PC+{LEN[op]}); {body(op, Ctx("i"))}')
w('  }')
w('out:')
w('  REGS_SAVE();')
w('}')

open(OUT, 'w').write('\n'.join(out) + '\n')
print(f'{len(addrs)} instructions, {len(blocks)} blocks, wrote {OUT}', file=sys.stderr)
