/* CPU state and instruction helpers used by the recompiled ARM code.
 *
 * Generated functions look like  void F_0012ab40(CPU *c)  and keep the
 * integer registers in C locals (R0..R12, SP, LR) and the flags in NF ZF CF
 * VF. The locals are written to / read from c->r[] around calls by the
 * CALL_PRE / CALL_POST / RET macros below; VFP registers live in c->V.
 *
 * Configuration (define before including):
 *   ARM_FULL_SYNC   sync r4-r12 and lr around calls too (default: ARM ABI,
 *                   only r0-r3 and sp cross a call; callee-saved registers
 *                   stay in the caller's locals).
 *   ARM_GPTR(a)     host pointer of guest address a (default: same address,
 *                   for a 32-bit host that maps the game at its own addresses).
 *   ARM_TEST_MEM    memory goes through tm_* functions (difference tests).
 *   ARM_LOW_DELTA   where data below 0x11000 is read from (host/games/gtracing/layout.h GT_LOW_DELTA):
 *                   the code uses LOWADDR(a) for those few addresses (default 0, tests).
 */
#ifndef ARM_RT_H
#define ARM_RT_H

#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef struct CPU {
    u32 r[16];                 /* r13 = sp, r14 = lr, r15 = next pc (tests) */
    u32 n, z, c, v;            /* flags, 0 or 1 (only stored where needed) */
    u32 q, ge;                 /* sticky saturation, GE[3:0] */
    u32 fpscr;
    u32 resume_pc;             /* SjLj: label to resume at */
    union {
        u64 d[32];
        u32 s[64];             /* s0..s31 = halves of d0..d15 */
        float f[64];
        double df[32];
    } V;
    void *rt;                  /* runtime per-thread data */
} CPU;

/* memory */
typedef u16 __attribute__((aligned(1), may_alias)) ua16;
typedef u32 __attribute__((aligned(1), may_alias)) ua32;
typedef u64 __attribute__((aligned(1), may_alias)) ua64;

#ifdef ARM_TEST_MEM
u8 tm_ld8(u32 a);
u16 tm_ld16(u32 a);
u32 tm_ld32(u32 a);
u64 tm_ld64(u32 a);
void tm_st8(u32 a, u8 v);
void tm_st16(u32 a, u16 v);
void tm_st32(u32 a, u32 v);
void tm_st64(u32 a, u64 v);
#define LD8(a) tm_ld8(a)
#define LD16(a) tm_ld16(a)
#define LD32(a) tm_ld32(a)
#define LD64(a) tm_ld64(a)
#define ST8(a, x) tm_st8((a), (x))
#define ST16(a, x) tm_st16((a), (x))
#define ST32(a, x) tm_st32((a), (x))
#define ST64(a, x) tm_st64((a), (x))
#else
#ifndef ARM_GPTR
#define ARM_GPTR(a) ((u8 *)(uintptr_t)(u32)(a))
#endif
#define LD8(a) (*(u8 *)ARM_GPTR(a))
#define LD16(a) (*(ua16 *)ARM_GPTR(a))
#define LD32(a) (*(ua32 *)ARM_GPTR(a))
#define LD64(a) (*(ua64 *)ARM_GPTR(a))
#define ST8(a, x) (*(u8 *)ARM_GPTR(a) = (u8)(x))
#define ST16(a, x) (*(ua16 *)ARM_GPTR(a) = (u16)(x))
#define ST32(a, x) (*(ua32 *)ARM_GPTR(a) = (u32)(x))
#define ST64(a, x) (*(ua64 *)ARM_GPTR(a) = (u64)(x))
#endif

#ifndef ARM_LOW_DELTA
#define ARM_LOW_DELTA 0u
#endif
#define LOWADDR(a) ((u32)(a) + ARM_LOW_DELTA)

/* register sync around calls */
#define LOCALS_IN()                                                          \
    u32 R0 = c->r[0], R1 = c->r[1], R2 = c->r[2], R3 = c->r[3];             \
    u32 R4 = c->r[4], R5 = c->r[5], R6 = c->r[6], R7 = c->r[7];             \
    u32 R8 = c->r[8], R9 = c->r[9], R10 = c->r[10], R11 = c->r[11];         \
    u32 R12 = c->r[12], SP = c->r[13], LR = c->r[14];                       \
    u32 NF = 0, ZF = 0, CF = 0, VF = 0;                                      \
    (void)R0; (void)R1; (void)R2; (void)R3; (void)R4; (void)R5; (void)R6;   \
    (void)R7; (void)R8; (void)R9; (void)R10; (void)R11; (void)R12;          \
    (void)LR; (void)NF; (void)ZF; (void)CF; (void)VF

#define SYNC_ARGS_OUT() (c->r[0] = R0, c->r[1] = R1, c->r[2] = R2, c->r[3] = R3, c->r[13] = SP)
#define SYNC_ARGS_IN() (R0 = c->r[0], R1 = c->r[1], R2 = c->r[2], R3 = c->r[3])
#define SYNC_HI_OUT() (c->r[4] = R4, c->r[5] = R5, c->r[6] = R6, c->r[7] = R7,   \
                       c->r[8] = R8, c->r[9] = R9, c->r[10] = R10, c->r[11] = R11, \
                       c->r[12] = R12)
#define SYNC_HI_IN() (R4 = c->r[4], R5 = c->r[5], R6 = c->r[6], R7 = c->r[7],    \
                      R8 = c->r[8], R9 = c->r[9], R10 = c->r[10], R11 = c->r[11],  \
                      R12 = c->r[12], SP = c->r[13])
/* Everything back from c (after a longjmp into this function). */
#define RELOAD_ALL() (SYNC_ARGS_IN(), SYNC_HI_IN(), LR = c->r[14],                 \
                      NF = c->n, ZF = c->z, CF = c->c, VF = c->v)

#ifdef ARM_FULL_SYNC
#define CALL_PRE(ret) (SYNC_ARGS_OUT(), SYNC_HI_OUT(), c->r[14] = (ret))
#define CALL_POST() (SYNC_ARGS_IN(), SYNC_HI_IN(), LR = c->r[14])
#define RET_SYNC() (SYNC_ARGS_OUT(), SYNC_HI_OUT(), c->r[14] = LR)
#else
#define CALL_PRE(ret) (SYNC_ARGS_OUT(), c->r[14] = (ret))
#define CALL_POST() SYNC_ARGS_IN()
#define RET_SYNC() SYNC_ARGS_OUT()
#endif

/* Runtime entry points (stage 3). */
void rt_call_indirect(CPU *c, u32 target);          /* blx rX / bx to code */
void rt_trap(CPU *c, u32 site, int kind);           /* never returns */
void rt_svc(CPU *c, u32 site, u32 imm);
enum { TRAP_UDF = 1, TRAP_FALLOFF, TRAP_NORETURN, TRAP_TABLE, TRAP_RESUME, TRAP_IJUMP };

#define TRAP(site, kind) rt_trap(c, (site), (kind))
/* Runs at every branch target (loop step counter in the function tests). */
#ifndef LABEL_HOOK
#define LABEL_HOOK() ((void)0)
#endif
#define CALL_IND(t) rt_call_indirect(c, (t))
#define MEMBAR() __sync_synchronize()

/* The host's jump buffers.  __builtin_setjmp / __builtin_longjmp (5 words, the cheap pair) where the compiler has
 * them; arm64 has not: the C library's _setjmp / _longjmp there (bionic's jmp_buf, 32 longs; no signal mask). */
#if defined(__aarch64__)
#include <setjmp.h>
#define RT_JB_WORDS 32
#define RT_SETJMP(b) _setjmp((long *)(void *)(b))
#define RT_LONGJMP(b) _longjmp((long *)(void *)(b), 1)
#else
#define RT_JB_WORDS 5
#define RT_SETJMP(b) __builtin_setjmp(b)
#define RT_LONGJMP(b) __builtin_longjmp(b, 1)
#endif

/* SjLj exception frames: a function with resume labels registers a host
 * jump buffer; the runtime's _Unwind_SjLj_Resume / throw jumps back into it
 * with c->resume_pc set and the guest registers in c. */
typedef struct RtFrame {
    void *jb[RT_JB_WORDS];
    struct RtFrame *prev;
    u32 guest_sp;
    u32 fc;                    /* the guest SjLj function context it registered, or 0 */
} RtFrame;
void rt_frame_push(CPU *c, RtFrame *f);
void rt_frame_pop(CPU *c, RtFrame *f);
#define SJLJ_FRAME() RtFrame fr_; rt_frame_push(c, &fr_)
#define SJLJ_SETJMP() RT_SETJMP(fr_.jb)
#define SJLJ_POP() rt_frame_pop(c, &fr_)

/* Guest setjmp / sigsetjmp (buffer in r0): returns twice, so the host setjmp is made in the
 * calling function's frame.  The runtime keeps the guest registers; a longjmp to it restores
 * them into c and comes back here with r0 = the value. */
void **rt_setjmp_prepare(CPU *c, u32 guest_buf, int sig);
#define GUEST_SETJMP(sig) do {                                   \
        void **jb_ = rt_setjmp_prepare(c, R0, (sig));            \
        if (RT_SETJMP(jb_)) { RELOAD_ALL(); }                   \
        else { R0 = 0; }                                         \
    } while (0)

/* flags */
#define SET_NZ(x) (NF = (u32)(x) >> 31, ZF = ((u32)(x) == 0))

static inline u32 add_flags(u32 a, u32 b, u32 cin, u32 *n, u32 *z, u32 *cf, u32 *v)
{
    u64 t = (u64)a + b + cin;
    u32 r = (u32)t;
    *cf = (u32)(t >> 32);
    *v = ((a ^ r) & (b ^ r)) >> 31;
    *n = r >> 31;
    *z = r == 0;
    return r;
}
/* a - b - !cin, as a + ~b + cin */
#define ADDS(d, a, b) ((d) = add_flags((a), (b), 0, &NF, &ZF, &CF, &VF))
#define ADCS(d, a, b) ((d) = add_flags((a), (b), CF, &NF, &ZF, &CF, &VF))
#define SUBS(d, a, b) ((d) = add_flags((a), ~(u32)(b), 1, &NF, &ZF, &CF, &VF))
#define SBCS(d, a, b) ((d) = add_flags((a), ~(u32)(b), CF, &NF, &ZF, &CF, &VF))
#define CMP(a, b) ((void)add_flags((a), ~(u32)(b), 1, &NF, &ZF, &CF, &VF))
#define CMN(a, b) ((void)add_flags((a), (b), 0, &NF, &ZF, &CF, &VF))

/* shifts by register (amount = low byte) */
static inline u32 lsl_r(u32 x, u32 s) { s &= 0xFF; return s >= 32 ? 0 : x << s; }
static inline u32 lsr_r(u32 x, u32 s) { s &= 0xFF; return s >= 32 ? 0 : x >> s; }
static inline u32 asr_r(u32 x, u32 s) { s &= 0xFF; return (u32)((s32)x >> (s >= 32 ? 31 : s)); }
static inline u32 ror_r(u32 x, u32 s) { s &= 31; return s ? (x >> s) | (x << (32 - s)) : x; }
static inline u32 ror_i(u32 x, u32 s) { return s ? (x >> s) | (x << (32 - s)) : x; }

/* With carry out; *c is left alone when the amount is 0. */
static inline u32 lsl_rc(u32 x, u32 s, u32 *c)
{
    s &= 0xFF;
    if (s == 0) return x;
    if (s < 32) { *c = (x >> (32 - s)) & 1; return x << s; }
    *c = s == 32 ? (x & 1) : 0;
    return 0;
}
static inline u32 lsr_rc(u32 x, u32 s, u32 *c)
{
    s &= 0xFF;
    if (s == 0) return x;
    if (s < 32) { *c = (x >> (s - 1)) & 1; return x >> s; }
    *c = s == 32 ? (x >> 31) : 0;
    return 0;
}
static inline u32 asr_rc(u32 x, u32 s, u32 *c)
{
    s &= 0xFF;
    if (s == 0) return x;
    if (s < 32) { *c = (x >> (s - 1)) & 1; return (u32)((s32)x >> s); }
    *c = x >> 31;
    return (u32)((s32)x >> 31);
}
static inline u32 ror_rc(u32 x, u32 s, u32 *c)
{
    if ((s & 0xFF) == 0) return x;
    s &= 31;
    if (s == 0) { *c = x >> 31; return x; }
    x = (x >> s) | (x << (32 - s));
    *c = x >> 31;
    return x;
}

/* multiply / saturation / misc */
static inline u32 smmul(u32 a, u32 b) { return (u32)(((s64)(s32)a * (s32)b) >> 32); }
static inline u32 smmla(u32 a, u32 b, u32 acc)
{
    return (u32)((((s64)(s32)acc << 32) + (s64)(s32)a * (s32)b) >> 32);
}
static inline u32 smmls(u32 a, u32 b, u32 acc)
{
    return (u32)((((s64)(s32)acc << 32) - (s64)(s32)a * (s32)b) >> 32);
}
/* 32-bit signed add that sets Q on overflow (SMLAxy, SMLAWy). */
static inline u32 add_q(u32 a, u32 b, u32 *q)
{
    u32 r = a + b;
    if (((a ^ r) & (b ^ r)) >> 31) *q = 1;
    return r;
}
static inline u32 smlaw(u32 a, u32 b16, u32 acc, u32 *q)
{
    u32 p = (u32)(((s64)(s32)a * (s16)b16) >> 16);
    return add_q(p, acc, q);
}
static inline u32 usat(s32 x, int bits, u32 *q)
{
    s32 hi = (s32)((1u << bits) - 1);
    if (x < 0) { *q = 1; return 0; }
    if (x > hi) { *q = 1; return (u32)hi; }
    return (u32)x;
}
static inline u32 ssat(s32 x, int bits, u32 *q)
{
    s32 hi = (s32)((1u << (bits - 1)) - 1), lo = -hi - 1;
    if (x < lo) { *q = 1; return (u32)lo; }
    if (x > hi) { *q = 1; return (u32)hi; }
    return (u32)x;
}
static inline u32 rev32(u32 x) { return __builtin_bswap32(x); }
static inline u32 rev16(u32 x) { return ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu); }
static inline u32 revsh(u32 x) { return (u32)(s32)(s16)(((x & 0xFF) << 8) | ((x >> 8) & 0xFF)); }
static inline u32 clz32(u32 x) { return x ? (u32)__builtin_clz(x) : 32; }

/* VFP */
static inline u32 f2bits(float x) { u32 b; memcpy(&b, &x, 4); return b; }
static inline float bits2f(u32 b) { float x; memcpy(&x, &b, 4); return x; }
static inline u64 d2bits(double x) { u64 b; memcpy(&b, &x, 8); return b; }
static inline double bits2d(u64 b) { double x; memcpy(&x, &b, 8); return x; }

/* float -> integer, round towards zero, saturating, NaN -> 0 (VCVT). */
static inline u32 cvt_f2s(double x)
{
    if (x != x) return 0;
    if (x >= 2147483648.0) return 0x7FFFFFFFu;
    if (x <= -2147483649.0) return 0x80000000u;
    return (u32)(s32)x;
}
static inline u32 cvt_f2u(double x)
{
    if (x != x) return 0;
    if (x >= 4294967296.0) return 0xFFFFFFFFu;
    if (x <= -1.0) return 0;
    return (u32)(u64)x;
}
/* VCMP / VCMPE: FPSCR N Z C V. */
static inline void vfp_cmp(CPU *c, double a, double b)
{
    u32 f = a < b ? 0x8u : a == b ? 0x6u : a > b ? 0x2u : 0x3u;
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (f << 28);
}
#define VMRS_NZCV() (NF = c->fpscr >> 31, ZF = (c->fpscr >> 30) & 1, \
                     CF = (c->fpscr >> 29) & 1, VF = (c->fpscr >> 28) & 1)

/* A NaN made by the operation itself (0/0, inf-inf, sqrt(-1)): x86 gives
 * 0xFFC00000, ARM the default NaN 0x7FC00000. NaN inputs pass through as on ARM. */
static inline float nanfix_f(float r, float a, float b, float x)
{
    if (__builtin_expect(r != r, 0) && a == a && b == b && x == x) return bits2f(0x7FC00000u);
    return r;
}
static inline double nanfix_d(double r, double a, double b, double x)
{
    if (__builtin_expect(r != r, 0) && a == a && b == b && x == x) return bits2d(0x7FF8000000000000ull);
    return r;
}

/* The C library's fmax / fmin, called by the game ~10,000 times a frame: done here, inline at each call
 * (recomp/armc.py INLINE_IMPORTS), instead of through the import bridge and libmingwex (the
 * bridge and the library were ~8 % of the render thread's own work in a race).  C99's answer as libmingwex gives
 * it (a NaN argument loses to a number; a NaN result quiet, as the bridge's x87 return made it); checked bit for
 * bit against it with gcc and clang over 450 pairs of zeros, NaNs, infinities and denormals (selftest too). */
static inline double imp_quiet(double r) { return r != r ? bits2d(d2bits(r) | 0x0008000000000000ull) : r; }
static inline double imp_fmax_d(double a, double b) { return imp_quiet((__builtin_isgreaterequal(a, b) || b != b) ? a : b); }
static inline double imp_fmin_d(double a, double b) { return imp_quiet((__builtin_islessequal(a, b) || b != b) ? a : b); }
#define IMP_DD_D(name, op)                                                                                   \
    static inline void name(CPU *c)                                                                          \
    {                                                                                                        \
        u64 r_ = d2bits(op(bits2d((u64)c->r[0] | (u64)c->r[1] << 32), bits2d((u64)c->r[2] | (u64)c->r[3] << 32))); \
        c->r[0] = (u32)r_;                                                                                   \
        c->r[1] = (u32)(r_ >> 32);                                                                           \
    }
IMP_DD_D(imp_fmax, imp_fmax_d)
IMP_DD_D(imp_fmin, imp_fmin_d)

/* Advanced SIMD and FPSCR.FZ/DN: denormals flushed to zero, NaN -> default. */
static inline float ftz(float x)
{
    u32 b = f2bits(x);
    if ((b & 0x7F800000u) == 0 && (b & 0x007FFFFFu)) return bits2f(b & 0x80000000u);
    return x;
}
static inline float dnan(float x)
{
    return x != x ? bits2f(0x7FC00000u) : x;
}
static inline float neon_f(float r) { return dnan(ftz(r)); }
static inline float fmax_neon(float a, float b)
{
    a = ftz(a); b = ftz(b);
    if (a != a || b != b) return bits2f(0x7FC00000u);
    if (a == 0 && b == 0) return bits2f(f2bits(a) & f2bits(b));   /* max(+0,-0) = +0 */
    return a > b ? a : b;
}
static inline float fmin_neon(float a, float b)
{
    a = ftz(a); b = ftz(b);
    if (a != a || b != b) return bits2f(0x7FC00000u);
    if (a == 0 && b == 0) return bits2f(f2bits(a) | f2bits(b));
    return a < b ? a : b;
}

/* conditions */
#define C_EQ (ZF)
#define C_NE (!ZF)
#define C_HS (CF)
#define C_LO (!CF)
#define C_MI (NF)
#define C_PL (!NF)
#define C_VS (VF)
#define C_VC (!VF)
#define C_HI (CF && !ZF)
#define C_LS (!CF || ZF)
#define C_GE (NF == VF)
#define C_LT (NF != VF)
#define C_GT (!ZF && NF == VF)
#define C_LE (ZF || NF != VF)

#endif
