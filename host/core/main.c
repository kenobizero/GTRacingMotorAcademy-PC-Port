/* main.c -- the GT Racing host (stage 3): loads the iPad game's image, binds its imports and
   runs the recompiled code from the Mach-O entry point (start -> main -> UIApplicationMain).

   usage: gtracing.exe [--app <path to GTRacingFreemiumHD.app>] [--trace]

   Android: SDL's activity calls main (SDL_main) with no arguments.  The game folder and the port's files (home\,
   gtracing.ini, gtracing.log) are in GTRacing/ on the shared storage when the app may write there (All files
   access), else in the app's own folder (Android/data/<package>/files). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#if GT_ANDROID
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#endif

#include "gl.h"
#include "liveblocks.h"
#include "loader.h"
#include "ns.h"
#include "rt.h"
#include "ui.h"

static char app_dir[MAX_PATH], exe_dir[MAX_PATH];

void gt_objc_init(void);           /* objc.c: the runtime and the root classes */
void gt_objc_image(void);          /* objc.c: classes, categories, selectors of the image */
void gt_objc_load(void);           /* objc.c: +load methods */
#ifdef _WIN32
extern char gt_reserve_start[];    /* gt.ld / reserve.S: the first byte of the game's range */
#else
extern char *gt_reserve_start;     /* fault_android.c: mapped as the library loads, NULL if it could not be */
int gt_reserve_error(void);
void gt_fault_install(void);
static char app_dir2[MAX_PATH];    /* Android: the app's own folder, where the game may be too */
#endif

static int is_dir(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* a game folder counts only with the game's program in it.  The release zip carries an empty
   GTRacingFreemiumHD.app\ holding a note (as the Gangstar port's), so a player sees where the game goes; that
   placeholder is skipped.  A copy put one level too deep (GTRacingFreemiumHD.app\GTRacingFreemiumHD.app\, an easy
   slip when dropping the game into the placeholder) is taken as well. */
static int has_game(const char *dir)
{
    char p[MAX_PATH * 3];
    if (snprintf(p, sizeof p, "%s" GT_SEP_S GT_GAME_NAME, dir) >= (int)sizeof p)
        return 0;
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int take_app(const char *path)
{
    char tmp[MAX_PATH * 2];
    GetFullPathNameA(path, sizeof app_dir, app_dir, NULL);
    if (!is_dir(app_dir))
        return 0;
    if (has_game(app_dir))
        return 1;
    snprintf(tmp, sizeof tmp, "%s" GT_SEP_S GT_GAME_NAME ".app", app_dir);
    if (is_dir(tmp) && has_game(tmp)) {
        GetFullPathNameA(tmp, sizeof app_dir, app_dir, NULL);
        return 1;
    }
    return -1;                                  /* the folder is there, the game is not */
}

/* 1 found; 0 no game folder; -1 only an empty one (the release's placeholder) */
static int find_app(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--app"))
            return take_app(argv[i + 1]);
#ifdef _WIN32
    static const char *const rel[] = {"", "\\..", "\\..\\..", "\\..\\..\\.."};
#else
    static const char *const rel[] = {""};
#endif
    int empty = 0;
#if GT_ANDROID
    if (app_dir2[0]) {                          /* the app's own folder too */
        char tmp[MAX_PATH * 2];
        snprintf(tmp, sizeof tmp, "%s/" GT_GAME_NAME ".app", app_dir2);
        int r = take_app(tmp);
        if (r > 0)
            return 1;
        if (r < 0)
            empty = 1;
    }
#endif
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char tmp[MAX_PATH * 2];
        snprintf(tmp, sizeof tmp, "%s%s" GT_SEP_S GT_GAME_NAME ".app", exe_dir, rel[i]);
        int r = take_app(tmp);
        if (r > 0)
            return 1;
        if (r < 0)
            empty = 1;
    }
    return empty ? -1 : 0;
}

#ifdef _WIN32
/* faults */

/* Reads of guest addresses 0x1000..0x11000 made from a pointer the game computed (a NULL object's
   field: NULL + 0x1d84) are fine on iOS, where that is the start of __TEXT, and the game relies on it
   (applicationWillResignActive in the menus).  Windows cannot map below 64 KB, but the bytes are at
   LOWCOPY: the faulting x86 instruction is decoded, its base (or index) register moved by
   GT_LOW_DELTA, the instruction single-stepped and the register put back. */
static __thread struct { int reg, active; u32 delta, eip; } low_fix;

static DWORD *ctx_reg(CONTEXT *x, int r)
{
    switch (r) {
    case 0: return &x->Eax;
    case 1: return &x->Ecx;
    case 2: return &x->Edx;
    case 3: return &x->Ebx;
    case 4: return &x->Esp;
    case 5: return &x->Ebp;
    case 6: return &x->Esi;
    default: return &x->Edi;
    }
}

/* the operand's registers of the instruction at p: 1 if understood */
static int decode_mem(const u8 *p, int *base, int *index, int *scale, int *dest, int *dest_is_input)
{
    int two = 0;
    while (*p == 0x66 || *p == 0xF2 || *p == 0xF3 || *p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36)
        p++;
    u8 op = *p++;
    if (op == 0x0F) {
        two = 1;
        op = *p++;
    }
    *dest = -1;
    *dest_is_input = 0;
    if (!two) {
        if (op == 0x8B || op == 0x8A)
            *dest = 1;                                   /* mov r, r/m */
        else if ((op & 0xC7) <= 0x03 && (op & 0xC7) >= 0x02 && op < 0x40) {
            *dest = 1;                                   /* add/or/adc/sbb/and/sub/xor/cmp r, r/m */
            *dest_is_input = (op & 0xF8) != 0x38;        /* cmp writes nothing */
            if ((op & 0xF8) == 0x38)
                *dest = -1;
        } else if (op == 0x69 || op == 0x6B)
            *dest = 1;                                   /* imul r, r/m, imm */
        else if (!((op >= 0x80 && op <= 0x85) || op == 0xF6 || op == 0xF7 || op == 0xFF || op == 0x38 ||
                   op == 0x39 || op == 0x3A || op == 0x3B))
            return 0;
    } else {
        if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF)
            *dest = 1;                                   /* movzx / movsx */
        else if (op == 0xAF || (op >= 0x40 && op <= 0x4F)) {
            *dest = 1;                                   /* imul, cmovcc */
            *dest_is_input = 1;
        } else if (!(op == 0x10 || op == 0x12 || op == 0x16 || op == 0x28 || op == 0x2E || op == 0x2F ||
                     op == 0x6E || op == 0x6F || op == 0x7E || (op >= 0x51 && op <= 0x5F) || op == 0x5A))
            return 0;                                    /* xmm loads and arithmetic */
    }
    u8 modrm = *p++;
    int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (mod == 3)
        return 0;
    if (*dest == 1)
        *dest = reg;
    *base = rm;
    *index = -1;
    *scale = 1;
    if (rm == 4) {
        u8 sib = *p++;
        *scale = 1 << (sib >> 6);
        *index = (sib >> 3) & 7;
        if (*index == 4)
            *index = -1;
        *base = sib & 7;
        if (*base == 5 && mod == 0)
            *base = -1;
    } else if (rm == 5 && mod == 0) {
        *base = -1;
    }
    return *base >= 0 || *index >= 0;
}

static int fix_low_read(EXCEPTION_POINTERS *e, u32 addr)
{
    CONTEXT *x = e->ContextRecord;
    int base, index, scale, dest, dest_in;
    if (!decode_mem((const u8 *)(uintptr_t)x->Eip, &base, &index, &scale, &dest, &dest_in))
        return 0;
    int r = base >= 0 ? base : index;
    u32 delta = base >= 0 ? GT_LOW_DELTA : GT_LOW_DELTA / (u32)scale;
    if (r == dest && dest_in)
        return 0;                                        /* add eax, [eax+...]: cannot be undone */
    static u32 seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == x->Eip)
            break;
        if (!seen[i]) {
            seen[i] = x->Eip;
            CPU *c = rt_cpu();
            rt_log("[low] the game read guest 0x%05x (a NULL pointer's field, readable on iOS) at host 0x%08lx, "
                   "lr 0x%08x\n", addr, x->Eip, c->r[14]);
            break;
        }
    }
    *ctx_reg(x, r) += delta;
    low_fix.reg = r == dest ? -1 : r;                    /* overwritten by the load itself */
    low_fix.delta = delta;
    low_fix.active = 1;
    low_fix.eip = x->Eip;
    x->EFlags |= 0x100;                                  /* trap after this one instruction */
    return 1;
}

void gt_dump_note(EXCEPTION_POINTERS *ep);
int gt_dump_is_writer(void);
__attribute__((noreturn)) void gt_crash(int code, const char *why);

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (gt_dump_is_writer())
        return EXCEPTION_CONTINUE_SEARCH;               /* dbghelp's own, while the crash dump is written */
    if (code == EXCEPTION_SINGLE_STEP && low_fix.active) {
        low_fix.active = 0;
        if (low_fix.reg >= 0)
            *ctx_reg(e->ContextRecord, low_fix.reg) -= low_fix.delta;
        e->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->ExceptionInformation[0] == 0) {
        u32 a = (u32)e->ExceptionRecord->ExceptionInformation[1];
        if (a >= GT_LOW_START && a < GT_LOW_END && fix_low_read(e, a))
            return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    u32 eip = (u32)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    static char why[512];
    gt_dump_note(e);
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        u32 addr = (u32)e->ExceptionRecord->ExceptionInformation[1];
        const char *g = gt_guard_name(addr);
        const char *imp = rt_addr_name(addr);
        const char *what = e->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute" :
                           e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read";
        rt_log("\n[FAULT] %s of 0x%08x at host 0x%08x%s%s%s%s\n", what, addr, eip,
               g ? " -- missing data import " : "", g ? g : "", imp ? " -- import address of " : "",
               imp ? imp : "");
        snprintf(why, sizeof why, "The game crashed: a bad memory %s at 0x%08x (host 0x%08x).", what, addr, eip);
    } else {
        rt_log("\n[FAULT] exception 0x%08lx at host 0x%08x\n", code, eip);
        snprintf(why, sizeof why, "The game crashed: %s (exception 0x%08lx at host 0x%08x).",
                 code == EXCEPTION_STACK_OVERFLOW ? "a stack overflow" :
                 code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "a division by zero" : "an illegal instruction",
                 code, eip);
    }
    HMODULE mod = NULL;
    char modname[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)eip, &mod) &&
        mod != GetModuleHandleA(NULL) && GetModuleFileNameA(mod, modname, sizeof modname))
        rt_log("host address is in %s+0x%x\n", modname, eip - (u32)(uintptr_t)mod);
    CPU *c = rt_cpu();
    rt_log("guest r0-r3 %08x %08x %08x %08x  r4-r7 %08x %08x %08x %08x\n"
           "      r8-r12 %08x %08x %08x %08x %08x  sp %08x lr %08x\n", c->r[0], c->r[1], c->r[2], c->r[3],
           c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->r[9], c->r[10], c->r[11], c->r[12], c->r[13], c->r[14]);
    gles_fault_dump();
    rt_log("(the host address names the function: build\\host\\gtracing.map)\n");
    gt_crash(4, why);                                   /* crashdump.c: the dump and the box */
}
#endif

/* main */

#if GT_ANDROID
/* the port's folder: GTRacing/ on the shared storage if it can be written (the player copies the game there with
   any file manager), else the app's own folder; that one is always looked in for the game too */
static void android_dirs(void)
{
    const char *own = SDL_GetAndroidExternalStoragePath();
    if (own)
        snprintf(app_dir2, sizeof app_dir2, "%s", own);
    const char *ext = getenv("EXTERNAL_STORAGE");
    char pub[MAX_PATH];
    snprintf(pub, sizeof pub, "%s/GTRacing", ext && *ext ? ext : "/storage/emulated/0");
    mkdir(pub, 0777);
    if (access(pub, W_OK) == 0)
        snprintf(exe_dir, sizeof exe_dir, "%s", pub);
    else if (own)
        snprintf(exe_dir, sizeof exe_dir, "%s", own);
    else
        snprintf(exe_dir, sizeof exe_dir, "%s", SDL_GetAndroidInternalStoragePath());
    if (!strcmp(exe_dir, app_dir2))
        app_dir2[0] = 0;
    gt_android_set_exe_dir(exe_dir);
    chdir(exe_dir);
}
#endif

static int host_main(int argc, char **argv)
{
    char path[MAX_PATH * 2], err[512] = "";
#if GT_ANDROID
    signal(SIGPIPE, SIG_IGN);           /* a socket closed by the peer is an error to the game, as on iOS */
    android_dirs();
#else
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
    char *s = strrchr(exe_dir, GT_SEP);
    if (s)
        *s = 0;
#endif
#ifdef _WIN32
    /* DISPLAY_MENU's restart (display.c): the run that asked for it ends first (its save, its log) */
    const char *wait_pid = getenv("GT_WAIT_PID");
    if (wait_pid && *wait_pid) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(wait_pid, NULL, 10));
        if (h) {
            WaitForSingleObject(h, 20000);
            CloseHandle(h);
        }
        _putenv_s("GT_WAIT_PID", "");
    }
#endif
    gt_settings_load(exe_dir);                          /* gtracing.ini and the defaults (settings.c) */
    /* GT_LOG=<file>: the log there (two runs at once for LAN_MULTIPLAYER, a test's scratch log); GT_LOG=0: no log,
       neither the file nor the console (rt_log_open).  From the environment or gtracing.ini. */
    const char *log_env = getenv("GT_LOG");
    int log_off = log_env && !strcmp(log_env, "0");
    if (log_env && *log_env)
        snprintf(path, sizeof path, "%s", log_env);
    else
        snprintf(path, sizeof path, "%s" GT_SEP_S "gtracing.log", exe_dir);
    /* CRASH_DUMP (scrub.c): the log names no folder of the player's, from its first line */
    rt_hide_path(exe_dir, "%EXE%");
    if (getenv("GT_HOME") && *getenv("GT_HOME"))
        rt_hide_path(getenv("GT_HOME"), "%HOME%");
    rt_log_open(path);
    gt_settings_log();                                  /* the file's "[ini]" lines, scrubbed */
    gt_live_init();                                     /* LIVE_BLOCKS (liveblocks.c): GT_LIVE_BLOCKS read now */
    if (log_off)                                        /* the dump keeps its place next to the program */
        snprintf(path, sizeof path, "%s" GT_SEP_S "gtracing.log", exe_dir);
    gt_dump_arm(path);                                  /* CRASH_DUMP (crashdump.c): gtracing.dmp by the log */
#if GT_DEV
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--trace"))
            rt_trace = 1;
    if (dev_getenv("GT_TRACE"))
        rt_trace = 1;
#endif
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_fault);

    if ((u32)(uintptr_t)gt_reserve_start != GT_RESERVE_START)
        rt_fatal("the exe's reserved range starts at %p, not 0x%x (link with gt.ld)", gt_reserve_start,
                 GT_RESERVE_START);
#else
    gt_fault_install();
    {
        Dl_info di;
        if (dladdr((void *)host_main, &di) && di.dli_fbase)
            rt_log("[main] libmain.so at %p\n", di.dli_fbase);
    }
    if ((u32)(uintptr_t)gt_reserve_start != GT_RESERVE_START)
        rt_fatal("The game's memory range 0x%x..0x%x could not be mapped (%s).  This device's 32-bit processes "
                 "have something there already.", GT_RESERVE_START, GT_RESERVE_END, strerror(gt_reserve_error()));
#endif
    int found = find_app(argc, argv);
#if GT_ANDROID
    if (found <= 0)
        rt_fatal("The game was not found.  Copy the files of your own iPad game's " GT_GAME_NAME ".app (version "
                 "1.3.4b) into the folder %s/" GT_GAME_NAME ".app, so that %s/" GT_GAME_NAME ".app/" GT_GAME_NAME
                 " is there, then start the app again.", exe_dir, exe_dir);
#endif
    if (found < 0)
        rt_fatal("The " GT_GAME_NAME ".app folder next to gtracing.exe has no game in it.  Copy the files of "
                 "your own iPad game's " GT_GAME_NAME ".app into it (see the note in that folder).");
    if (!found)
        rt_fatal("no " GT_GAME_NAME ".app next to the program (or give --app <folder>)");
    rt_hide_path(app_dir, "%APP%");
    snprintf(path, sizeof path, "%s" GT_SEP_S GT_GAME_NAME, app_dir);

    RtThread *t = rt_thread_new(8u << 20);
    CPU *c = &t->cpu;
    gt_paths_init(app_dir, exe_dir);
    gt_objc_init();
    ns_init();
    ui_init();
    if (gt_load(path, err, sizeof err))
        rt_fatal("loading the game: %s", err);
    ns_fix_cfstrings();
    gt_objc_image();
    gt_objc_load();
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--selftest")) {
            return gt_selftest();
        } else if (!strcmp(argv[i], "--objc-dump") && i + 1 < argc) {
            FILE *f = fopen(argv[i + 1], "w");
            if (!f)
                rt_fatal("cannot write %s", argv[i + 1]);
            gt_objc_dump(f);
            fclose(f);
            return 0;
        }

    /* argv / envp / apple as the kernel leaves them for start: on the guest stack (the guest's
       own path for the executable, inside its bundle). */
    static char exe_path[MAX_PATH * 2];
    snprintf(exe_path, sizeof exe_path, "%s/%s", gt_guest_bundle, GT_GAME_NAME);
    static char *gargv[2], *genvp[1], *gapple[2];
    gargv[0] = exe_path;
    gapple[0] = exe_path;
    static u32 vars_argc;
    vars_argc = 1;
    (void)vars_argc;

    gt_game_running = 1;                                /* a crash from here on has a dump (crashdump.c) */

    /* C++ and other static initializers, in order, as dyld runs them before start. */
    for (u32 i = 0; i < gt_ninit; i++) {
        u32 fn = rd32(gt_init_funcs + 4 * i);
        if (rt_trace)
            rt_log("[init] %u/%u 0x%08x\n", i + 1, gt_ninit, fn);
        u32 args[4] = {1, GA(gargv), GA(genvp), GA(gapple)};
        rt_guest_call(c, fn, 4, args);
    }
    rt_log("[main] %u initializers done; starting the game at 0x%x\n", gt_ninit, gt_entry);

    u32 sp = (c->r[13] - 64) & ~7u;
    u32 w[] = {1, GA(exe_path), 0, 0, GA(exe_path), 0};
    for (unsigned i = 0; i < sizeof w / sizeof *w; i++)
        wr32(sp + 4 * i, w[i]);
    c->r[13] = sp;
    c->r[14] = 0x0FFF0000u;
    rt_call_indirect(c, gt_entry);
    rt_log("[main] start returned\n");
    return 0;
}

#if GT_ANDROID
/* SDL's activity runs main on a Java thread with a small stack; the recompiled frames are larger than the
   game's own (Windows gives the main thread 64 MB, build.py --stack), so the host runs on a thread of that size */
typedef struct { int argc; char **argv; int rc; } MainArgs;
static void *main_thread(void *p)
{
    MainArgs *a = p;
    a->rc = host_main(a->argc, a->argv);
    return NULL;
}
int main(int argc, char **argv)
{
    MainArgs a = {argc, argv, 0};
    pthread_attr_t at;
    pthread_t t;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    if (pthread_create(&t, &at, main_thread, &a)) {
        pthread_attr_destroy(&at);
        return host_main(argc, argv);
    }
    pthread_attr_destroy(&at);
    pthread_join(t, NULL);
    return a.rc;
}
#else
int main(int argc, char **argv) { return host_main(argc, argv); }
#endif
