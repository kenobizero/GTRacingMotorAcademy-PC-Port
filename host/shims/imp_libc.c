/* imp_libc.c -- the C library the game imports (libSystem): process, memory, strings, stdio,
   files, time, ctype, compiler helpers.  Most map onto the host CRT; this file has the ones
   whose iOS behaviour or layout differs (struct stat / tm, open flags, RAND_MAX, errno...). */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
/* the Microsoft CRT's names, on POSIX */
#include <unistd.h>
#define _O_BINARY 0
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_APPEND O_APPEND
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_EXCL O_EXCL
#define _S_IREAD 0444
#define _S_IWRITE 0222
#define _S_IFDIR S_IFDIR
#define _open open
#define _read read
#define _write write
#define _close close
#define _lseeki64 lseek64
#define _stati64 stat64
#define _fstati64 fstat64
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "plat.h"

#include "abi.h"
#include "rt.h"
#include "net.h"
#include "game_hooks.h"

#define S(a) ((const char *)GP(a))
#define W(a) ((char *)GP(a))

/* errno (Darwin numbers) */

static u32 darwin_errno(int e)
{
    switch (e) {
    case EAGAIN: return 35;
    case EDEADLK: return 11;
    case ENAMETOOLONG: return 63;
    case ENOTEMPTY: return 66;
    case ENOSYS: return 78;
    case ETIMEDOUT: return 60;
    default: return e <= 34 ? (u32)e : 22;
    }
}

static void gt_set_errno(int host_errno) { rt_thread()->errno_cell = darwin_errno(host_errno); }
u32 h_error(CPU *c) { return GA(&((RtThread *)c->rt)->errno_cell); }

/* process */

void h_exit(CPU *c, u32 code)
{
    (void)c;
    rt_log("[exit] the game exits with code %u\n", code);
    fflush(NULL);
    ExitProcess(code);
}

void h_abort(CPU *c) { rt_fatal("the game called abort() (return address 0x%08x)", c->r[14]); }

void h_assert_rtn(u32 func, u32 file, u32 line, u32 expr)
{
    rt_fatal("assertion failed: %s, function %s, file %s, line %u", expr ? S(expr) : "?",
             func ? S(func) : "?", file ? S(file) : "?", line);
}

/* no variable is set for the game (HOME, TMPDIR, USER ...): it finds its folders through NSSearchPathFor... and
   never sees the PC's environment (the Windows user's name) */
u32 h_getenv(u32 name)
{
    (void)name;
    return 0;
}

u32 h_system(u32 cmd) { (void)cmd; return (u32)-1; }
u32 h_sleep(u32 s) { Sleep(s * 1000); return 0; }
u32 h_usleep(u32 us) { Sleep(us >= 1000 ? us / 1000 : (us ? 1 : 0)); return 0; }
u32 h_alarm(u32 s) { (void)s; return 0; }
u32 h_zero1(u32 a) { (void)a; return 0; }
u32 h_getpid(void) { return GetCurrentProcessId(); }
u32 h_getuid(void) { return 501; }
u32 h_getpwuid(u32 uid) { (void)uid; return 0; }
u32 h_gethostname(u32 buf, u32 n)
{
    if (n)
        snprintf(W(buf), n, "iPad");
    return 0;
}
u32 h_signal(u32 sig, u32 fn) { (void)sig; (void)fn; return 0; }
u32 h_sigaction(u32 sig, u32 act, u32 old)
{
    (void)sig; (void)act;
    if (old)
        memset(GP(old), 0, 12);
    return 0;
}

/* memory */

u32 h_memcpy_chk(u32 d, u32 s, u32 n, u32 cap) { (void)cap; return GA(memcpy(GP(d), GP(s), n)); }
u32 h_memmove_chk(u32 d, u32 s, u32 n, u32 cap) { (void)cap; return GA(memmove(GP(d), GP(s), n)); }
u32 h_memset_chk(u32 d, u32 v, u32 n, u32 cap) { (void)cap; return GA(memset(GP(d), (int)v, n)); }
void h_bzero(u32 d, u32 n) { memset(GP(d), 0, n); }
void h_memset_pattern16(u32 d, u32 pat, u32 n)
{
    u8 *p = GP(d);
    const u8 *q = GP(pat);
    for (u32 i = 0; i < n; i++)
        p[i] = q[i & 15];
}

/* strings */

u32 h_strcpy_chk(u32 d, u32 s, u32 cap) { (void)cap; return GA(strcpy(W(d), S(s))); }
u32 h_strcat_chk(u32 d, u32 s, u32 cap) { (void)cap; return GA(strcat(W(d), S(s))); }
u32 h_strlcpy(u32 d, u32 s, u32 n)
{
    size_t len = strlen(S(s));
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(W(d), S(s), k);
        W(d)[k] = 0;
    }
    return (u32)len;
}
u32 h_strlcat(u32 d, u32 s, u32 n)
{
    size_t dl = strnlen(S(d), n), sl = strlen(S(s));
    if (dl == n)
        return (u32)(n + sl);
    size_t k = sl < n - dl - 1 ? sl : n - dl - 1;
    memcpy(W(d) + dl, S(s), k);
    W(d)[dl + k] = 0;
    return (u32)(dl + sl);
}
u32 h_strcasecmp(u32 a, u32 b) { return (u32)_stricmp(S(a), S(b)); }
u32 h_strncasecmp(u32 a, u32 b, u32 n) { return (u32)_strnicmp(S(a), S(b), n); }
u32 h_strdup(u32 s) { return GA(_strdup(S(s))); }
u32 h_strtok_r(u32 s, u32 delim, u32 save)
{
    char *ctx = s ? NULL : (char *)GP(rd32(save));
    char *r = strtok_s(s ? W(s) : NULL, S(delim), &ctx);
    wr32(save, GA(ctx));
    return GA(r);
}
u32 h_strerror(u32 e)
{
    static char buf[64];
    snprintf(buf, sizeof buf, "error %u", e);
    return GA(buf);
}
u32 h_strerror_r(u32 e, u32 buf, u32 n)
{
    if (n)
        snprintf(W(buf), n, "error %u", e);
    return 0;
}
u32 h_basename(u32 p)
{
    static char buf[MAX_PATH];
    const char *s = S(p), *a = strrchr(s, '/'), *b = strrchr(s, '\\');
    if (b > a)
        a = b;
    snprintf(buf, sizeof buf, "%s", a ? a + 1 : s);
    return GA(buf);
}

/* ctype (Darwin's rune table, dataimp.c) */

u32 gt_runetype(int ch);
u32 h_maskrune(u32 ch, u32 mask) { return gt_runetype((int)ch) & mask; }
u32 h_tolower(u32 ch) { return ch < 128 ? (u32)tolower((int)ch) : ch; }
u32 h_toupper(u32 ch) { return ch < 128 ? (u32)toupper((int)ch) : ch; }

/* random numbers (RAND_MAX is 0x7fffffff on iOS) */

static u32 rand_next = 1;
static u32 do_rand(u32 *ctx)
{
    /* FreeBSD / Darwin rand(): Park-Miller "minimal standard" */
    s32 hi, lo, x = (s32)(*ctx % 0x7ffffffe) + 1;
    hi = x / 127773;
    lo = x % 127773;
    x = 16807 * lo - 2836 * hi;
    if (x < 0)
        x += 0x7fffffff;
    *ctx = (u32)x - 1;
    return (u32)x - 1;
}
u32 h_rand(void) { return do_rand(&rand_next); }
void h_srand(u32 s) { rand_next = s; }
static u32 random_state = 1;
u32 h_random(void) { return do_rand(&random_state); }
void h_srandom(u32 s) { random_state = s; }
void h_srandomdev(void) { random_state = GetTickCount() ^ GetCurrentProcessId(); }
u32 h_arc4random(void)
{
    static u32 s = 0;
    if (!s)
        s = GetTickCount() * 2654435761u | 1;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

/* qsort (the comparator is guest code) */

static __thread u32 qsort_cmp;
static int qsort_cb(const void *a, const void *b) { return (int)rt_call2(qsort_cmp, GA(a), GA(b)); }
void h_qsort(CPU *c, u32 base, u32 n, u32 size, u32 cmp)
{
    (void)c;
    u32 save = qsort_cmp;
    qsort_cmp = cmp;
    qsort(GP(base), n, size, qsort_cb);
    qsort_cmp = save;
}

/* stdio */

#define VA(p) ((va_list)(void *)(p))

/* The game's console output (stdout and stderr, which ___stdoutp / ___stderrp hand it, dataimp.c) goes through the
   log: each line is scrubbed of the player's folders and names (scrub.c), and GT_LOG=0 silences it as it does the
   host's own lines: nothing is formatted, locked or written then (rt_vlog returns at once; printf's count is
   still the formatted length).  The same holds for write() to descriptors 1 and 2 and for NSLog (ns_string.c).
   The game's i386 va_list is a plain pointer to its words, so it can be read twice. */
static int console(u32 f) { return GP(f) == stdout || GP(f) == stderr; }
#ifdef _WIN32
static u32 console_vf(const char *fmt, va_list ap)
{
    int n = rt_vlog(fmt, ap);
    return (u32)(n >= 0 ? n : vsnprintf(NULL, 0, fmt, ap));
}
#endif
static u32 console_n(const void *p, size_t n)
{
    if (n && rt_log_enabled())
        rt_log("%.*s", (int)(n > 0x7fffffff ? 0x7fffffff : n), (const char *)p);
    return (u32)n;
}

#ifndef _WIN32
/* Android: the host's va_list (AAPCS: a struct, 64-bit values 8-byte aligned) is not the game's (Darwin armv7:
   words, 64-bit values on any word), so the game's arguments are formatted from its words (ns_format, ns_string.c:
   the C conversions and NSString's own) and the text written. */
#include "ns.h"
static u32 gfmt(char *out, size_t n, const char *fmt, const u32 *w)
{
    Buf b = {0};
    ns_format(&b, fmt, w);
    if (out && n) {
        size_t k = b.n < n - 1 ? b.n : n - 1;
        memcpy(out, b.p ? b.p : "", k);
        out[k] = 0;
    }
    u32 len = b.n;
    buf_free(&b);
    return len;
}
static u32 gfile(FILE *f, const char *fmt, const u32 *w)
{
    Buf b = {0};
    ns_format(&b, fmt, w);
    if (b.n)
        fwrite(b.p, 1, b.n, f);
    u32 len = b.n;
    buf_free(&b);
    return len;
}
static u32 gconsole(const char *fmt, const u32 *w)
{
    if (!rt_log_enabled())
        return gfmt(NULL, 0, fmt, w);
    Buf b = {0};
    ns_format(&b, fmt, w);
    if (b.n)
        rt_log("%.*s", (int)b.n, b.p);
    u32 len = b.n;
    buf_free(&b);
    return len;
}
u32 h_printf(u32 fmt, const u32 *va) { return gconsole(S(fmt), va); }
u32 h_vprintf(u32 fmt, u32 va) { return gconsole(S(fmt), GP(va)); }
u32 h_fprintf(u32 f, u32 fmt, const u32 *va) { return console(f) ? gconsole(S(fmt), va) : gfile(GP(f), S(fmt), va); }
u32 h_vfprintf(u32 f, u32 fmt, u32 va)
{
    return console(f) ? gconsole(S(fmt), GP(va)) : gfile(GP(f), S(fmt), GP(va));
}
#else
u32 h_printf(u32 fmt, const u32 *va) { return console_vf(S(fmt), VA(va)); }
u32 h_vprintf(u32 fmt, u32 va) { return console_vf(S(fmt), VA(GP(va))); }
u32 h_fprintf(u32 f, u32 fmt, const u32 *va)
{
    return console(f) ? console_vf(S(fmt), VA(va)) : (u32)vfprintf(GP(f), S(fmt), VA(va));
}
u32 h_vfprintf(u32 f, u32 fmt, u32 va)
{
    return console(f) ? console_vf(S(fmt), VA(GP(va))) : (u32)vfprintf(GP(f), S(fmt), VA(GP(va)));
}
#endif
u32 h_puts(u32 s)
{
    rt_log("%s\n", S(s));
    return 1;
}
u32 h_putchar(u32 ch)
{
    char b = (char)ch;
    console_n(&b, 1);
    return ch & 0xff;
}
void h_perror(u32 s) { rt_log("%s%s%s\n", s ? S(s) : "", s && *S(s) ? ": " : "", strerror(errno)); }
u32 h_fwrite(u32 p, u32 size, u32 n, u32 f)
{
    if (!console(f))
        return (u32)fwrite(GP(p), size, n, GP(f));
    console_n(GP(p), (size_t)size * n);
    return n;
}
u32 h_fputc(u32 ch, u32 f)
{
    if (!console(f))
        return (u32)fputc((int)ch, GP(f));
    char b = (char)ch;
    console_n(&b, 1);
    return ch & 0xff;
}
u32 h_fputs(u32 s, u32 f)
{
    if (!console(f))
        return (u32)fputs(S(s), GP(f));
    rt_log("%s", S(s));
    return 1;
}
#ifndef _WIN32
u32 h_sprintf(u32 buf, u32 fmt, const u32 *va) { return gfmt(W(buf), (size_t)-1, S(fmt), va); }
u32 h_snprintf(u32 buf, u32 n, u32 fmt, const u32 *va) { return gfmt(W(buf), n, S(fmt), va); }
u32 h_vsprintf(u32 buf, u32 fmt, u32 va) { return gfmt(W(buf), (size_t)-1, S(fmt), GP(va)); }
u32 h_vsnprintf(u32 buf, u32 n, u32 fmt, u32 va) { return gfmt(W(buf), n, S(fmt), GP(va)); }
u32 h_sprintf_chk(u32 buf, u32 flag, u32 cap, u32 fmt, const u32 *va)
{
    (void)flag;
    return gfmt(W(buf), cap, S(fmt), va);
}
u32 h_snprintf_chk(u32 buf, u32 n, u32 flag, u32 cap, u32 fmt, const u32 *va)
{
    (void)flag; (void)cap;
    return gfmt(W(buf), n, S(fmt), va);
}
u32 h_vsprintf_chk(u32 buf, u32 flag, u32 cap, u32 fmt, u32 va)
{
    (void)flag;
    return gfmt(W(buf), cap, S(fmt), GP(va));
}
/* scanf: every argument is a pointer (one word), so the words are passed as they are; unused ones are ignored */
#define SCAN_ARGS(w) GP((w)[0]), GP((w)[1]), GP((w)[2]), GP((w)[3]), GP((w)[4]), GP((w)[5]), GP((w)[6]), \
                     GP((w)[7]), GP((w)[8]), GP((w)[9]), GP((w)[10]), GP((w)[11]), GP((w)[12]), GP((w)[13]), \
                     GP((w)[14]), GP((w)[15])
u32 h_sscanf(u32 s, u32 fmt, const u32 *va) { return (u32)sscanf(S(s), S(fmt), SCAN_ARGS(va)); }
u32 h_fscanf(u32 f, u32 fmt, const u32 *va) { return (u32)fscanf(GP(f), S(fmt), SCAN_ARGS(va)); }
#else
u32 h_sprintf(u32 buf, u32 fmt, const u32 *va) { return (u32)vsprintf(W(buf), S(fmt), VA(va)); }
u32 h_snprintf(u32 buf, u32 n, u32 fmt, const u32 *va) { return (u32)vsnprintf(W(buf), n, S(fmt), VA(va)); }
u32 h_vsprintf(u32 buf, u32 fmt, u32 va) { return (u32)vsprintf(W(buf), S(fmt), VA(GP(va))); }
u32 h_vsnprintf(u32 buf, u32 n, u32 fmt, u32 va) { return (u32)vsnprintf(W(buf), n, S(fmt), VA(GP(va))); }
u32 h_sprintf_chk(u32 buf, u32 flag, u32 cap, u32 fmt, const u32 *va)
{
    (void)flag;
    return (u32)vsnprintf(W(buf), cap, S(fmt), VA(va));
}
u32 h_snprintf_chk(u32 buf, u32 n, u32 flag, u32 cap, u32 fmt, const u32 *va)
{
    (void)flag; (void)cap;
    return (u32)vsnprintf(W(buf), n, S(fmt), VA(va));
}
u32 h_vsprintf_chk(u32 buf, u32 flag, u32 cap, u32 fmt, u32 va)
{
    (void)flag;
    return (u32)vsnprintf(W(buf), cap, S(fmt), VA(GP(va)));
}
u32 h_sscanf(u32 s, u32 fmt, const u32 *va) { return (u32)vsscanf(S(s), S(fmt), VA(va)); }
u32 h_fscanf(u32 f, u32 fmt, const u32 *va) { return (u32)vfscanf(GP(f), S(fmt), VA(va)); }
#endif

/* Game paths: absolute host paths (NSBundle / NSHomeDirectory give those), '/' or '\'. */
const char *gt_host_path(const char *p, char *buf, size_t n);   /* ns_misc.c: guest path -> host path */

u32 h_fopen(u32 path, u32 mode)
{
    char p[MAX_PATH * 2], m[16];
    snprintf(m, sizeof m - 1, "%s", S(mode));
    if (!strchr(m, 'b'))
        strcat(m, "b");                     /* no text mode on iOS */
    FILE *f = fopen(gt_host_path(S(path), p, sizeof p), m);
    softrefl_note_file(p);                  /* SOFT_REFLECTION: the track loaded */
    if (!f)
        gt_set_errno(errno);
    if (rt_trace)
        rt_log("[file] fopen %s %s -> %p\n", p, m, (void *)f);
    return GA(f);
}
u32 h_fseek(u32 f, u32 off, u32 whence) { return (u32)fseek(GP(f), (long)(s32)off, (int)whence); }
u32 h_setvbuf(u32 f, u32 buf, u32 mode, u32 size)
{
    int m = mode == 0 ? _IOFBF : mode == 1 ? _IOLBF : _IONBF;
    return (u32)setvbuf(GP(f), buf ? W(buf) : NULL, m, size ? size : 512);
}
u32 h_remove(u32 path)
{
    char p[MAX_PATH * 2];
    return (u32)remove(gt_host_path(S(path), p, sizeof p));
}

/* file descriptors */

u32 h_open(u32 path, u32 flags, const u32 *va)
{
    int f = _O_BINARY;
    switch (flags & 3) {
    case 0: f |= _O_RDONLY; break;
    case 1: f |= _O_WRONLY; break;
    default: f |= _O_RDWR; break;
    }
    if (flags & 0x8) f |= _O_APPEND;
    if (flags & 0x200) f |= _O_CREAT;
    if (flags & 0x400) f |= _O_TRUNC;
    if (flags & 0x800) f |= _O_EXCL;
    char p[MAX_PATH * 2];
    int fd = _open(gt_host_path(S(path), p, sizeof p), f, _S_IREAD | _S_IWRITE);
    softrefl_note_file(p);                  /* SOFT_REFLECTION: the track loaded */
    (void)va;
    if (fd < 0)
        gt_set_errno(errno);
    return (u32)fd;
}
u64 h_lseek(u32 fd, u64 off, u32 whence) { return (u64)_lseeki64((int)fd, (s64)off, (int)whence); }
u32 h_read(u32 fd, u32 buf, u32 n) { return (u32)_read((int)fd, GP(buf), n); }
u32 h_write(u32 fd, u32 buf, u32 n)
{
    if (fd == 1 || fd == 2)                 /* the console: through the log, as stdout / stderr above */
        return console_n(GP(buf), n);
    return (u32)_write((int)fd, GP(buf), n);
}
/* socket fds (LAN_MULTIPLAYER, net.c) are 900..1019, apart from the CRT's */
u32 h_close(u32 fd) { return net_is_socket(fd) ? net_close(fd) : (u32)_close((int)fd); }
u32 h_fcntl(u32 fd, u32 cmd, const u32 *va) { return net_is_socket(fd) ? net_fcntl(fd, cmd, va ? va[0] : 0) : 0; }
u32 h_ioctl(u32 fd, u32 req, const u32 *va) { return net_is_socket(fd) ? net_ioctl(fd, req, va ? va[0] : 0) : (u32)-1; }

/* Darwin's (32-bit inode) struct stat, 96 bytes. */
static void put_stat(u32 buf, const struct _stati64 *st)
{
    u8 *b = GP(buf);
    memset(b, 0, 96);
    u32 mode = (st->st_mode & _S_IFDIR ? 0040000 : 0100000) | (st->st_mode & _S_IWRITE ? 0644 : 0444);
    if (st->st_mode & _S_IFDIR)
        mode |= 0111;
    *(u32 *)(b + 0) = st->st_dev;
    *(u32 *)(b + 4) = 1;
    *(u16 *)(b + 8) = (u16)mode;
    *(u16 *)(b + 10) = 1;
    *(u32 *)(b + 12) = 501;
    *(u32 *)(b + 16) = 501;
    *(u32 *)(b + 24) = (u32)st->st_atime;
    *(u32 *)(b + 32) = (u32)st->st_mtime;
    *(u32 *)(b + 40) = (u32)st->st_ctime;
    *(u64 *)(b + 48) = (u64)st->st_size;
    *(u64 *)(b + 56) = ((u64)st->st_size + 511) / 512;
    *(u32 *)(b + 64) = 4096;
}
u32 h_stat(u32 path, u32 buf)
{
    char p[MAX_PATH * 2];
    struct _stati64 st;
    gt_host_path(S(path), p, sizeof p);
    size_t n = strlen(p);
    while (n > 3 && (p[n - 1] == '/' || p[n - 1] == '\\'))
        p[--n] = 0;                         /* _stat fails on a trailing slash */
    if (_stati64(p, &st)) {
        gt_set_errno(errno);
        return (u32)-1;
    }
    put_stat(buf, &st);
    return 0;
}
u32 h_fstat(u32 fd, u32 buf)
{
    struct _stati64 st;
    if (_fstati64((int)fd, &st)) {
        gt_set_errno(errno);
        return (u32)-1;
    }
    put_stat(buf, &st);
    return 0;
}
u32 h_statfs(u32 path, u32 buf)
{
    (void)path;
    u8 *b = GP(buf);
    memset(b, 0, 348);
    *(u32 *)(b + 0) = 4096;                 /* f_bsize */
    *(u32 *)(b + 4) = 4096;                 /* f_iosize */
    *(u64 *)(b + 8) = 1u << 22;             /* f_blocks: 16 GB */
    *(u64 *)(b + 16) = 1u << 21;            /* f_bfree */
    *(u64 *)(b + 24) = 1u << 21;            /* f_bavail */
    return 0;
}
/* getcwd, chdir: ns_misc.c (the guest's own working directory) */
u32 h_utimes(u32 p, u32 t) { (void)p; (void)t; return 0; }
u32 h_setxattr(u32 p, u32 n, u32 v, u32 s, u32 pos, u32 opt) { (void)p; (void)n; (void)v; (void)s; (void)pos; (void)opt; return 0; }

/* time */

u32 h_time(u32 t)
{
    u32 now = (u32)time(NULL);
    if (t)
        wr32(t, now);
    return now;
}

static void filetime_now(u64 *us)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    u64 v = ((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10;
    *us = v - 11644473600000000ull;          /* since 1970 */
}

u32 h_gettimeofday(u32 tv, u32 tz)
{
    u64 us;
    filetime_now(&us);
    if (tv) {
        wr32(tv, (u32)(us / 1000000));
        wr32(tv + 4, (u32)(us % 1000000));
    }
    if (tz) {
        wr32(tz, 0);
        wr32(tz + 4, 0);
    }
    return 0;
}

double h_CFAbsoluteTimeGetCurrent(void)
{
    u64 us;
    filetime_now(&us);
    return (double)us / 1e6 - 978307200.0;  /* since 2001-01-01 */
}

u64 h_mach_absolute_time(void)
{
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    return (u64)((double)q.QuadPart * 1e9 / (double)f.QuadPart);   /* nanoseconds: timebase 1/1 */
}
u32 h_mach_timebase_info(u32 info)
{
    wr32(info, 1);
    wr32(info + 4, 1);
    return 0;
}
u32 h_mach_thread_self(void) { return GetCurrentThreadId(); }

/* Darwin struct tm: the nine ints, then long tm_gmtoff and char *tm_zone (44 bytes). */
static void put_tm(u32 out, const struct tm *t, long gmtoff, const char *zone)
{
    u32 *o = GP(out);
    o[0] = (u32)t->tm_sec; o[1] = (u32)t->tm_min; o[2] = (u32)t->tm_hour; o[3] = (u32)t->tm_mday;
    o[4] = (u32)t->tm_mon; o[5] = (u32)t->tm_year; o[6] = (u32)t->tm_wday; o[7] = (u32)t->tm_yday;
    o[8] = (u32)t->tm_isdst; o[9] = (u32)gmtoff; o[10] = GA(zone);
}
u32 h_localtime_r(u32 tp, u32 out)
{
    time_t t = (time_t)(s32)rd32(tp);
    struct tm tm, g;
    localtime_s(&tm, &t);
    gmtime_s(&g, &t);
    long off = (long)(mktime(&tm) - mktime(&g));
    put_tm(out, &tm, off, "LOC");
    return out;
}
u32 h_localtime(u32 tp)
{
    static u32 buf[11];
    return h_localtime_r(tp, GA(buf));
}
u32 h_gmtime_r(u32 tp, u32 out)
{
    time_t t = (time_t)(s32)rd32(tp);
    struct tm tm;
    gmtime_s(&tm, &t);
    put_tm(out, &tm, 0, "UTC");
    return out;
}

/* compiler helpers (division by zero gives 0, as the game never relies on a trap) */

u32 h_divsi3(u32 a, u32 b) { return b ? (u32)((s32)a == INT32_MIN && (s32)b == -1 ? a : (u32)((s32)a / (s32)b)) : 0; }
u32 h_modsi3(u32 a, u32 b) { return b && !((s32)a == INT32_MIN && (s32)b == -1) ? (u32)((s32)a % (s32)b) : 0; }
u32 h_udivsi3(u32 a, u32 b) { return b ? a / b : 0; }
u32 h_umodsi3(u32 a, u32 b) { return b ? a % b : 0; }
u64 h_divdi3(u64 a, u64 b) { return b ? (u64)((s64)a / (s64)b) : 0; }
u64 h_udivdi3(u64 a, u64 b) { return b ? a / b : 0; }
u64 h_umoddi3(u64 a, u64 b) { return b ? a % b : 0; }
u64 h_fixdfdi(double x)
{
    if (x != x) return 0;
    if (x >= 9223372036854775808.0) return 0x7fffffffffffffffull;
    if (x <= -9223372036854775808.0) return 0x8000000000000000ull;
    return (u64)(s64)x;
}
u64 h_fixunsdfdi(double x)
{
    if (!(x > 0)) return 0;
    if (x >= 18446744073709551616.0) return ~0ull;
    return (u64)x;
}
double h_floatdidf(u64 x) { return (double)(s64)x; }
double h_floatundidf(u64 x) { return (double)x; }
float h_floatundisf(u64 x) { return (float)x; }

u32 h_OSAtomicAdd32(u32 amount, u32 p) { return (u32)InterlockedAdd((volatile LONG *)GP(p), (LONG)amount); }

/* math the CRT spells differently */

u32 h_lrintf(float x) { return (u32)(s32)lrintf(x); }
float h_modff(float x, u32 ip)
{
    float i;
    float r = modff(x, &i);
    wr32(ip, f2bits(i));
    return r;
}
