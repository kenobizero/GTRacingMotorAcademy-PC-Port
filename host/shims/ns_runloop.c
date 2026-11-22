/* ns_runloop.c -- Foundation's time and threads for the game: NSDate, NSTimer, NSRunLoop and the performSelector
   family (the run loop's queue of timers, delayed and cross-thread performs), NSThread, NSLock / NSRecursiveLock /
   NSCondition / NSConditionLock.  Split from ns_misc.c (paths, bundles, files, defaults, notifications, URLs). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat.h"

#include "hmap.h"
#include "ns.h"
#include "ns_internal.h"

/* NSDate */

typedef struct { u32 isa, pad; double t; } Date;
static u32 cls_date;

double ns_now(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    u64 v = (u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return (double)v / 1e7 - 11644473600.0 - SINCE_1970;
}

/* PRECISE_WAIT (port addition, optimisation pass 1): sleeps
   `seconds` on a high-resolution waitable timer (Windows 10 1803 and later: wakes within a fraction of a
   millisecond, where Sleep wakes on the 1 ms tick and often a tick late).  Without it, the frame cap
   (eagl.c) and the main loop (uikit.c) spin the last 0.5-1.5 ms of each wait in a loop of
   SwitchToThread / run-loop polls, which a profile at 60 fps showed as ~11 % of the main thread spinning.
   Without such a timer (older Windows), Sleep.  GT_PRECISE_WAIT=0: Sleep and the spins. */
#ifndef _WIN32
/* POSIX (Android): nanosleep wakes within tens of microseconds, so it is the precise wait */
int ns_precise_wait_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT_PRECISE_WAIT");
        on = !(e && *e == '0');
        rt_log("[time] precise waits: %s\n", on ? "nanosleep" : "whole milliseconds");
    }
    return on;
}
void ns_wait(double seconds)
{
    if (seconds <= 0)
        return;
    if (!ns_precise_wait_on()) {
        Sleep((DWORD)(seconds * 1000.0));
        return;
    }
    struct timespec t = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}
#else
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
int ns_precise_wait_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT_PRECISE_WAIT");
        on = !(e && *e == '0');
        if (on) {
            HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (t)
                CloseHandle(t);
            else
                on = 0;
        }
        rt_log("[time] precise waits: %s\n", on ? "high-resolution timer" : "Sleep");
    }
    return on;
}
void ns_wait(double seconds)
{
    if (seconds <= 0)
        return;
    static __thread HANDLE timer;
    if (ns_precise_wait_on() && !timer)
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        Sleep((DWORD)(seconds * 1000.0));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(seconds * 1e7);     /* relative, in 100 ns */
    if (!due.QuadPart)
        due.QuadPart = -1;
    if (SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep((DWORD)(seconds * 1000.0));
}
#endif

int ns_is_date(id o) { return o && rd32(o) == cls_date; }
id ns_date(double t)
{
    id o = ns_alloc(cls_date, sizeof(Date));
    ((Date *)GP(o))->t = t;
    return objc_autorelease(o);
}
double ns_date_value(id o)
{
    if (!o)
        return 0;
    if (ns_is_date(o))
        return ((Date *)GP(o))->t;
    return bits2d(rt_guest_call64(rt_cpu(), objc_lookup(rd32(o), sel_get("timeIntervalSinceReferenceDate")), 2,
                                  (u32[]){o, sel_get("timeIntervalSinceReferenceDate")}));
}
#define DT(o) (((Date *)GP(o))->t)

static void dt_alloc(CPU *c) { ret_i(c, ns_alloc(SELF, sizeof(Date))); }
static void dt_date(CPU *c) { ret_i(c, ns_date(ns_now())); }
static void dt_sinceNow(CPU *c) { ret_i(c, ns_date(ns_now() + ARGD(0))); }
static void dt_since1970(CPU *c) { ret_i(c, ns_date(ARGD(0) - SINCE_1970)); }
static void dt_sinceRef(CPU *c) { ret_i(c, ns_date(ARGD(0))); }
static void dt_future(CPU *c) { ret_i(c, ns_date(63113904000.0)); }
static void dt_past(CPU *c) { ret_i(c, ns_date(-63114076800.0)); }
static void dt_init(CPU *c) { DT(SELF) = ns_now(); }
static void dt_initSinceNow(CPU *c) { DT(SELF) = ns_now() + ARGD(0); }
static void dt_initSince1970(CPU *c) { DT(SELF) = ARGD(0) - SINCE_1970; }
static void dt_initSinceRef(CPU *c) { DT(SELF) = ARGD(0); }
static void dt_initSinceDate(CPU *c) { DT(SELF) = ARGD(0) + ns_date_value(ARG(2)); }
static void dt_ref(CPU *c) { ret_d(c, ns_date_value(SELF)); }
static void dt_1970(CPU *c) { ret_d(c, ns_date_value(SELF) + SINCE_1970); }
static void dt_now(CPU *c) { ret_d(c, ns_date_value(SELF) - ns_now()); }
static void dt_sinceDate(CPU *c) { ret_d(c, ns_date_value(SELF) - ns_date_value(ARG(0))); }
static void dt_adding(CPU *c) { ret_i(c, ns_date(ns_date_value(SELF) + ARGD(0))); }
static void dt_compare(CPU *c)
{
    double a = ns_date_value(SELF), b = ns_date_value(ARG(0));
    ret_i(c, a < b ? (u32)-1 : a > b);
}
static void dt_isEqual(CPU *c) { ret_i(c, ARG(0) && ns_is_date(ARG(0)) && ns_date_value(SELF) == ns_date_value(ARG(0))); }
static void dt_earlier(CPU *c) { ret_i(c, ns_date_value(SELF) <= ns_date_value(ARG(0)) ? SELF : ARG(0)); }
static void dt_later(CPU *c) { ret_i(c, ns_date_value(SELF) >= ns_date_value(ARG(0)) ? SELF : ARG(0)); }
static void dt_class_ref(CPU *c) { ret_d(c, ns_now()); }
static void dt_description(CPU *c)
{
    time_t t = (time_t)(ns_date_value(SELF) + SINCE_1970);
    struct tm *g = gmtime(&t);
    char s[48] = "?";
    if (g)
        strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S +0000", g);
    ret_i(c, ns_string(s));
}
static void dt_copy(CPU *c) { ret_i(c, objc_retain(SELF)); }

/* run loop queue: timers, delayed and cross-thread performs */

typedef struct Sched {
    u32 thread;                 /* host thread id whose run loop runs it */
    double fire, interval;
    u32 timer;                  /* NSTimer object (retained) or 0 */
    u32 target, sel, arg;       /* performs: target/arg retained */
    int repeats, done;
    HANDLE event;               /* waitUntilDone */
    struct Sched *next;
} Sched;

static CRITICAL_SECTION sched_lock;
static Sched *sched_list;

typedef struct { u32 isa, pad; double fire, interval; u32 target, sel, info, repeats, valid; } Timer;
#define TM(o) ((Timer *)GP(o))
static u32 cls_timer;

static void sched_add(Sched *s)
{
    EnterCriticalSection(&sched_lock);
    s->next = sched_list;
    sched_list = s;
    LeaveCriticalSection(&sched_lock);
}

static void timer_schedule(id t, u32 thread)
{
    Sched *s = calloc(1, sizeof *s);
    s->thread = thread;
    s->fire = TM(t)->fire;
    s->interval = TM(t)->interval;
    s->repeats = TM(t)->repeats;
    s->timer = objc_retain(t);
    sched_add(s);
}

static void timer_fire(id t)
{
    Timer *x = TM(t);
    if (!x->valid)
        return;
    msg1(x->target, sel_name(x->sel), t);
    if (!x->repeats) {
        x->valid = 0;
    }
}

/* Runs what is due on this thread's run loop; returns the seconds to the next item (or 1e9). */
double ns_run_loop_once(void)
{
    u32 me = GetCurrentThreadId();
    double now = ns_now(), next = 1e9;
    for (;;) {
        Sched *due = NULL;
        EnterCriticalSection(&sched_lock);
        for (Sched **pp = &sched_list; *pp; pp = &(*pp)->next) {
            Sched *s = *pp;
            if (s->thread != me)
                continue;
            if (s->timer && !TM(s->timer)->valid) {
                *pp = s->next;
                objc_release(s->timer);
                free(s);
                if (!*pp)
                    break;
                continue;
            }
            if (s->fire <= now) {
                *pp = s->next;
                due = s;
                break;
            }
        }
        LeaveCriticalSection(&sched_lock);
        if (!due)
            break;
        u32 mark = objc_pool_push();
        if (due->timer) {
            timer_fire(due->timer);
            Timer *x = TM(due->timer);
            if (x->valid && x->repeats) {
                double iv = x->interval > 0.0001 ? x->interval : 0.0001;
                x->fire += iv;
                if (x->fire < now)
                    x->fire = now + iv;
                due->fire = x->fire;
                sched_add(due);
                due = NULL;
            } else {
                objc_release(due->timer);
            }
        } else {
            msg1(due->target, sel_name(due->sel), due->arg);
            objc_release(due->target);
            objc_release(due->arg);
            if (due->event)
                SetEvent(due->event);
        }
        objc_pool_pop(mark);
        free(due);
    }
    EnterCriticalSection(&sched_lock);
    for (Sched *s = sched_list; s; s = s->next)
        if (s->thread == me && s->fire - now < next)
            next = s->fire - now;
    LeaveCriticalSection(&sched_lock);
    return next < 0 ? 0 : next;
}

/* Waits up to `seconds` running the loop (secondary threads' CFRunLoopRunInMode and friends). */
/* PRECISE_RUNLOOP (port fix, found by the optimisation pass's benchmark): the game's render thread runs
   CFRunLoopRunInMode(mode, 0.001) once a
   frame (550 calls in 550 frames), a wait of at most 1 ms on the iPad.  Sleep(whole ms + 1) waits 2 to 2.5 ms on
   Windows' 1 ms tick (measured with the cap off: 234 fps in the menus and 235 in a race, the render thread busy
   only 40 % of the time), and so loses 1 to 1.5 ms of each 16.7 ms frame, which a slower PC needs to hold 60 fps.
   A port fault (the iPad's wait is 1 ms).  So the wait is the precise timer's (PRECISE_WAIT; GT_PRECISE_WAIT=0
   uses Sleep): 317 / 315 fps uncapped. */
static void run_for(double seconds)
{
    double end = ns_now() + seconds;
    for (;;) {
        double next = ns_run_loop_once(), left = end - ns_now();
        if (left <= 0)
            break;
        double w = next < left ? next : left;
        if (ns_precise_wait_on())
            ns_wait(w);
        else
            Sleep((DWORD)(w * 1000.0) + (w > 0 ? 1 : 0));
    }
}

static void perform_later(u32 thread, double delay, id target, SEL sel, id arg, int wait)
{
    Sched *s = calloc(1, sizeof *s);
    s->thread = thread;
    s->fire = ns_now() + delay;
    s->target = objc_retain(target);
    s->sel = sel;
    s->arg = objc_retain(arg);
    HANDLE ev = wait ? (s->event = CreateEventA(NULL, TRUE, FALSE, NULL)) : NULL;
    sched_add(s);
    if (ev) {
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }
}

static void tm_scheduled(CPU *c)
{
    /* scheduledTimerWithTimeInterval:(double)target:selector:userInfo:repeats: / timerWith... */
    id t = ns_alloc(cls_timer, sizeof(Timer));
    Timer *x = TM(t);
    x->interval = ARGD(0);
    x->fire = ns_now() + x->interval;
    x->target = objc_retain(ARG(2));
    x->sel = ARG(3);
    x->info = objc_retain(ARG(4));
    x->repeats = (u8)ARG(5);
    x->valid = 1;
    /* Port fix: Gameloft's IGP ("G" button, -[FreemiumIGPViewController buttonPressed:]) covers the screen with a
       UIWebView and gives the page 20 s (this timer, -targetMethod:) before it shows "no connection" and closes.
       This port draws no UIKit views and loads no web pages, so for those 20 s the menu sat under an invisible
       window that took every click and looked frozen. Firing the timer on the next run-loop turn takes the
       game's own offline path (page not loaded -> alert -> quit IGP) at once. */
    if (!strcmp(sel_name(x->sel), "targetMethod:") && x->target &&
        !strcmp(objc_class_name(rd32(x->target)), "FreemiumIGPViewController")) {
        rt_log("[ui] Gameloft IGP page cannot load in this port: closing it now instead of after %.0f s\n", x->interval);
        x->fire = ns_now();
    }
    objc_autorelease(t);
    if (c->r[1] == sel_get("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:"))
        timer_schedule(t, GetCurrentThreadId());
    ret_i(c, t);
}
static void tm_initWithFireDate(CPU *c)
{
    /* initWithFireDate:(id)interval:(double)target:selector:userInfo:repeats: */
    Timer *x = TM(SELF);
    x->fire = ns_date_value(ARG(0));
    x->interval = arg_d(c, 3);
    x->target = objc_retain(arg_word(c, 5));
    x->sel = arg_word(c, 6);
    x->info = objc_retain(arg_word(c, 7));
    x->repeats = (u8)arg_word(c, 8);
    x->valid = 1;
}
static void tm_alloc(CPU *c) { ret_i(c, ns_alloc(SELF, sizeof(Timer))); }
static void tm_invalidate(CPU *c)
{
    Timer *x = TM(SELF);
    if (!x->valid)
        return;
    x->valid = 0;
    id tg = x->target, inf = x->info;
    x->target = x->info = 0;
    objc_release(tg);
    objc_release(inf);
}
static void tm_isValid(CPU *c) { ret_i(c, TM(SELF)->valid); }
static void tm_fire(CPU *c) { timer_fire(SELF); }
static void tm_fireDate(CPU *c) { ret_i(c, ns_date(TM(SELF)->fire)); }
static void tm_setFireDate(CPU *c)
{
    TM(SELF)->fire = ns_date_value(ARG(0));
    EnterCriticalSection(&sched_lock);
    for (Sched *s = sched_list; s; s = s->next)
        if (s->timer == SELF)
            s->fire = TM(SELF)->fire;
    LeaveCriticalSection(&sched_lock);
}
static void tm_interval(CPU *c) { ret_d(c, TM(SELF)->interval); }
static void tm_userInfo(CPU *c) { ret_i(c, TM(SELF)->info); }
static void tm_dealloc(CPU *c)
{
    objc_release(TM(SELF)->target);
    objc_release(TM(SELF)->info);
    objc_free_instance(SELF);
}

/* NSRunLoop: one object per thread is enough for the game (it adds timers and runs modes). */
static id runloop_obj;
static void rl_current(CPU *c) { ret_i(c, ns_singleton(&runloop_obj, "NSRunLoop", 16)); }
static void rl_addTimer(CPU *c)
{
    id t = ARG(0);
    if (rd32(t) == cls_timer)
        timer_schedule(t, GetCurrentThreadId());
}
static void rl_runMode(CPU *c)
{
    /* runMode:beforeDate: -- runs what is due, waiting until the date at most */
    double until = ns_date_value(ARG(1)), left = until - ns_now();
    double next = ns_run_loop_once();
    if (left > 0) {
        double w = next < left ? next : left;
        if (w > 0.05)
            w = 0.05;
        if (ns_precise_wait_on())
            ns_wait(w);
        else
            Sleep((DWORD)(w * 1000.0));
        ns_run_loop_once();
    }
    ret_i(c, 1);
}
static void rl_runUntilDate(CPU *c) { run_for(ns_date_value(ARG(0)) - ns_now()); }
static void rl_getCF(CPU *c) { ret_i(c, SELF); }
u32 h_CFRunLoopGetCurrent(void) { return ns_singleton(&runloop_obj, "NSRunLoop", 16); }
u32 h_CFRunLoopGetMain(void) { return ns_singleton(&runloop_obj, "NSRunLoop", 16); }
u32 h_CFRunLoopRunInMode(u32 mode, u64 seconds, u32 once)
{
    (void)mode;
    (void)once;
    run_for(bits2d(seconds));
    return 3;   /* kCFRunLoopRunTimedOut */
}

/* performSelector variants on NSObject */
static void o_performAfterDelay(CPU *c)
{
    /* performSelector:withObject:afterDelay:(double) */
    perform_later(GetCurrentThreadId(), arg_d(c, 4), SELF, ARG(0), ARG(1), 0);
}
static void o_performOnMain(CPU *c)
{
    /* performSelectorOnMainThread:withObject:waitUntilDone: */
    id self = SELF;
    SEL s = ARG(0);
    id arg = ARG(1);
    int wait = (u8)ARG(2);
    if (GetCurrentThreadId() == ns_main_thread_id) {
        if (wait)
            msg1(self, sel_name(s), arg);
        else
            perform_later(ns_main_thread_id, 0, self, s, arg, 0);
        return;
    }
    perform_later(ns_main_thread_id, 0, self, s, arg, wait);
}
/* [target sel:arg] on the main thread's run loop, later (dispatch_async to the main queue) */
void ns_perform_main(id target, const char *sel, id arg) { perform_later(ns_main_thread_id, 0, target, sel_get(sel), arg, 0); }
/* ... on this thread's run loop, after `delay` seconds */
void ns_perform_here(id target, const char *sel, id arg, double delay)
{
    perform_later(GetCurrentThreadId(), delay, target, sel_get(sel), arg, 0);
}

static void o_cancelPrevious(CPU *c)
{
    /* cancelPreviousPerformRequestsWithTarget:[selector:object:] (class method) */
    int all = c->r[1] == sel_get("cancelPreviousPerformRequestsWithTarget:");
    id target = ARG(0);
    SEL s = all ? 0 : ARG(1);
    id arg = all ? 0 : ARG(2);
    EnterCriticalSection(&sched_lock);
    for (Sched **pp = &sched_list; *pp;) {
        Sched *x = *pp;
        if (!x->timer && !x->event && x->target == target && (all || (x->sel == s && x->arg == arg))) {
            *pp = x->next;
            objc_release(x->target);
            objc_release(x->arg);
            free(x);
        } else {
            pp = &x->next;
        }
    }
    LeaveCriticalSection(&sched_lock);
}

/* threads */

u32 h_pthread_create(u32 out, u32 attr, u32 start, u32 arg);

typedef struct { u32 isa; u32 target, sel, arg, name, dict, started, finished, cancelled, tid; } Thr;
#define TH(o) ((Thr *)GP(o))
static u32 cls_thread;
static Map32 thread_objs;      /* host thread id -> NSThread */
static CRITICAL_SECTION thread_lock;
static id main_thread_obj;
static volatile LONG live_threads;

static id thread_for(u32 tid)
{
    u32 v;
    EnterCriticalSection(&thread_lock);
    int ok = m32_get(&thread_objs, tid, &v);
    if (!ok) {
        v = ns_alloc(cls_thread, sizeof(Thr));
        objc_retain(v);
        m32_put(&thread_objs, tid, v);
        TH(v)->started = 1;
        TH(v)->tid = tid;
    }
    LeaveCriticalSection(&thread_lock);
    return v;
}

static void thread_entry(CPU *c)
{
    id t = c->r[0];
    TH(t)->tid = GetCurrentThreadId();
    EnterCriticalSection(&thread_lock);
    m32_put(&thread_objs, GetCurrentThreadId(), t);
    LeaveCriticalSection(&thread_lock);
    u32 mark = objc_pool_push();
    msg1(TH(t)->target, sel_name(TH(t)->sel), TH(t)->arg);
    objc_pool_pop(mark);
    TH(t)->finished = 1;
    EnterCriticalSection(&thread_lock);
    m32_del(&thread_objs, GetCurrentThreadId());
    LeaveCriticalSection(&thread_lock);
    InterlockedDecrement(&live_threads);
    objc_release(t);
    ret_i(c, 0);
}

static void thread_start(id t)
{
    static u32 entry;
    static Map32 seen;
    if (!entry)
        entry = rt_host_addr(thread_entry, "NSThread entry");
    u32 key = rd32(TH(t)->target) * 31 + TH(t)->sel, v;
    EnterCriticalSection(&thread_lock);
    int first = !m32_get(&seen, key, &v);
    m32_put(&seen, key, first ? 1 : v + 1);
    LeaveCriticalSection(&thread_lock);
    LONG n = InterlockedIncrement(&live_threads);
    if (first || rt_trace || (v + 1) % 10 == 0)
        rt_log("[thread] NSThread -[%s %s] (%ld running, %u started)\n", objc_class_name(rd32(TH(t)->target)),
               sel_name(TH(t)->sel), n, first ? 1 : v + 1);
    objc_retain(t);
    TH(t)->started = 1;
    u32 tid = 0;
    h_pthread_create(GA(&tid), 0, entry, t);
}

static void th_alloc(CPU *c) { ret_i(c, ns_alloc(SELF, sizeof(Thr))); }
static void th_current(CPU *c) { ret_i(c, thread_for(GetCurrentThreadId())); }
static void th_main(CPU *c) { ret_i(c, main_thread_obj); }
static void th_isMain_c(CPU *c) { ret_i(c, GetCurrentThreadId() == ns_main_thread_id); }
static void th_isMain_i(CPU *c) { ret_i(c, SELF == main_thread_obj); }
static void th_detach(CPU *c)
{
    /* detachNewThreadSelector:toTarget:withObject: */
    id t = ns_alloc(cls_thread, sizeof(Thr));
    TH(t)->sel = ARG(0);
    TH(t)->target = objc_retain(ARG(1));
    TH(t)->arg = objc_retain(ARG(2));
    thread_start(t);
    objc_release(t);
}
static void th_initWithTarget(CPU *c)
{
    TH(SELF)->target = objc_retain(ARG(0));
    TH(SELF)->sel = ARG(1);
    TH(SELF)->arg = objc_retain(ARG(2));
}
static void th_start(CPU *c)
{
    if (!TH(SELF)->started)
        thread_start(SELF);
}
static void th_sleep(CPU *c)
{
    double d = ARGD(0);
    Sleep(d > 0 ? (DWORD)(d * 1000.0 + 0.5) : 0);
}
static void th_sleepUntil(CPU *c)
{
    double d = ns_date_value(ARG(0)) - ns_now();
    Sleep(d > 0 ? (DWORD)(d * 1000.0 + 0.5) : 0);
}
static void th_exit(CPU *c) { rt_fatal("+[NSThread exit] (not supported yet)"); (void)c; }
static void th_cancel(CPU *c) { TH(SELF)->cancelled = 1; }
static void th_isCancelled(CPU *c) { ret_i(c, TH(SELF)->cancelled); }
static void th_isFinished(CPU *c) { ret_i(c, TH(SELF)->finished); }
static void th_isExecuting(CPU *c) { ret_i(c, TH(SELF)->started && !TH(SELF)->finished); }
static void th_priority(CPU *c) { ret_d(c, 0.5); }
static void th_name(CPU *c) { ret_i(c, TH(SELF)->name); }
static void th_setName(CPU *c) { set_copy(SELF, 4, ARG(0)); }
static void th_dict(CPU *c)
{
    if (!TH(SELF)->dict)
        TH(SELF)->dict = ns_dict_new();
    ret_i(c, TH(SELF)->dict);
}
static void o_performInBackground(CPU *c)
{
    id t = ns_alloc(cls_thread, sizeof(Thr));
    TH(t)->sel = ARG(0);
    TH(t)->target = objc_retain(SELF);
    TH(t)->arg = objc_retain(ARG(1));
    thread_start(t);
    objc_release(t);
}
static void o_performOnThread(CPU *c)
{
    /* performSelector:onThread:withObject:waitUntilDone: */
    id thr = ARG(1);
    u32 tid = thr && rd32(thr) == cls_thread && !TH(thr)->finished ? TH(thr)->tid : 0;
    if (!tid || tid == GetCurrentThreadId()) {
        msg1(SELF, sel_name(ARG(0)), ARG(2));
        return;
    }
    perform_later(tid, 0, SELF, ARG(0), ARG(2), (u8)ARG(3));
}

/* locks */

typedef struct { u32 isa; CRITICAL_SECTION *cs; CONDITION_VARIABLE *cv; u32 name; } Lock;
#define LK(o) ((Lock *)GP(o))

static Lock *lock_of(id o)
{
    Lock *l = LK(o);
    if (!l->cs) {
        CRITICAL_SECTION *cs = malloc(sizeof *cs);
        InitializeCriticalSection(cs);
        CONDITION_VARIABLE *cv = malloc(sizeof *cv);
        InitializeConditionVariable(cv);
        if (InterlockedCompareExchangePointer((void **)&l->cs, cs, NULL)) {
            DeleteCriticalSection(cs);
            free(cs);
            free(cv);
        } else {
            l->cv = cv;
        }
        while (!l->cv)
            Sleep(0);
    }
    return l;
}
static void lk_alloc(CPU *c) { ret_i(c, ns_alloc(SELF, sizeof(Lock))); }
static void lk_lock(CPU *c) { EnterCriticalSection(lock_of(SELF)->cs); }
static void lk_unlock(CPU *c) { LeaveCriticalSection(lock_of(SELF)->cs); }
static void lk_tryLock(CPU *c) { ret_i(c, TryEnterCriticalSection(lock_of(SELF)->cs) != 0); }
static void lk_lockBefore(CPU *c)
{
    Lock *l = lock_of(SELF);
    double end = ns_date_value(ARG(0));
    for (;;) {
        if (TryEnterCriticalSection(l->cs)) {
            ret_i(c, 1);
            return;
        }
        if (ns_now() >= end) {
            ret_i(c, 0);
            return;
        }
        Sleep(1);
    }
}
static void lk_wait(CPU *c)
{
    Lock *l = lock_of(SELF);
    SleepConditionVariableCS(l->cv, l->cs, INFINITE);
}
static void lk_waitUntil(CPU *c)
{
    Lock *l = lock_of(SELF);
    double d = ns_date_value(ARG(0)) - ns_now();
    ret_i(c, SleepConditionVariableCS(l->cv, l->cs, d > 0 ? (DWORD)(d * 1000.0) : 0) != 0);
}
static void lk_signal(CPU *c) { WakeConditionVariable(lock_of(SELF)->cv); }
static void lk_broadcast(CPU *c) { WakeAllConditionVariable(lock_of(SELF)->cv); }
static void lk_dealloc(CPU *c)
{
    Lock *l = LK(SELF);
    if (l->cs) {
        DeleteCriticalSection(l->cs);
        free(l->cs);
        free(l->cv);
    }
    objc_release(l->name);
    objc_free_instance(SELF);
}
static void lk_setName(CPU *c) { set_copy(SELF, 3, ARG(0)); }
static void lk_name(CPU *c) { ret_i(c, LK(SELF)->name); }

/* registration */

void ns_init_runloop(void)
{
    InitializeCriticalSection(&sched_lock);
    InitializeCriticalSection(&thread_lock);
    cls_date = ns_class("NSDate");
    cls_timer = ns_class("NSTimer");
    cls_thread = ns_class("NSThread");
    main_thread_obj = thread_for(GetCurrentThreadId());

    static const NsMethod obj[] = {
        {"performSelector:withObject:afterDelay:", o_performAfterDelay, 0},
        {"performSelectorOnMainThread:withObject:waitUntilDone:", o_performOnMain, 0},
        {"performSelectorInBackground:withObject:", o_performInBackground, 0},
        {"performSelector:onThread:withObject:waitUntilDone:", o_performOnThread, 0},
        {"cancelPreviousPerformRequestsWithTarget:", o_cancelPrevious, 1},
        {"cancelPreviousPerformRequestsWithTarget:selector:object:", o_cancelPrevious, 1}, {NULL, NULL, 0}};
    ns_methods("NSObject", obj);

    static const NsMethod date[] = {
        {"alloc", dt_alloc, 1}, {"allocWithZone:", dt_alloc, 1}, {"date", dt_date, 1},
        {"dateWithTimeIntervalSinceNow:", dt_sinceNow, 1}, {"dateWithTimeIntervalSince1970:", dt_since1970, 1},
        {"dateWithTimeIntervalSinceReferenceDate:", dt_sinceRef, 1}, {"distantFuture", dt_future, 1},
        {"distantPast", dt_past, 1}, {"timeIntervalSinceReferenceDate", dt_class_ref, 1},
        {"init", dt_init, 0}, {"initWithTimeIntervalSinceNow:", dt_initSinceNow, 0},
        {"initWithTimeIntervalSince1970:", dt_initSince1970, 0},
        {"initWithTimeIntervalSinceReferenceDate:", dt_initSinceRef, 0},
        {"initWithTimeInterval:sinceDate:", dt_initSinceDate, 0},
        {"timeIntervalSinceReferenceDate", dt_ref, 0}, {"timeIntervalSince1970", dt_1970, 0},
        {"timeIntervalSinceNow", dt_now, 0}, {"timeIntervalSinceDate:", dt_sinceDate, 0},
        {"dateByAddingTimeInterval:", dt_adding, 0}, {"addTimeInterval:", dt_adding, 0},
        {"compare:", dt_compare, 0}, {"isEqualToDate:", dt_isEqual, 0}, {"isEqual:", dt_isEqual, 0},
        {"earlierDate:", dt_earlier, 0}, {"laterDate:", dt_later, 0}, {"description", dt_description, 0},
        {"copyWithZone:", dt_copy, 0}, {NULL, NULL, 0}};
    ns_methods("NSDate", date);

    static const NsMethod timer[] = {
        {"alloc", tm_alloc, 1}, {"allocWithZone:", tm_alloc, 1},
        {"scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", tm_scheduled, 1},
        {"timerWithTimeInterval:target:selector:userInfo:repeats:", tm_scheduled, 1},
        {"initWithFireDate:interval:target:selector:userInfo:repeats:", tm_initWithFireDate, 0},
        {"invalidate", tm_invalidate, 0}, {"isValid", tm_isValid, 0}, {"fire", tm_fire, 0},
        {"fireDate", tm_fireDate, 0}, {"setFireDate:", tm_setFireDate, 0}, {"timeInterval", tm_interval, 0},
        {"userInfo", tm_userInfo, 0}, {"dealloc", tm_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("NSTimer", timer);
    static const NsMethod rl[] = {
        {"currentRunLoop", rl_current, 1}, {"mainRunLoop", rl_current, 1}, {"addTimer:forMode:", rl_addTimer, 0},
        {"runMode:beforeDate:", rl_runMode, 0}, {"runUntilDate:", rl_runUntilDate, 0},
        {"getCFRunLoop", rl_getCF, 0}, {"addPort:forMode:", ret_self, 0}, {"currentMode", ret_zero, 0},
        {NULL, NULL, 0}};
    ns_methods("NSRunLoop", rl);

    static const NsMethod th[] = {
        {"alloc", th_alloc, 1}, {"allocWithZone:", th_alloc, 1}, {"currentThread", th_current, 1},
        {"mainThread", th_main, 1}, {"isMainThread", th_isMain_c, 1}, {"isMultiThreaded", ret_yes, 1},
        {"detachNewThreadSelector:toTarget:withObject:", th_detach, 1}, {"sleepForTimeInterval:", th_sleep, 1},
        {"sleepUntilDate:", th_sleepUntil, 1}, {"exit", th_exit, 1}, {"setThreadPriority:", ret_yes, 1},
        {"threadPriority", th_priority, 1},
        {"initWithTarget:selector:object:", th_initWithTarget, 0}, {"start", th_start, 0},
        {"isMainThread", th_isMain_i, 0}, {"cancel", th_cancel, 0}, {"isCancelled", th_isCancelled, 0},
        {"isFinished", th_isFinished, 0}, {"isExecuting", th_isExecuting, 0}, {"name", th_name, 0},
        {"setName:", th_setName, 0}, {"threadDictionary", th_dict, 0}, {"setThreadPriority:", ret_yes, 0},
        {"threadPriority", th_priority, 0}, {"setStackSize:", ret_self, 0}, {NULL, NULL, 0}};
    ns_methods("NSThread", th);

    static const NsMethod lock[] = {
        {"alloc", lk_alloc, 1}, {"allocWithZone:", lk_alloc, 1}, {"lock", lk_lock, 0}, {"unlock", lk_unlock, 0},
        {"tryLock", lk_tryLock, 0}, {"lockBeforeDate:", lk_lockBefore, 0}, {"wait", lk_wait, 0},
        {"waitUntilDate:", lk_waitUntil, 0}, {"signal", lk_signal, 0}, {"broadcast", lk_broadcast, 0},
        {"setName:", lk_setName, 0}, {"name", lk_name, 0}, {"dealloc", lk_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("NSLock", lock);
    ns_methods("NSRecursiveLock", lock);
    ns_methods("NSCondition", lock);
    ns_methods("NSConditionLock", lock);
}
