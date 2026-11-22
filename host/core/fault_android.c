/* fault_android.c -- the game's address range and the fault handler, on Android (main.c has Windows' own).

   The range.  On Windows the exe's first section reserves GT_RESERVE_START..GT_RESERVE_END (gt.ld, reserve.S).
   On Android the host is a shared library in a process that is already running, so the range is mapped here, in a
   constructor (when the app loads libmain.so, before SDL or anything of ours allocates), with MAP_FIXED_NOREPLACE:
   it is taken whole or not at all.  gt_reserve_start then names it as reserve.S's symbol does.

   Low reads.  The game reads guest 0x1000..0x11000 through NULL objects' fields (fine on iOS, where __TEXT starts
   there; main.c says more).  Android maps nothing below mmap_min_addr (0x8000), and the bytes are kept at
   LOWCOPY anyway, so a read of that range faults: the handler decodes the load (ARM or Thumb-2: LDR, LDRB, LDRH,
   LDRSB, LDRSH, LDRD, with or without writeback), does it from LOWCOPY into the register, and steps over it (the
   Thumb IT state advanced as the CPU would).  Other faults: the log's lines and gt_crash, as on Windows. */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "plat.h"

#include "gl.h"
#include "rt.h"

char *gt_reserve_start;                    /* the range's first byte, once mapped (main.c checks it) */
static int reserve_errno;

__attribute__((constructor(101))) static void reserve_range(void)
{
    void *want = (void *)(uintptr_t)GT_RESERVE_START;
    size_t n = GT_RESERVE_END - GT_RESERVE_START;
    void *p = mmap(want, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        reserve_errno = errno;
        return;
    }
    if (p != want) {                       /* a kernel older than 4.17 took the flag as a hint */
        munmap(p, n);
        reserve_errno = EEXIST;
        return;
    }
    gt_reserve_start = p;
}

int gt_reserve_error(void) { return reserve_errno; }

/* the low read */

typedef struct {
    int rt, rt2;                           /* destination(s); rt2 -1 unless LDRD */
    int size, sign;                        /* bytes 1 2 4 (8 for LDRD), sign-extended */
    int wb_reg;                            /* base written back, or -1 */
    u32 wb_value;
    int len;                               /* instruction bytes */
} Load;

static u32 *reg_of(mcontext_t *m, int r)
{
    return r < 0 || r > 15 ? NULL : &((u32 *)&m->arm_r0)[r];
}

static u32 shift_imm(u32 v, int type, int amt)
{
    switch (type) {
    case 0: return amt ? v << amt : v;
    case 1: return amt ? v >> amt : 0;
    case 2: return amt ? (u32)((s32)v >> amt) : (u32)((s32)v >> 31);
    default: return amt ? (v >> amt) | (v << (32 - amt)) : v;
    }
}

/* an A32 load at pc: 1 if understood */
static int decode_arm(u32 i, mcontext_t *m, Load *L)
{
    memset(L, 0, sizeof *L);
    L->rt2 = L->wb_reg = -1;
    L->len = 4;
    u32 rn = i >> 16 & 15, rt = i >> 12 & 15;
    int p = i >> 24 & 1, u = i >> 23 & 1, w = i >> 21 & 1;
    u32 base = *reg_of(m, (int)rn), off = 0;
    if ((i & 0x0C100000u) == 0x04100000u) {                /* LDR / LDRB (imm12 or register) */
        if (i & 0x02000000u) {
            if (i & 0x10)
                return 0;
            off = shift_imm(*reg_of(m, (int)(i & 15)), (int)(i >> 5 & 3), (int)(i >> 7 & 31));
        } else
            off = i & 0xFFF;
        L->size = (i >> 22 & 1) ? 1 : 4;
    } else if ((i & 0x0E1000F0u) == 0x001000B0u || (i & 0x0E1000D0u) == 0x001000D0u) {
        /* LDRH (1011), LDRSB (1101), LDRSH (1111) */
        u32 op = i >> 5 & 3;
        L->size = op == 2 ? 1 : 2;
        L->sign = op != 1;
        off = (i >> 22 & 1) ? ((i >> 4 & 0xF0) | (i & 15)) : *reg_of(m, (int)(i & 15));
    } else if ((i & 0x0E1000F0u) == 0x000000D0u) {         /* LDRD */
        if (rt & 1)
            return 0;
        L->size = 8;
        L->rt2 = (int)rt + 1;
        off = (i >> 22 & 1) ? ((i >> 4 & 0xF0) | (i & 15)) : *reg_of(m, (int)(i & 15));
    } else
        return 0;
    if (rt == 15)
        return 0;
    L->rt = (int)rt;
    u32 addr = u ? base + off : base - off;
    if (!p || w) {
        L->wb_reg = (int)rn;
        L->wb_value = addr;
    }
    return 1;
}

/* a T32 load at pc (hw1, hw2: its halfwords): 1 if understood */
static int decode_thumb(u32 hw1, u32 hw2, mcontext_t *m, Load *L)
{
    memset(L, 0, sizeof *L);
    L->rt2 = L->wb_reg = -1;
    if ((hw1 & 0xF800) < 0xE800) {                         /* 16-bit */
        L->len = 2;
        L->rt = (int)(hw1 & 7);
        switch (hw1 >> 11) {
        case 0x0D: L->size = 4; return 1;                  /* LDR rt, [rn, #imm5*4] */
        case 0x0F: L->size = 1; return 1;                  /* LDRB */
        case 0x11: L->size = 2; return 1;                  /* LDRH */
        case 0x13: L->rt = (int)(hw1 >> 8 & 7); L->size = 4; return 1;   /* LDR rt, [sp, #imm8*4] */
        }
        switch (hw1 >> 9) {
        case 0x2C: L->size = 4; return 1;                  /* LDR (register) */
        case 0x2E: L->size = 1; return 1;                  /* LDRB */
        case 0x2D: L->size = 2; return 1;                  /* LDRH */
        case 0x2B: L->size = 1; L->sign = 1; return 1;     /* LDRSB */
        case 0x2F: L->size = 2; L->sign = 1; return 1;     /* LDRSH */
        }
        return 0;
    }
    L->len = 4;
    u32 rn = hw1 & 15, rt = hw2 >> 12;
    if ((hw1 & 0xFE50) == 0xE850) {                        /* LDRD (immediate) */
        int p = hw1 >> 8 & 1, u = hw1 >> 7 & 1, w = hw1 >> 5 & 1;
        if (!p && !w)
            return 0;                                      /* LDREX and the table branches */
        L->rt = (int)rt;
        L->rt2 = (int)(hw2 >> 8 & 15);
        L->size = 8;
        if (w) {
            u32 off = (hw2 & 0xFF) * 4, base = *reg_of(m, (int)rn);
            L->wb_reg = (int)rn;
            L->wb_value = u ? base + off : base - off;
        }
        return rt != 15;
    }
    if ((hw1 & 0xFE10) != 0xF810 || rt == 15)              /* LDR{S}{B,H}.W */
        return 0;
    int sign = hw1 >> 8 & 1, sz = hw1 >> 5 & 3;
    if (sz == 3 || (sign && sz == 2))
        return 0;
    L->rt = (int)rt;
    L->size = 1 << sz;
    L->sign = sign;
    if (!(hw1 & 0x80) && (hw2 & 0x800)) {                  /* imm8 forms: P U W */
        int p = hw2 >> 10 & 1, u = hw2 >> 9 & 1, w = hw2 >> 8 & 1;
        u32 base = *reg_of(m, (int)rn), off = hw2 & 0xFF, addr = u ? base + off : base - off;
        if (!p || w) {
            L->wb_reg = (int)rn;
            L->wb_value = addr;
        }
    } else if (!(hw1 & 0x80) && (hw2 & 0xFC0)) {
        return 0;                                          /* not the register form either */
    }
    return 1;
}

/* the Thumb IT state, one instruction on (CPSR bits 15:10 and 26:25) */
static void it_advance(mcontext_t *m)
{
    u32 cpsr = m->arm_cpsr, it = (cpsr >> 25 & 3) | (cpsr >> 8 & 0xFC);
    if (!it)
        return;
    it = (it & 7) == 0 ? 0 : (it & 0xE0) | ((it << 1) & 0x1F);
    cpsr &= ~(0x06000000u | 0x0000FC00u);
    cpsr |= (it & 3) << 25 | (it & 0xFC) << 8;
    m->arm_cpsr = cpsr;
}

static int fix_low_read(mcontext_t *m, u32 addr)
{
    u32 pc = m->arm_pc;
    int thumb = (m->arm_cpsr >> 5) & 1;
    Load L;
    int ok;
    if (thumb) {
        u32 hw1 = *(const u16 *)(uintptr_t)pc, hw2 = *(const u16 *)(uintptr_t)(pc + 2);
        ok = decode_thumb(hw1, hw2, m, &L);
    } else
        ok = decode_arm(*(const u32 *)(uintptr_t)pc, m, &L);
    if (!ok || addr + (u32)L.size > GT_LOW_END)
        return 0;
    const u8 *src = (const u8 *)(uintptr_t)(addr + GT_LOW_DELTA);
    u32 v = 0, v2 = 0;
    switch (L.size) {
    case 1: v = L.sign ? (u32)(s32)(s8)src[0] : src[0]; break;
    case 2: { u16 h; memcpy(&h, src, 2); v = L.sign ? (u32)(s32)(s16)h : h; break; }
    case 4: memcpy(&v, src, 4); break;
    case 8: memcpy(&v, src, 4); memcpy(&v2, src + 4, 4); break;
    }
    static u32 seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == pc)
            break;
        if (!seen[i]) {
            seen[i] = pc;
            RtThread *t = rt_thread_if();
            rt_log("[low] the game read guest 0x%05x (a NULL pointer's field, readable on iOS) at host 0x%08x, "
                   "lr 0x%08x\n", addr, pc, t ? t->cpu.r[14] : 0);
            break;
        }
    }
    if (L.wb_reg >= 0 && L.wb_reg != L.rt && L.wb_reg != L.rt2)
        *reg_of(m, L.wb_reg) = L.wb_value;
    *reg_of(m, L.rt) = v;
    if (L.rt2 >= 0)
        *reg_of(m, L.rt2) = v2;
    m->arm_pc = pc + (u32)L.len;
    if (thumb)
        it_advance(m);
    return 1;
}

/* faults */

static void on_fault(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = ucv;
    mcontext_t *m = &uc->uc_mcontext;
    u32 addr = (u32)(uintptr_t)si->si_addr;
    /* error_code is the data abort's FSR: bit 11 set for a write */
    if (sig == SIGSEGV && addr >= GT_LOW_START && addr < GT_LOW_END && !(m->error_code & (1u << 11)) &&
        fix_low_read(m, addr))
        return;
    static char why[512];
    const char *what = sig == SIGSEGV || sig == SIGBUS ? ((m->error_code & (1u << 11)) ? "write" : "read") : NULL;
    if (what) {
        const char *imp = rt_addr_name(addr);
        rt_log("\n[FAULT] %s of 0x%08x at host 0x%08lx (lr 0x%08lx)%s%s\n", what, addr, m->arm_pc, m->arm_lr,
               imp ? " -- import address of " : "", imp ? imp : "");
        snprintf(why, sizeof why, "The game crashed: a bad memory %s at 0x%08x (host 0x%08lx).", what, addr,
                 m->arm_pc);
    } else {
        rt_log("\n[FAULT] signal %d at host 0x%08lx (lr 0x%08lx)\n", sig, m->arm_pc, m->arm_lr);
        snprintf(why, sizeof why, "The game crashed: %s (signal %d at host 0x%08lx).",
                 sig == SIGILL ? "an illegal instruction" : sig == SIGFPE ? "an arithmetic fault" : "a fault", sig,
                 m->arm_pc);
    }
    RtThread *t = rt_thread_if();
    if (t) {
        CPU *c = &t->cpu;
        rt_log("guest r0-r3 %08x %08x %08x %08x  r4-r7 %08x %08x %08x %08x\n"
               "      r8-r12 %08x %08x %08x %08x %08x  sp %08x lr %08x\n", c->r[0], c->r[1], c->r[2], c->r[3],
               c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13],
               c->r[14]);
    }
    gles_fault_dump();
    rt_log("(the host address minus libmain.so's base, in the log's [main] line, names the function: "
           "llvm-addr2line -e libmain.so)\n");
    gt_crash(4, why);
}

/* each host thread that runs game code: a stack of its own for the handler (a guest stack overflow) */
void gt_fault_thread_init(void)
{
    static const size_t n = 64 * 1024;
    stack_t ss;
    ss.ss_sp = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ss.ss_sp == MAP_FAILED)
        return;
    ss.ss_size = n;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
}

void gt_fault_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    gt_fault_thread_init();
}

#endif
