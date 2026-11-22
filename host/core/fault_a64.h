/* fault_a64.h -- fault_android.c on a 64-bit host (arm64-v8a): the low read of an A64 load.

   The game's 4 GB are the process's lowest 4 GB there (lowmem.c), so a guest address is its host address and
   the generated code's loads are plain A64 loads of a zero-extended 32-bit address.  The low bytes are mapped at
   their own addresses from gt_low_mirror_lo up (LOW_MIRROR, fault_android.c); a read below that (from 0x1000:
   a NULL object's field, readable on iOS) faults and is done here from LOWCOPY:
   - LDR / LDRB / LDRH / LDRSB / LDRSH / LDRSW / LDUR*, immediate (unsigned offset, pre / post index, unscaled)
     or register offset, into a general register or a SIMD / FP one (B H S D Q);
   - LDP / LDPSW, both kinds of register;
   - PRFM: nothing to do.
   The base written back where the form says so; the PC moved past the instruction.  Included once, by
   fault_android.c. */
#include <asm/sigcontext.h>

#define M_PC(m) ((unsigned long)(m)->pc)
#define M_LR(m) ((unsigned long)(m)->regs[30])

/* the signal frame's records after the registers: the fault's ESR (write or read), the SIMD registers (FPSIMD,
   and SVE's when the thread has used SVE: the kernel then restores them from that one); a frame too big for the
   space goes on in an extra area (EXTRA_MAGIC) */
static struct _aarch64_ctx *a64_rec(mcontext_t *m, u32 magic)
{
    u8 *p = (u8 *)m->__reserved, *end = p + sizeof m->__reserved;
    for (int hops = 0; hops < 2 && p + sizeof(struct _aarch64_ctx) <= end;) {
        struct _aarch64_ctx *h = (struct _aarch64_ctx *)p;
        if (!h->magic)
            break;
        if (h->magic == magic)
            return h;
        if (h->magic == EXTRA_MAGIC) {
            struct extra_context *x = (struct extra_context *)h;
            p = (u8 *)(uintptr_t)x->datap;
            end = p + x->size;
            hops++;
            continue;
        }
        if (!h->size)
            break;
        p += h->size;
    }
    return NULL;
}

/* SIMD / FP register r := v (a scalar load: the rest of the register, SVE's bits too, cleared) */
static int a64_set_v(mcontext_t *m, u32 r, __uint128_t v)
{
    int done = 0;
    struct fpsimd_context *f = (struct fpsimd_context *)a64_rec(m, FPSIMD_MAGIC);
    if (f) {
        f->vregs[r] = v;
        done = 1;
    }
    struct sve_context *sv = (struct sve_context *)a64_rec(m, SVE_MAGIC);
    if (sv && sv->head.size > sizeof *sv && !(sv->flags & SVE_SIG_FLAG_SM)) {
        u32 vq = sve_vq_from_vl(sv->vl);
        u8 *z = (u8 *)sv + SVE_SIG_ZREG_OFFSET(vq, r);
        memset(z, 0, SVE_SIG_ZREG_SIZE(vq));
        memcpy(z, &v, 16);
        done = 1;
    }
    return done;
}

static int M_WRITE(mcontext_t *m)
{
    struct esr_context *e = (struct esr_context *)a64_rec(m, ESR_MAGIC);
    if (!e)
        return 0;
    u32 ec = (u32)(e->esr >> 26) & 0x3f;
    return (ec == 0x24 || ec == 0x25) && (e->esr >> 6 & 1);      /* a data abort, WnR */
}

static u64 *a64_x(mcontext_t *m, u32 r, int sp_ok)
{
    static u64 zr;
    if (r == 31)
        return sp_ok ? (u64 *)&m->sp : (zr = 0, &zr);
    return (u64 *)&m->regs[r];
}

static u64 a64_ext(u64 v, u32 option, u32 shift)
{
    switch (option) {
    case 2: v = (u32)v; break;                 /* UXTW */
    case 6: v = (u64)(s64)(s32)v; break;       /* SXTW */
    default: break;                            /* UXTX / LSL, SXTX */
    }
    return v << shift;
}

static void low_bytes(u32 addr, u8 *out, u32 n)
{
    for (u32 k = 0; k < n; k++) {
        u32 a = addr + k;
        out[k] = a >= GT_LOW_START && a < GT_LOW_END ? *(const u8 *)(uintptr_t)(a + GT_LOW_DELTA)
                                                      : *(volatile const u8 *)(uintptr_t)a;
    }
}

static int fix_low_read(mcontext_t *m, u32 addr)
{
    u32 i = *(const u32 *)(uintptr_t)m->pc;
    u32 rt = i & 31, rn = i >> 5 & 31;
    int vec = i >> 26 & 1;
    u32 nbytes = 0;
    int sign = 0, to64 = 1, pair = 0, rt2 = -1;
    u64 base = *a64_x(m, rn, 1), ea = 0, wb = 0;
    int has_wb = 0;
    if ((i & 0x3b000000u) == 0x39000000u || (i & 0x3b200000u) == 0x38000000u || (i & 0x3b200c00u) == 0x38200800u) {
        /* single register: unsigned offset (..01), imm9 (..00, bit 21 clear), register offset (..00, bit 21 set) */
        u32 size = i >> 30, opc = i >> 22 & 3;
        if (vec) {
            if (!(opc & 1))
                return 0;                                  /* a store */
            nbytes = (opc & 2) ? 16 : 1u << size;
            if ((opc & 2) && size)
                return 0;
        } else {
            if (opc == 0)
                return 0;                                  /* a store */
            nbytes = 1u << size;
            if (size == 3 && opc >= 2) {                   /* PRFM */
                m->pc += 4;
                return 1;
            }
            if (opc >= 2) {
                if (size == 2 && opc == 3)
                    return 0;
                sign = 1;
                to64 = opc == 2;
            } else
                to64 = size == 3;
        }
        u32 scale = vec && nbytes == 16 ? 4 : (u32)__builtin_ctz(nbytes);
        if ((i & 0x3b000000u) == 0x39000000u)
            ea = base + ((u64)(i >> 10 & 0xfff) << scale);
        else if (i & 0x00200000u) {
            u32 rm = i >> 16 & 31, option = i >> 13 & 7, S = i >> 12 & 1;
            ea = base + a64_ext(*a64_x(m, rm, 0), option, S ? scale : 0);
        } else {
            s64 imm9 = (s64)((s32)(i << 11) >> 23);
            u32 mode = i >> 10 & 3;
            if (mode == 1) {                               /* post-index */
                ea = base;
                wb = base + (u64)imm9, has_wb = 1;
            } else if (mode == 3) {                        /* pre-index */
                ea = base + (u64)imm9;
                wb = ea, has_wb = 1;
            } else
                ea = base + (u64)imm9;                     /* LDUR, LDTR */
        }
    } else if ((i & 0x3a400000u) == 0x28400000u) {
        /* LDP / LDPSW: no-allocate (00) and signed offset (10), post (01), pre (11) */
        u32 opc = i >> 30, mode = i >> 23 & 3;
        pair = 1;
        rt2 = (int)(i >> 10 & 31);
        if (vec) {
            if (opc == 3)
                return 0;
            nbytes = 4u << opc;
        } else {
            if (opc == 3)
                return 0;
            nbytes = opc == 2 ? 8 : 4;
            sign = opc == 1;
            to64 = opc != 0;
        }
        s64 off = (s64)((s32)(i << 10) >> 25) * (s64)nbytes;
        if (mode == 1)
            ea = base, wb = base + (u64)off, has_wb = 1;
        else if (mode == 3)
            ea = base + (u64)off, wb = ea, has_wb = 1;
        else
            ea = base + (u64)off;
    } else
        return 0;
    if (ea >> 32 || (u32)ea < GT_LOW_START || (u32)ea + nbytes * (pair ? 2 : 1) > GT_LOW_END)
        return 0;
    u8 b[32] = {0};
    low_bytes((u32)ea, b, nbytes * (pair ? 2 : 1));
    for (int k = 0; k < (pair ? 2 : 1); k++) {
        u32 r = k ? (u32)rt2 : rt;
        const u8 *src = b + k * nbytes;
        if (vec) {
            __uint128_t v = 0;
            memcpy(&v, src, nbytes);
            if (!a64_set_v(m, r, v))
                return 0;
        } else if (r != 31) {
            u64 v = 0;
            memcpy(&v, src, nbytes);
            if (sign) {
                int sh = 64 - (int)nbytes * 8;
                v = (u64)(((s64)(v << sh)) >> sh);
            }
            if (!to64)
                v = (u32)v;
            m->regs[r] = v;
        }
    }
    if (has_wb)
        *a64_x(m, rn, 1) = wb;
    static u64 seen[64];
    for (int k = 0; k < 64; k++) {
        if (seen[k] == m->pc)
            break;
        if (!seen[k]) {
            seen[k] = m->pc;
            RtThread *t = rt_thread_if();
            rt_log("[low] the game read guest 0x%05x (a NULL pointer's field, readable on iOS) at host 0x%08lx, "
                   "lr 0x%08x\n", addr, (unsigned long)m->pc, t ? t->cpu.r[14] : 0);
            break;
        }
    }
    gt_low_reads++;
    m->pc += 4;
    return 1;
}
