/* Android: ADPF performance hints (GT_PERF_HINT=1, the default).  A phone's CPU governor sets the clocks from
   the load it has seen; a game's frame is a burst then a wait, and the governor kept the big cores below the
   clock a 16.7 ms frame needs (52 fps on a Snapdragon 870).  The hint session tells the system the frame's
   target and each frame's real work: it raises the clocks when a frame runs long.  Android 13+ (API 33; looked
   up at run time, the APK runs from Android 10). */
#ifdef __ANDROID__
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "plat.h"
#include "rt.h"

typedef void *(*T_getManager)(void);
typedef void *(*T_createSession)(void *mgr, const int32_t *tids, size_t n, int64_t target_ns);
typedef int (*T_report)(void *session, int64_t ns);
typedef int (*T_update)(void *session, int64_t ns);

static void *session;
static T_report report;
static T_update update_target;
static int state;                         /* 0 not tried, 1 on, -1 off */
static int64_t target;

void gt_android_perf_frame(double work_s, double period_s)
{
    if (state < 0)
        return;
    if (state == 0) {
        state = -1;
        const char *e = getenv("GT_PERF_HINT");
        if (e && *e == '0') {
            rt_log("[perf] PERF_HINT: off (GT_PERF_HINT=0)\n");
            return;
        }
        void *lib = dlopen("libandroid.so", RTLD_NOW);
        T_getManager gm = lib ? (T_getManager)dlsym(lib, "APerformanceHint_getManager") : NULL;
        T_createSession cs = lib ? (T_createSession)dlsym(lib, "APerformanceHint_createSession") : NULL;
        report = lib ? (T_report)dlsym(lib, "APerformanceHint_reportActualWorkDuration") : NULL;
        update_target = lib ? (T_update)dlsym(lib, "APerformanceHint_updateTargetWorkDuration") : NULL;
        void *mgr = gm ? gm() : NULL;
        target = (int64_t)((period_s > 0 ? period_s : 1.0 / 60) * 1e9);
        int32_t tid = gettid();
        session = mgr && cs && report ? cs(mgr, &tid, 1, target) : NULL;
        rt_log("[perf] PERF_HINT: %s\n", session ? "on (the frame thread's clocks follow its frames)"
                                                 : mgr ? "the system gave no session" : "not on this Android (13 and later)");
        if (!session)
            return;
        state = 1;
    }
    int64_t t = (int64_t)((period_s > 0 ? period_s : 1.0 / 60) * 1e9);
    if (t != target && update_target) {
        update_target(session, t);
        target = t;
    }
    int64_t w = (int64_t)(work_s * 1e9);
    if (w > 0)
        report(session, w);
}
#endif
