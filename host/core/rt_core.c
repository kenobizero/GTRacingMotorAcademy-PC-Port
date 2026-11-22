/* rt_core.c -- threads, calls between the host and the recompiled game, traps, logging. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "rt.h"

typedef struct { u32 addr; void (*fn)(CPU *); } DispatchEntry;
extern const DispatchEntry rt_dispatch[];          /* build/gen/dispatch.c, sorted by address */
extern const unsigned rt_dispatch_count;

__attribute__((noreturn)) void rt_longjmp_host(void **jb);   /* runtime/rt_jump.c (clang) */

/* logging */

static FILE *logf;
static CRITICAL_SECTION log_lock;
static int log_ready;
static int log_off;             /* set by rt_log_open only, before any other thread runs: read without the lock */
#if GT_DEV
int rt_trace;
#endif

/* path: the log file; "0" (GT_LOG=0) writes no log at all, neither the file nor the console */
void rt_log_open(const char *path)
{
    if (!log_ready) {
        InitializeCriticalSection(&log_lock);
        log_ready = 1;
    }
    if (path && !strcmp(path, "0"))
        log_off = 1;
    else if (path)
        logf = fopen(path, "w");
}

int rt_log_enabled(void) { return !log_off; }

/* Each line with the folders it names written as their tags (scrub.c, CRASH_DUMP): the log goes
   with a bug report. */
size_t rt_scrub(char *s, size_t n);

int rt_vlog(const char *fmt, va_list ap)
{
    if (!log_ready)
        rt_log_open(NULL);
    if (log_off)
        return -1;
    char buf[2048], *p = buf;
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) {
        va_end(aq);
        return n;
    }
    int len = n;
    if ((size_t)n >= sizeof buf && (p = malloc((size_t)n + 1)))
        vsnprintf(p, (size_t)n + 1, fmt, aq);
    else if (!p)
        p = buf, n = sizeof buf - 1;
    va_end(aq);
    EnterCriticalSection(&log_lock);
    size_t m = rt_scrub(p, (size_t)n);
    fwrite(p, 1, m, stderr);
    fflush(stderr);
    if (logf) {
        fwrite(p, 1, m, logf);
        fflush(logf);
    }
    LeaveCriticalSection(&log_lock);
    /* the line is gone from memory once written: a crash dump holds this thread's stack, and the tail of a long
       line (past what rt_scrub shortened) would otherwise stay in it */
    SecureZeroMemory(p, (size_t)n + 1);   /* n < sizeof buf when p is buf */
    if (p != buf)
        free(p);
    return len;
}

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    rt_vlog(fmt, ap);
    va_end(ap);
}

/* The log's line, the crash dump and the box (crashdump.c), then the end. */
void rt_fatal(const char *fmt, ...)
{
    static char why[1024];              /* static: the box shows it after this frame is gone */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    gt_crash(3, why);
}

/* threads */

static __thread RtThread *cur;
static volatile LONG next_id;

#define STACK_SLACK 0x10000u       /* above the guest stack top: variadic reads past the args */

RtThread *rt_thread_new(u32 stack_size)
{
    RtThread *t = calloc(1, sizeof *t);
    if (!t)
        rt_fatal("out of memory (thread)");
    stack_size = (stack_size + 0xFFFFu) & ~0xFFFFu;
    u8 *st = VirtualAlloc(NULL, stack_size + STACK_SLACK, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!st)
        rt_fatal("no memory for a %u-byte guest stack", stack_size);
    t->stack_lo = GA(st);
    t->stack_hi = GA(st) + stack_size;
    t->cpu.r[13] = t->stack_hi;
    t->cpu.rt = t;
    t->id = (int)InterlockedIncrement(&next_id);
    cur = t;
#ifndef _WIN32
    void gt_fault_thread_init(void);
    gt_fault_thread_init();             /* fault_android.c: the fault handler's own stack on this thread */
#endif
    return t;
}

RtThread *rt_thread_if(void) { return cur; }

RtThread *rt_thread(void)
{
    if (!cur)
        rt_thread_new(512 * 1024);
    return cur;
}

void rt_thread_end(void)
{
    RtThread *t = cur;
    if (!t)
        return;
    cur = NULL;
    VirtualFree(GP(t->stack_lo), 0, MEM_RELEASE);
    for (RtJmp *j = t->jmps, *n; j; j = n) {
        n = j->next;
        free(j);
    }
    free(t);
}

/* guest functions */

/* FAST_DISPATCH (port addition, optimisation pass 3): a game address to its recompiled function.  A binary
   search over the 23,873 functions takes about 15 steps through a 190 KB table on every indirect call, message
   send and host call into the game (measured: rt_lookup was the render thread's busiest host function in a race,
   1.6 %).  So a table over __text in 64-byte buckets (about 100,000 of them, 0.4 MB) gives
   the first function at or after each bucket's start, and the few functions inside a bucket are stepped through.
   One table entry per 2-byte address would take 12.4 MB for the same answer.  Built
   once before main; addresses outside the table take the binary search. */
#define LK_SHIFT 6
static u32 *lk_first, lk_base, lk_buckets;

__attribute__((constructor)) static void lk_build(void)
{
    if (!rt_dispatch_count)
        return;
    lk_base = rt_dispatch[0].addr & ~((1u << LK_SHIFT) - 1);
    lk_buckets = ((rt_dispatch[rt_dispatch_count - 1].addr - lk_base) >> LK_SHIFT) + 1;
    u32 *t = malloc(lk_buckets * sizeof *t);
    if (!t)
        return;
    unsigned i = 0;
    for (u32 b = 0; b < lk_buckets; b++) {
        u32 start = lk_base + (b << LK_SHIFT);
        while (i < rt_dispatch_count && rt_dispatch[i].addr < start)
            i++;
        t[b] = i;
    }
    lk_first = t;
}

void (*rt_lookup(u32 addr))(CPU *)
{
    u32 b = (addr - lk_base) >> LK_SHIFT;
    if (lk_first && b < lk_buckets) {
        unsigned i = lk_first[b];
        while (i < rt_dispatch_count && rt_dispatch[i].addr < addr)
            i++;
        return i < rt_dispatch_count && rt_dispatch[i].addr == addr ? rt_dispatch[i].fn : NULL;
    }
    unsigned lo = 0, hi = rt_dispatch_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rt_dispatch[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < rt_dispatch_count && rt_dispatch[lo].addr == addr)
        return rt_dispatch[lo].fn;
    return NULL;
}

/* host function addresses */

static RtBridge hostfn[GT_HOSTFN_SIZE / 4];
static const char *hostfn_name[GT_HOSTFN_SIZE / 4];
static unsigned nhostfn;
static CRITICAL_SECTION hostfn_lock;
static int hostfn_ready;

u32 rt_host_addr(RtBridge fn, const char *what)
{
    if (!hostfn_ready) {
        InitializeCriticalSection(&hostfn_lock);
        hostfn_ready = 1;
    }
    EnterCriticalSection(&hostfn_lock);
    for (unsigned i = 0; i < nhostfn; i++)
        if (hostfn[i] == fn) {
            LeaveCriticalSection(&hostfn_lock);
            return GT_HOSTFN_BASE + 4 * i;
        }
    if (nhostfn == GT_HOSTFN_SIZE / 4)
        rt_fatal("too many host function addresses");
    hostfn[nhostfn] = fn;
    hostfn_name[nhostfn] = what;
    u32 a = GT_HOSTFN_BASE + 4 * nhostfn++;
    LeaveCriticalSection(&hostfn_lock);
    return a;
}

extern RtBridge gt_import_fn[];            /* loader.c: by (address - GT_IMPORT_BASE) / 4 */
extern const char *gt_import_name[];
extern unsigned gt_import_count;

const char *rt_addr_name(u32 a)
{
    if (a - GT_IMPORT_BASE < GT_IMPORT_SIZE && (a - GT_IMPORT_BASE) / 4 < gt_import_count)
        return gt_import_name[(a - GT_IMPORT_BASE) / 4];
    if (a - GT_HOSTFN_BASE < GT_HOSTFN_SIZE && (a - GT_HOSTFN_BASE) / 4 < nhostfn)
        return hostfn_name[(a - GT_HOSTFN_BASE) / 4];
    return NULL;
}

void rt_call_indirect(CPU *c, u32 target)
{
    u32 a = target;
    if (a - GT_IMPORT_BASE < GT_IMPORT_SIZE) {
        unsigned i = (a - GT_IMPORT_BASE) / 4;
        if (i < gt_import_count && !(a & 3)) {
            if (rt_trace)
                rt_log("[call] %s (via pointer)\n", gt_import_name[i]);
            gt_import_fn[i](c);
            return;
        }
    } else if (a - GT_HOSTFN_BASE < GT_HOSTFN_SIZE) {
        unsigned i = (a - GT_HOSTFN_BASE) / 4;
        if (i < nhostfn && !(a & 3)) {
            hostfn[i](c);
            return;
        }
    }
    void (*fn)(CPU *) = rt_lookup(a);
    if (!fn)
        fn = rt_lookup(a ^ 1);             /* mode bit missing or extra */
    if (!fn)
        rt_fatal("call to 0x%08x: not a function (from return address 0x%08x)", target, c->r[14]);
    fn(c);
}

#define MAGIC_RET 0x0FFF0000u

u64 rt_guest_call64(CPU *c, u32 fn, int n, const u32 *args)
{
    u32 save[4] = {c->r[0], c->r[1], c->r[2], c->r[3]};
    u32 sp = c->r[13], lr = c->r[14];
    u32 nsp = sp;
    if (n > 4) {
        nsp = (sp - 4u * (u32)(n - 4)) & ~7u;
        for (int i = 4; i < n; i++)
            wr32(nsp + 4u * (u32)(i - 4), args[i]);
    } else {
        nsp &= ~7u;
    }
    for (int i = 0; i < 4; i++)
        c->r[i] = i < n ? args[i] : 0;
    c->r[13] = nsp;
    c->r[14] = MAGIC_RET;
    rt_call_indirect(c, fn);
    u32 r0 = c->r[0], r1 = c->r[1];
    for (int i = 0; i < 4; i++)
        c->r[i] = save[i];
    c->r[13] = sp;
    c->r[14] = lr;
    return (u64)r1 << 32 | r0;
}

u32 rt_guest_call(CPU *c, u32 fn, int n, const u32 *args) { return (u32)rt_guest_call64(c, fn, n, args); }
u32 rt_call1(u32 fn, u32 a) { u32 v[] = {a}; return rt_guest_call(rt_cpu(), fn, 1, v); }
u32 rt_call2(u32 fn, u32 a, u32 b) { u32 v[] = {a, b}; return rt_guest_call(rt_cpu(), fn, 2, v); }
u32 rt_call3(u32 fn, u32 a, u32 b, u32 c3) { u32 v[] = {a, b, c3}; return rt_guest_call(rt_cpu(), fn, 3, v); }
u32 rt_call4(u32 fn, u32 a, u32 b, u32 c3, u32 d)
{
    u32 v[] = {a, b, c3, d};
    return rt_guest_call(rt_cpu(), fn, 4, v);
}

/* traps */

static const char *trap_name(int k)
{
    switch (k) {
    case TRAP_UDF: return "undefined instruction / trap";
    case TRAP_FALLOFF: return "ran off the end of the code";
    case TRAP_NORETURN: return "a no-return call returned";
    case TRAP_TABLE: return "jump table index out of range";
    case TRAP_RESUME: return "SjLj resume at an unknown label";
    case TRAP_IJUMP: return "unresolved indirect jump";
    }
    return "?";
}

void rt_trap(CPU *c, u32 site, int kind)
{
    rt_fatal("guest trap at 0x%08x: %s (lr 0x%08x, r0 0x%08x)", site, trap_name(kind), c->r[14], c->r[0]);
}

void rt_svc(CPU *c, u32 site, u32 imm)
{
    rt_fatal("guest svc #%u at 0x%08x (r12 %u)", imm, site, c->r[12]);
}

void rt_missing_import(CPU *c, const char *name)
{
    rt_fatal("the game called %s, which this port does not have yet (return address 0x%08x)",
             name, c->r[14]);
}

/* SjLj host frames */

void rt_frame_push(CPU *c, RtFrame *f)
{
    RtThread *t = c->rt;
    f->prev = t->top;
    f->guest_sp = c->r[13];
    f->fc = 0;
    t->top = f;
}

void rt_frame_pop(CPU *c, RtFrame *f)
{
    RtThread *t = c->rt;
    t->top = f->prev;
}

/* Resume guest function context fc at its landing pad (the unwinder, imp_unwind.c): the host
   frame that registered it is found on the list; frames above it are dropped. */
__attribute__((noreturn)) void rt_resume_context(CPU *c, u32 fc)
{
    RtThread *t = c->rt;
    RtFrame *f = t->top;
    while (f && f->fc != fc)
        f = f->prev;
    if (!f)
        rt_fatal("SjLj resume: no host frame registered function context 0x%08x", fc);
    t->top = f;
    /* jbuf: [0] fp (r7), [1] resume address, [2] sp */
    c->r[7] = rd32(fc + 0x20);
    c->resume_pc = rd32(fc + 0x24);
    c->r[13] = rd32(fc + 0x28);
    rt_longjmp_host(f->jb);
}

/* guest setjmp / longjmp */

void **rt_setjmp_prepare(CPU *c, u32 guest_buf, int sig)
{
    (void)sig;
    RtThread *t = c->rt;
    RtJmp *j = t->jmps;
    while (j && j->guest_buf != guest_buf)
        j = j->next;
    if (!j) {
        j = calloc(1, sizeof *j);
        if (!j)
            rt_fatal("out of memory (setjmp)");
        j->guest_buf = guest_buf;
        j->next = t->jmps;
        t->jmps = j;
    }
    memcpy(j->r, c->r, sizeof j->r);
    memcpy(j->d, &c->V.d[8], sizeof j->d);
    j->top = t->top;
    return j->jb;
}

static __attribute__((noreturn)) void rt_guest_longjmp(CPU *c, u32 guest_buf, u32 val)
{
    RtThread *t = c->rt;
    RtJmp *j = t->jmps;
    while (j && j->guest_buf != guest_buf)
        j = j->next;
    if (!j)
        rt_fatal("longjmp to 0x%08x: no setjmp was made there on this thread", guest_buf);
    for (int i = 4; i <= 14; i++)
        c->r[i] = j->r[i];
    memcpy(&c->V.d[8], j->d, sizeof j->d);
    c->r[0] = val ? val : 1;
    t->top = j->top;
    rt_longjmp_host(j->jb);
}

/* the imports: longjmp(buf, val), siglongjmp(buf, val) -- the signal mask is not kept */
void I_longjmp(CPU *c) { rt_guest_longjmp(c, c->r[0], c->r[1]); }
void I_siglongjmp(CPU *c) { rt_guest_longjmp(c, c->r[0], c->r[1]); }
