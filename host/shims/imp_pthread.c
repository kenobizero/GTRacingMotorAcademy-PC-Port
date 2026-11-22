/* imp_pthread.c -- POSIX threads (Darwin layouts) on Win32.

   The game's pthread objects are Darwin structs in its memory: a signature word, then opaque
   bytes.  The host keeps its own object (CRITICAL_SECTION, CONDITION_VARIABLE, SRWLOCK) and stores
   a pointer to it in the opaque area; statically initialised objects (PTHREAD_MUTEX_INITIALIZER
   etc.) are recognised by their Darwin signature and set up on first use.
   pthread_t is the address of a host ThreadRec. */
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "abi.h"
#include "rt.h"

#define SIG_MUTEX_INIT   0x32AAABA7u   /* Darwin _PTHREAD_MUTEX_SIG_init */
#define SIG_RMUTEX_INIT  0x32AAABA1u   /* _PTHREAD_RECURSIVE_MUTEX_SIG_init */
#define SIG_COND_INIT    0x3CB0B1BBu
#define SIG_RWLOCK_INIT  0x2DA8B3B4u
#define SIG_HOST_MUTEX   0x47544D58u   /* "GTMX": made by the host */
#define SIG_HOST_COND    0x4754434Eu
#define SIG_HOST_RWLOCK  0x4754524Cu

#define ETIMEDOUT_D 60
#define EBUSY_D 16

static CRITICAL_SECTION init_lock;
static int init_ready;

static void need_init_lock(void)
{
    if (!init_ready) {
        InitializeCriticalSection(&init_lock);
        init_ready = 1;
    }
}

/* mutexes (always recursive: a CRITICAL_SECTION) */

static CRITICAL_SECTION *mutex_of(u32 m)
{
    u32 sig = rd32(m);
    if (sig == SIG_HOST_MUTEX)
        return GP(rd32(m + 4));
    need_init_lock();
    EnterCriticalSection(&init_lock);
    if (rd32(m) != SIG_HOST_MUTEX) {
        CRITICAL_SECTION *cs = malloc(sizeof *cs);
        InitializeCriticalSection(cs);
        wr32(m + 4, GA(cs));
        MemoryBarrier();
        wr32(m, SIG_HOST_MUTEX);
    }
    LeaveCriticalSection(&init_lock);
    return GP(rd32(m + 4));
}

u32 h_pthread_mutex_init(u32 m, u32 attr)
{
    (void)attr;
    CRITICAL_SECTION *cs = malloc(sizeof *cs);
    InitializeCriticalSection(cs);
    wr32(m + 4, GA(cs));
    wr32(m, SIG_HOST_MUTEX);
    return 0;
}
u32 h_pthread_mutex_destroy(u32 m)
{
    if (rd32(m) == SIG_HOST_MUTEX) {
        CRITICAL_SECTION *cs = GP(rd32(m + 4));
        DeleteCriticalSection(cs);
        free(cs);
        wr32(m, 0);
    }
    return 0;
}
u32 h_pthread_mutex_lock(u32 m) { EnterCriticalSection(mutex_of(m)); return 0; }
u32 h_pthread_mutex_trylock(u32 m) { return TryEnterCriticalSection(mutex_of(m)) ? 0 : EBUSY_D; }
u32 h_pthread_mutex_unlock(u32 m) { LeaveCriticalSection(mutex_of(m)); return 0; }
u32 h_pthread_mutexattr_init(u32 a) { wr32(a, 0x4D555458); wr32(a + 4, 0); return 0; }
u32 h_pthread_mutexattr_destroy(u32 a) { (void)a; return 0; }
u32 h_pthread_mutexattr_settype(u32 a, u32 t) { wr32(a + 4, t); return 0; }

/* condition variables */

static CONDITION_VARIABLE *cond_of(u32 c)
{
    if (rd32(c) == SIG_HOST_COND)
        return GP(rd32(c + 4));
    need_init_lock();
    EnterCriticalSection(&init_lock);
    if (rd32(c) != SIG_HOST_COND) {
        CONDITION_VARIABLE *cv = malloc(sizeof *cv);
        InitializeConditionVariable(cv);
        wr32(c + 4, GA(cv));
        MemoryBarrier();
        wr32(c, SIG_HOST_COND);
    }
    LeaveCriticalSection(&init_lock);
    return GP(rd32(c + 4));
}

u32 h_pthread_cond_init(u32 c, u32 attr)
{
    (void)attr;
    CONDITION_VARIABLE *cv = malloc(sizeof *cv);
    InitializeConditionVariable(cv);
    wr32(c + 4, GA(cv));
    wr32(c, SIG_HOST_COND);
    return 0;
}
u32 h_pthread_cond_destroy(u32 c)
{
    if (rd32(c) == SIG_HOST_COND) {
        free(GP(rd32(c + 4)));
        wr32(c, 0);
    }
    return 0;
}
u32 h_pthread_cond_signal(u32 c) { WakeConditionVariable(cond_of(c)); return 0; }
/* abstime: struct timespec {time_t tv_sec; long tv_nsec;} in wall-clock time */
u32 h_pthread_cond_timedwait(u32 c, u32 m, u32 ts)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    u64 now_ms = (((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000) - 11644473600000ull;
    u64 at_ms = (u64)rd32(ts) * 1000 + rd32(ts + 4) / 1000000;
    DWORD wait = at_ms > now_ms ? (DWORD)(at_ms - now_ms) : 0;
    if (SleepConditionVariableCS(cond_of(c), mutex_of(m), wait))
        return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT_D : 0;
}

/* read-write locks */

typedef struct { SRWLOCK l; volatile DWORD writer; } RwLock;

static RwLock *rw_of(u32 r)
{
    if (rd32(r) == SIG_HOST_RWLOCK)
        return GP(rd32(r + 4));
    need_init_lock();
    EnterCriticalSection(&init_lock);
    if (rd32(r) != SIG_HOST_RWLOCK) {
        RwLock *l = calloc(1, sizeof *l);
        InitializeSRWLock(&l->l);
        wr32(r + 4, GA(l));
        MemoryBarrier();
        wr32(r, SIG_HOST_RWLOCK);
    }
    LeaveCriticalSection(&init_lock);
    return GP(rd32(r + 4));
}

u32 h_pthread_rwlock_init(u32 r, u32 attr) { (void)attr; wr32(r, 0); rw_of(r); return 0; }
u32 h_pthread_rwlock_destroy(u32 r)
{
    if (rd32(r) == SIG_HOST_RWLOCK) {
        free(GP(rd32(r + 4)));
        wr32(r, 0);
    }
    return 0;
}
u32 h_pthread_rwlock_wrlock(u32 r)
{
    RwLock *l = rw_of(r);
    AcquireSRWLockExclusive(&l->l);
    l->writer = GetCurrentThreadId();
    return 0;
}
u32 h_pthread_rwlock_trywrlock(u32 r)
{
    RwLock *l = rw_of(r);
    if (!TryAcquireSRWLockExclusive(&l->l))
        return EBUSY_D;
    l->writer = GetCurrentThreadId();
    return 0;
}
u32 h_pthread_rwlock_tryrdlock(u32 r) { return TryAcquireSRWLockShared(&rw_of(r)->l) ? 0 : EBUSY_D; }
u32 h_pthread_rwlock_unlock(u32 r)
{
    RwLock *l = rw_of(r);
    if (l->writer == GetCurrentThreadId()) {
        l->writer = 0;
        ReleaseSRWLockExclusive(&l->l);
    } else {
        ReleaseSRWLockShared(&l->l);
    }
    return 0;
}

/* threads */

typedef struct ThreadRec {
    HANDLE h;
    u32 start, arg, result;
    u32 stack;
    void *exit_jb[5];
    int detached;
} ThreadRec;

static __thread ThreadRec *self_rec;

/* Darwin pthread_attr_t: sig, then (here) the stack size and the detach state. */
u32 h_pthread_attr_init(u32 a)
{
    memset(GP(a), 0, 40);
    wr32(a, 0x54484441);
    wr32(a + 4, 512 * 1024);
    return 0;
}
u32 h_pthread_attr_destroy(u32 a) { (void)a; return 0; }
u32 h_pthread_attr_setstacksize(u32 a, u32 n) { wr32(a + 4, n); return 0; }

static DWORD WINAPI thread_main(void *p)
{
    ThreadRec *t = p;
    self_rec = t;
    rt_thread_new(t->stack < 256 * 1024 ? 256 * 1024 : t->stack);
    if (!__builtin_setjmp(t->exit_jb))
        t->result = rt_call1(t->start, t->arg);
    rt_thread_end();
    return 0;
}

u32 h_pthread_create(u32 out, u32 attr, u32 start, u32 arg)
{
    ThreadRec *t = calloc(1, sizeof *t);
    t->start = start;
    t->arg = arg;
    t->stack = attr && rd32(attr) == 0x54484441 ? rd32(attr + 4) : 512 * 1024;
    t->detached = attr && rd32(attr) == 0x54484441 && rd32(attr + 8) == 2;
    /* the host stack holds the recompiled frames, which are larger than the game's */
    t->h = CreateThread(NULL, 16u << 20, thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->h) {
        free(t);
        return 35;
    }
    rt_log("[thread] pthread_create start 0x%08x arg 0x%08x\n", start, arg);
    wr32(out, GA(t));
    return 0;
}

/* SCHED_YIELD (optimisation pass 6, GT_SCHED_YIELD): a sched_yield that only returns 0 lets the game's
   loops that wait for another thread by yielding spin at full speed: 96-115 thousand calls a frame at the start and
   17 thousand a frame on the render thread while a race loads (measured on a test save).  On iOS the call
   gives the rest of the time slice to another thread that is ready, so this does the same (SwitchToThread:
   another ready thread on this core runs, else the call returns at once, as on iOS).  Measured on an 8-core
   laptop: the spins ~30 times fewer, the race's loading time the same (11.02 / 11.06 s); the CPU time it frees
   counts on 2-4 core CPUs, where the spinning thread takes a core the loader needs.
   GT_SCHED_YIELD=0: returns at once. */
u32 h_sched_yield(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT_SCHED_YIELD");
        on = !(e && *e == '0');
        rt_log("[thread] SCHED_YIELD: %s\n", on ? "on (SwitchToThread)" : "off (GT_SCHED_YIELD=0)");
    }
    if (on)
        SwitchToThread();
    return 0;
}

u32 h_pthread_self(void)
{
    if (!self_rec) {
        self_rec = calloc(1, sizeof *self_rec);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self_rec->h, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    }
    return GA(self_rec);
}

u32 h_pthread_join(u32 th, u32 res)
{
    ThreadRec *t = GP(th);
    WaitForSingleObject(t->h, INFINITE);
    if (res)
        wr32(res, t->result);
    return 0;
}

void h_pthread_exit(u32 value)
{
    ThreadRec *t = self_rec;
    if (!t || !t->start)
        rt_fatal("pthread_exit on a thread the game did not create");
    t->result = value;
    __builtin_longjmp(t->exit_jb, 1);
}

u32 h_pthread_cancel(u32 th) { rt_log("[thread] pthread_cancel 0x%08x ignored\n", th); return 0; }
void h_pthread_testcancel(void) {}
u32 h_pthread_setcanceltype(u32 t, u32 old) { if (old) wr32(old, 0); (void)t; return 0; }
u32 h_pthread_kill(u32 th, u32 sig) { (void)th; (void)sig; return 0; }
u32 h_pthread_getschedparam(u32 th, u32 policy, u32 param)
{
    (void)th;
    if (policy)
        wr32(policy, 1);
    if (param)
        wr32(param, 31);
    return 0;
}
u32 h_pthread_setschedparam(u32 th, u32 policy, u32 param) { (void)th; (void)policy; (void)param; return 0; }
