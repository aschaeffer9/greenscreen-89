/* Shared state + helpers used by the generated CPU code (cpu_rec.c). */
#ifndef CPU_MACROS_H
#define CPU_MACROS_H
#include <stdint.h>
#include "gbcore.h"

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef uint64_t u64;

typedef struct { u8 a, f, b, c, d, e, h, l; u16 sp, pc; } CpuRegs;
extern CpuRegs cpu;
extern u64 cyc, evt;
extern int ime, ei_delay, halted, frame_done;
extern u16 halt_next;
extern u8 rom[0x8000], wram[0x2000], vram[0x2000], oam[0xA0], io[0x100];

u8 gb_rd_slow(u16 a);
void gb_wr_slow(u16 a, u8 v);
void sync_events(void);
#ifdef GB_INTERP_ONLY
#define gb_note_interp(pc) ((void)0)
#else
void gb_note_interp(u16 pc);
#endif
extern u16 serial_pc;

#define IFREG io[0x0F]
#define IEREG io[0xFF]

static inline u8 rd(u16 a) {
    if (a < 0x8000) return rom[a];
    if (a >= 0xC000 && a < 0xE000) return wram[a - 0xC000];
    if (a >= 0xFF80 && a != 0xFFFF) return io[a & 0xFF];
    return gb_rd_slow(a);
}
static inline void wr(u16 a, u8 v) {
    if (a >= 0xC000 && a < 0xE000) { wram[a - 0xC000] = v; return; }
    if (a >= 0xFF80 && a != 0xFFFF) { io[a & 0xFF] = v; return; }
    gb_wr_slow(a, v);
}

#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

#define BC ((u16)((B << 8) | C))
#define DE ((u16)((D << 8) | E))
#define HL ((u16)((H << 8) | L))
#define SETHL(v) do { u16 t__ = (u16)(v); H = t__ >> 8; L = t__ & 0xFF; } while (0)


#define INC8(r) do { u8 o_ = (r); (r) = (u8)(o_ + 1); F = (F & FC) | ((r) ? 0 : FZ) | (((o_ & 0xF) == 0xF) ? FH : 0); } while (0)
#define DEC8(r) do { u8 o_ = (r); (r) = (u8)(o_ - 1); F = (F & FC) | FN | ((r) ? 0 : FZ) | (((o_ & 0xF) == 0) ? FH : 0); } while (0)

#define ADD8(x) do { u8 v_ = (x); unsigned r_ = A + v_; F = ((u8)r_ ? 0 : FZ) | (((A & 0xF) + (v_ & 0xF)) > 0xF ? FH : 0) | (r_ > 0xFF ? FC : 0); A = (u8)r_; } while (0)
#define ADC8(x) do { u8 v_ = (x); unsigned c_ = (F & FC) ? 1 : 0; unsigned r_ = A + v_ + c_; F = ((u8)r_ ? 0 : FZ) | (((A & 0xF) + (v_ & 0xF) + c_) > 0xF ? FH : 0) | (r_ > 0xFF ? FC : 0); A = (u8)r_; } while (0)
#define SUB8(x) do { u8 v_ = (x); u8 r_ = (u8)(A - v_); F = FN | (r_ ? 0 : FZ) | ((A & 0xF) < (v_ & 0xF) ? FH : 0) | (A < v_ ? FC : 0); A = r_; } while (0)
#define SBC8(x) do { u8 v_ = (x); int c_ = (F & FC) ? 1 : 0; int r_ = A - v_ - c_; F = FN | ((u8)r_ ? 0 : FZ) | (((A & 0xF) - (v_ & 0xF) - c_) < 0 ? FH : 0) | (r_ < 0 ? FC : 0); A = (u8)r_; } while (0)
#define AND8(x) do { A &= (x); F = (A ? 0 : FZ) | FH; } while (0)
#define XOR8(x) do { A ^= (x); F = A ? 0 : FZ; } while (0)
#define OR8(x)  do { A |= (x); F = A ? 0 : FZ; } while (0)
#define CP8(x)  do { u8 v_ = (x); u8 r_ = (u8)(A - v_); F = FN | (r_ ? 0 : FZ) | ((A & 0xF) < (v_ & 0xF) ? FH : 0) | (A < v_ ? FC : 0); } while (0)

#define ADDHL(x) do { unsigned h_ = HL, v_ = (x); unsigned r_ = h_ + v_; F = (F & FZ) | (((h_ & 0xFFF) + (v_ & 0xFFF)) > 0xFFF ? FH : 0) | (r_ > 0xFFFF ? FC : 0); SETHL(r_); } while (0)
static inline u16 addspe(u16 sp, u8 e, u8 *f) {
    *f = (((sp & 0xF) + (e & 0xF)) > 0xF ? FH : 0) | (((sp & 0xFF) + e) > 0xFF ? FC : 0);
    return (u16)(sp + (s8)e);
}
#define ADDSPE(e) addspe(SP, (u8)(e), &F)

#define RLC(r)  do { u8 v_ = (r); (r) = (u8)((v_ << 1) | (v_ >> 7)); F = ((r) ? 0 : FZ) | ((v_ & 0x80) ? FC : 0); } while (0)
#define RRC(r)  do { u8 v_ = (r); (r) = (u8)((v_ >> 1) | (v_ << 7)); F = ((r) ? 0 : FZ) | ((v_ & 1) ? FC : 0); } while (0)
#define RL(r)   do { u8 v_ = (r); (r) = (u8)((v_ << 1) | ((F & FC) ? 1 : 0)); F = ((r) ? 0 : FZ) | ((v_ & 0x80) ? FC : 0); } while (0)
#define RR(r)   do { u8 v_ = (r); (r) = (u8)((v_ >> 1) | ((F & FC) ? 0x80 : 0)); F = ((r) ? 0 : FZ) | ((v_ & 1) ? FC : 0); } while (0)
#define SLA(r)  do { u8 v_ = (r); (r) = (u8)(v_ << 1); F = ((r) ? 0 : FZ) | ((v_ & 0x80) ? FC : 0); } while (0)
#define SRA(r)  do { u8 v_ = (r); (r) = (u8)((v_ >> 1) | (v_ & 0x80)); F = ((r) ? 0 : FZ) | ((v_ & 1) ? FC : 0); } while (0)
#define SWAP(r) do { u8 v_ = (r); (r) = (u8)((v_ >> 4) | (v_ << 4)); F = (r) ? 0 : FZ; } while (0)
#define SRL(r)  do { u8 v_ = (r); (r) = (u8)(v_ >> 1); F = ((r) ? 0 : FZ) | ((v_ & 1) ? FC : 0); } while (0)

#define DAA() do { \
    int a_ = A; \
    if (!(F & FN)) { if ((F & FC) || a_ > 0x99) { a_ += 0x60; F |= FC; } if ((F & FH) || (a_ & 0xF) > 9) a_ += 6; } \
    else { if (F & FC) a_ -= 0x60; if (F & FH) a_ -= 6; } \
    A = (u8)a_; F = (F & (FN | FC)) | (A ? 0 : FZ); } while (0)

#define REGS_LOAD() u8 A = cpu.a, F = cpu.f, B = cpu.b, C = cpu.c, D = cpu.d, E = cpu.e, H = cpu.h, L = cpu.l; u16 SP = cpu.sp, PC = cpu.pc
#define REGS_SAVE() do { cpu.a = A; cpu.f = F; cpu.b = B; cpu.c = C; cpu.d = D; cpu.e = E; cpu.h = H; cpu.l = L; cpu.sp = SP; cpu.pc = PC; } while (0)

typedef void (*blockfn)(void);
extern const blockfn gb_block_table[0x8000];
void gb_interp_one(void);

#endif
