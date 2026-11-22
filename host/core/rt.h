/* rt.h -- the GT Racing host runtime (stage 3): threads, guest calls, imports, logging.
   The recompiled game (build/gen) calls into it through arm_rt.h; host code that talks to the
   game uses the helpers here. */
#ifndef GT_RT_H
#define GT_RT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

#include "arm_rt.h"
#include "layout.h"

#ifndef _WIN32                     /* the C library's Microsoft names the host uses (Android: posixwin.h has the rest) */
#include <strings.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifndef _stricmp
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#endif
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define strtok_s strtok_r
#define _mkgmtime timegm
#define _fileno fileno
#define localtime_s(tm, t) (localtime_r((t), (tm)) ? 0 : 1)
#define gmtime_s(tm, t) (gmtime_r((t), (tm)) ? 0 : 1)
static inline void *_aligned_malloc(size_t n, size_t align)
{
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, n) ? NULL : p;
}
#define _aligned_free free
#endif

/* threads */

/* A guest setjmp buffer (setjmp/sigsetjmp) and the host frame that made it. */
typedef struct RtJmp {
    u32 guest_buf;
    void *jb[5];               /* __builtin_setjmp buffer in the caller's host frame */
    u32 r[16];                 /* guest registers at setjmp (r4-r11, sp, lr used) */
    u64 d[8];                  /* d8-d15 */
    RtFrame *top;              /* SjLj frame list at setjmp */
    struct RtJmp *next;
} RtJmp;

typedef struct RtThread {
    CPU cpu;
    RtFrame *top;              /* host frames of functions with SjLj resume labels */
    u32 stack_lo, stack_hi;    /* guest stack */
    RtJmp *jmps;
    int id;
    u32 sjlj_head;             /* _Unwind_SjLj_Register list (guest function contexts) */
    u32 eh_caught;             /* __cxa_begin_catch stack (guest exception headers) */
    u32 eh_uncaught;
    void *host;                /* host-side per-thread data (shims) */
    void *objc_pool;           /* autorelease pool stack (objc.c) */
    u32 errno_cell;            /* the guest's errno (___error) */
} RtThread;

RtThread *rt_thread(void);                 /* the calling thread's, made on first use */
RtThread *rt_thread_if(void);              /* the calling thread's, or NULL (none made) */
static inline CPU *rt_cpu(void) { return &rt_thread()->cpu; }
RtThread *rt_thread_new(u32 stack_size);   /* for a new host thread; becomes current */
void rt_thread_end(void);

/* calling guest code */

/* Call guest function fn (bit 0 = Thumb) with up to n 32-bit argument words (more than 4 go on
   the guest stack).  Returns r0 (rt_guest_call64: r1:r0); the caller's registers are kept. */
u32 rt_guest_call(CPU *c, u32 fn, int n, const u32 *args);
u64 rt_guest_call64(CPU *c, u32 fn, int n, const u32 *args);
u32 rt_call1(u32 fn, u32 a);
u32 rt_call2(u32 fn, u32 a, u32 b);
u32 rt_call3(u32 fn, u32 a, u32 b, u32 c3);
u32 rt_call4(u32 fn, u32 a, u32 b, u32 c3, u32 d);

/* Guest function at addr, or NULL. */
void (*rt_lookup(u32 addr))(CPU *);

/* imports and host functions */

typedef void (*RtBridge)(CPU *);
typedef struct { const char *name; RtBridge fn; } RtBridgeEntry;
extern const RtBridgeEntry rt_bridges[];   /* generated (bridges.c), sorted by name */
extern const unsigned rt_bridge_count;
RtBridge rt_bridge_by_name(const char *name);
/* The guest address for host function fn (made on first use): calls through it reach fn. */
u32 rt_host_addr(RtBridge fn, const char *what);
/* Import address range bookkeeping (loader.c). */
u32 rt_import_addr(const char *name);      /* 0 if not a function import */
const char *rt_addr_name(u32 addr);        /* import / host function at a fake address, or NULL */
void rt_missing_import(CPU *c, const char *name);

/* logging */
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int rt_vlog(const char *fmt, va_list ap);  /* the line's length before scrubbing; -1: the log is off, nothing formatted */
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_log_open(const char *path);
int rt_log_enabled(void);                  /* 0 after GT_LOG=0 (rt_log_open) */
void rt_hide_path(const char *path, const char *tag);   /* scrub.c: that folder is written as its tag */

/* settings (settings.c): gtracing.ini, the PC's defaults */
void gt_settings_load(const char *exe_dir);   /* before the log is open: the file, then the defaults */
void gt_settings_log(void);                   /* once it is open: their "[ini]" lines */
const char *gt_ini_path(void);                /* the file read (and written by DISPLAY_MENU) */
void gt_ini_forget_env(void);                 /* its keys out of the environment: a restart reads it afresh */
/* A player's setting as a number.  Missing, blank or not a number (spaces around it allowed): def.  Outside
   lo .. hi: the nearer end.  Never NaN or infinite.  rt_parse_* take the text itself. */
int rt_setting_int(const char *key, int def, int lo, int hi);
float rt_setting_float(const char *key, float def, float lo, float hi);
int rt_parse_int(const char *s, int def, int lo, int hi);
float rt_parse_float(const char *s, float def, float lo, float hi);

/* crashes (crashdump.c) */
extern int gt_game_running;                /* set by main once the game's own code starts: a crash has a dump */
void gt_dump_arm(const char *log);         /* gtracing.dmp goes next to that log */
__attribute__((noreturn)) void gt_crash(int code, const char *why);
void gt_crash_test_tick(u32 frame);        /* GT_CRASH_TEST (development build) */
int gt_selftest(void);                     /* selftest.c: --selftest */

/* GT_DEV (host/build.py): 1 in a development build, 0 in a release build (build.py --release).
   A release build reads no diagnostic key and writes no diagnostic trace.  dev_getenv() is a constant NULL there
   and dev_log() compiles to if (0), so the compiler still checks the arguments but neither the key's name nor the
   format string is in the release exe.  Settings a player sets (gtracing.ini, the GT_*=0 opt-outs) use getenv(). */
#ifndef GT_DEV
#define GT_DEV 0
#endif
#define dev_getenv(k) (GT_DEV ? getenv(k) : (char *)0)
#define dev_log(...) do { if (GT_DEV) rt_log(__VA_ARGS__); } while (0)
#if GT_DEV
extern int rt_trace;                       /* --trace or GT_TRACE: log import calls */
#else
#define rt_trace 0
#endif

/* loader (loader.c) */
int gt_load(const char *exe_path, char *err, size_t errsz);
extern u32 gt_entry, gt_mh_header;

/* Guest memory helpers (1:1 mapping). */
#define GP(a) ((void *)(uintptr_t)(u32)(a))
#define GA(p) ((u32)(uintptr_t)(p))
static inline u32 rd32(u32 a) { return *(ua32 *)GP(a); }
static inline void wr32(u32 a, u32 v) { *(ua32 *)GP(a) = v; }

#endif
