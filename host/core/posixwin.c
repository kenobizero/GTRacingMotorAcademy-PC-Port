/* posixwin.c -- the parts of posixwin.h (the Win32 calls on POSIX) that keep state.  Empty on Windows. */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/statvfs.h>
#include <sys/uio.h>

#include "plat.h"

static __thread DWORD last_error;
DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD e) { last_error = e; }

/* locks */

void InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &a);
    pthread_mutexattr_destroy(&a);
}

static void deadline(struct timespec *t, DWORD ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *cs, DWORD ms)
{
    int r;
    if (ms == INFINITE)
        r = pthread_cond_wait(c, cs);
    else {
        struct timespec t;
        deadline(&t, ms);
        r = pthread_cond_timedwait(c, cs, &t);
    }
    if (r == ETIMEDOUT) {
        last_error = ERROR_TIMEOUT;
        return 0;
    }
    return 1;
}

/* An SRW lock is a pthread rwlock, which a condition cannot wait on: the wait drops it, waits on a mutex of its
   own (taken before the drop, so a wake made after the drop is not lost) and takes it again. */
static pthread_mutex_t srw_cv_mutex = PTHREAD_MUTEX_INITIALIZER;
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, ULONG flags)
{
    (void)flags;
    int r;
    pthread_mutex_lock(&srw_cv_mutex);
    pthread_rwlock_unlock(l);
    if (ms == INFINITE)
        r = pthread_cond_wait(c, &srw_cv_mutex);
    else {
        struct timespec t;
        deadline(&t, ms);
        r = pthread_cond_timedwait(c, &srw_cv_mutex, &t);
    }
    pthread_mutex_unlock(&srw_cv_mutex);
    pthread_rwlock_wrlock(l);
    if (r == ETIMEDOUT) {
        last_error = ERROR_TIMEOUT;
        return 0;
    }
    return 1;
}
/* The waker takes srw_cv_mutex around the wake (it does not know which kind of wait it ends), so an SRW waiter
   between its unlock and its wait cannot miss it.  A CS waiter's wake is the same with the extra lock. */
void WakeConditionVariable(CONDITION_VARIABLE *c)
{
    pthread_mutex_lock(&srw_cv_mutex);
    pthread_cond_signal(c);
    pthread_mutex_unlock(&srw_cv_mutex);
}
void WakeAllConditionVariable(CONDITION_VARIABLE *c)
{
    pthread_mutex_lock(&srw_cv_mutex);
    pthread_cond_broadcast(c);
    pthread_mutex_unlock(&srw_cv_mutex);
}

/* handles */

enum { H_THREAD = 1, H_EVENT, H_FIND, H_PSEUDO_THREAD, H_PSEUDO_PROCESS };

struct gt_handle {
    int kind;
    volatile int refs;                 /* the creator's handle + the running thread */
    pthread_mutex_t m;
    pthread_cond_t cv;
    int signaled, manual;              /* events; threads: signaled when they end */
    pthread_t th;
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    DIR *dir;                          /* H_FIND */
    char dirpath[MAX_PATH];
};

static struct gt_handle pseudo_thread = {.kind = H_PSEUDO_THREAD};
static struct gt_handle pseudo_process = {.kind = H_PSEUDO_PROCESS};
HANDLE GetCurrentThread(void) { return &pseudo_thread; }
HANDLE GetCurrentProcess(void) { return &pseudo_process; }

DWORD GetCurrentThreadId(void) { return (DWORD)gettid(); }

static HANDLE h_new(int kind)
{
    HANDLE h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->kind = kind;
    h->refs = 1;
    pthread_mutex_init(&h->m, NULL);
    pthread_cond_init(&h->cv, NULL);
    return h;
}

static void h_unref(HANDLE h)
{
    if (__atomic_sub_fetch(&h->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        pthread_mutex_destroy(&h->m);
        pthread_cond_destroy(&h->cv);
        free(h);
    }
}

static void h_signal(HANDLE h)
{
    pthread_mutex_lock(&h->m);
    h->signaled = 1;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->m);
}

static void *thread_start(void *p)
{
    HANDLE h = p;
    h->fn(h->arg);
    h_signal(h);
    h_unref(h);
    return NULL;
}

HANDLE CreateThread(void *attr, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id)
{
    (void)attr;
    (void)flags;
    HANDLE h = h_new(H_THREAD);
    if (!h)
        return NULL;
    h->fn = fn;
    h->arg = arg;
    h->refs = 2;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   /* waited for through the handle */
    if (stack)
        pthread_attr_setstacksize(&a, (stack + 0xFFFFu) & ~(SIZE_T)0xFFFFu);
    int r = pthread_create(&h->th, &a, thread_start, h);
    pthread_attr_destroy(&a);
    if (r) {
        free(h);
        return NULL;
    }
    if (id)
        *id = 0;                       /* the new thread's id is its own GetCurrentThreadId() */
    return h;
}

/* the calling thread as a handle another thread can wait for: signaled by gt_thread_handle_done (never, for the
   main thread, which ends with the process) */
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD opts)
{
    (void)sp, (void)tp, (void)access, (void)inherit, (void)opts;
    if (src != &pseudo_thread) {
        *out = NULL;
        return 0;
    }
    HANDLE h = h_new(H_THREAD);
    if (h)
        h->th = pthread_self();
    *out = h;
    return h != NULL;
}

BOOL TerminateThread(HANDLE h, DWORD code)
{
    (void)h, (void)code;
    return 0;
}

HANDLE CreateEventA(void *attr, BOOL manual, BOOL initial, const char *name)
{
    (void)attr, (void)name;
    HANDLE h = h_new(H_EVENT);
    if (h) {
        h->manual = manual;
        h->signaled = initial != 0;
    }
    return h;
}

BOOL SetEvent(HANDLE h)
{
    if (!h || h->kind != H_EVENT)
        return 0;
    h_signal(h);
    return 1;
}

BOOL ResetEvent(HANDLE h)
{
    if (!h || h->kind != H_EVENT)
        return 0;
    pthread_mutex_lock(&h->m);
    h->signaled = 0;
    pthread_mutex_unlock(&h->m);
    return 1;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!h || h == INVALID_HANDLE_VALUE || (h->kind != H_THREAD && h->kind != H_EVENT))
        return WAIT_FAILED;
    struct timespec t;
    if (ms != INFINITE)
        deadline(&t, ms);
    DWORD r = WAIT_OBJECT_0;
    pthread_mutex_lock(&h->m);
    while (!h->signaled) {
        if (ms == 0) {
            r = WAIT_TIMEOUT;
            break;
        }
        int e = ms == INFINITE ? pthread_cond_wait(&h->cv, &h->m) : pthread_cond_timedwait(&h->cv, &h->m, &t);
        if (e == ETIMEDOUT && !h->signaled) {
            r = WAIT_TIMEOUT;
            break;
        }
    }
    if (r == WAIT_OBJECT_0 && h->kind == H_EVENT && !h->manual)
        h->signaled = 0;
    pthread_mutex_unlock(&h->m);
    return r;
}

BOOL CloseHandle(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE || h == &pseudo_thread || h == &pseudo_process)
        return 0;
    if (h->kind == H_FIND)
        return FindClose(h);
    h_unref(h);
    return 1;
}

typedef struct { PTP_SIMPLE_CALLBACK fn; void *arg; } PoolCall;
static void *pool_start(void *p)
{
    PoolCall c = *(PoolCall *)p;
    free(p);
    c.fn(NULL, c.arg);
    return NULL;
}
BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK fn, void *arg, void *env)
{
    (void)env;
    PoolCall *c = malloc(sizeof *c);
    if (!c)
        return 0;
    c->fn = fn;
    c->arg = arg;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, pool_start, c);
    pthread_attr_destroy(&a);
    if (r) {
        free(c);
        return 0;
    }
    return 1;
}

/* CPU time */

static void ns_to_ft(ULONGLONG ns, FILETIME *f)
{
    ULONGLONG v = ns / 100;
    f->dwLowDateTime = (DWORD)v;
    f->dwHighDateTime = (DWORD)(v >> 32);
}
static BOOL cpu_times(clockid_t id, FILETIME *c, FILETIME *e, FILETIME *k, FILETIME *u)
{
    struct timespec t;
    if (clock_gettime(id, &t))
        return 0;
    memset(c, 0, sizeof *c);
    memset(e, 0, sizeof *e);
    memset(k, 0, sizeof *k);
    ns_to_ft((ULONGLONG)t.tv_sec * 1000000000ull + (ULONGLONG)t.tv_nsec, u);
    return 1;
}
BOOL GetThreadTimes(HANDLE h, FILETIME *c, FILETIME *e, FILETIME *k, FILETIME *u)
{
    (void)h;
    return cpu_times(CLOCK_THREAD_CPUTIME_ID, c, e, k, u);
}
BOOL GetProcessTimes(HANDLE h, FILETIME *c, FILETIME *e, FILETIME *k, FILETIME *u)
{
    (void)h;
    return cpu_times(CLOCK_PROCESS_CPUTIME_ID, c, e, k, u);
}

/* memory */

static void *gt_va_note(void *p, SIZE_T n);

static int prot_of(DWORD p)
{
    switch (p & 0xFF) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}

void *VirtualAlloc(void *at, SIZE_T n, DWORD type, DWORD prot)
{
    if (at && (type & MEM_COMMIT) && !(type & MEM_RESERVE)) {
        /* committing inside a range already reserved (MAP_FIXED over our own mapping) */
        return mprotect(at, n, prot_of(prot)) ? NULL : at;
    }
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (at)
        flags |= MAP_FIXED_NOREPLACE;
    void *p = mmap(at, n, (type & MEM_COMMIT) ? prot_of(prot) : PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    if (at && p != at) {              /* an old kernel without MAP_FIXED_NOREPLACE gave another address */
        munmap(p, n);
        return NULL;
    }
    return gt_va_note(p, n);
}

/* VirtualFree(p, 0, MEM_RELEASE) needs the size: kept for each block VirtualAlloc made without one given back */
#define VA_MAX 4096
static struct { void *p; SIZE_T n; } va_blocks[VA_MAX];
static pthread_mutex_t va_lock = PTHREAD_MUTEX_INITIALIZER;
static void *gt_va_note(void *p, SIZE_T n)
{
    pthread_mutex_lock(&va_lock);
    for (int i = 0; i < VA_MAX; i++)
        if (!va_blocks[i].p) {
            va_blocks[i].p = p;
            va_blocks[i].n = n;
            break;
        }
    pthread_mutex_unlock(&va_lock);
    return p;
}

BOOL VirtualFree(void *p, SIZE_T n, DWORD type)
{
    (void)type;
    if (!n) {
        pthread_mutex_lock(&va_lock);
        for (int i = 0; i < VA_MAX; i++)
            if (va_blocks[i].p == p) {
                n = va_blocks[i].n;
                va_blocks[i].p = NULL;
                break;
            }
        pthread_mutex_unlock(&va_lock);
        if (!n)
            return 0;
    }
    return munmap(p, n) == 0;
}

BOOL VirtualProtect(void *p, SIZE_T n, DWORD prot, DWORD *old)
{
    if (old)
        *old = PAGE_READWRITE;
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)0xFFF;
    return mprotect((void *)a, n + ((uintptr_t)p - a), prot_of(prot)) == 0;
}

/* one page readable?  process_vm_readv on ourselves reads it without a fault: EFAULT when it cannot be read */
static int page_readable(uintptr_t a)
{
    char b;
    struct iovec local = {&b, 1}, remote = {(void *)a, 1};
    return syscall(__NR_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL) == 1;
}

SIZE_T VirtualQuery(const void *p, MEMORY_BASIC_INFORMATION *m, SIZE_T n)
{
    if (n < sizeof *m)
        return 0;
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)0xFFF;
    memset(m, 0, sizeof *m);
    m->BaseAddress = m->AllocationBase = (void *)a;
    if (!page_readable(a)) {
        m->State = MEM_FREE;
        m->Protect = PAGE_NOACCESS;
        m->RegionSize = 0x1000;
        return sizeof *m;
    }
    SIZE_T size = 0x1000;
    while (size < 0x10000 && a + size > a && page_readable(a + size))
        size += 0x1000;
    m->State = MEM_COMMIT;
    m->Protect = m->AllocationProtect = PAGE_READWRITE;
    m->RegionSize = size;
    return sizeof *m;
}

/* files */

DWORD GetFileAttributesA(const char *p)
{
    struct stat st;
    if (!p || stat(p, &st))
        return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL CreateDirectoryA(const char *p, void *attr)
{
    (void)attr;
    if (mkdir(p, 0777) == 0)
        return 1;
    last_error = errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_FILE_NOT_FOUND;
    return 0;
}

static void time_to_ft(time_t t, FILETIME *f)
{
    ULONGLONG v = ((ULONGLONG)t + 11644473600ull) * 10000000ull;
    f->dwLowDateTime = (DWORD)v;
    f->dwHighDateTime = (DWORD)(v >> 32);
}

BOOL GetFileAttributesExA(const char *p, GET_FILEEX_INFO_LEVELS level, void *out)
{
    (void)level;
    struct stat st;
    if (stat(p, &st)) {
        last_error = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA *d = out;
    memset(d, 0, sizeof *d);
    d->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    d->nFileSizeLow = (DWORD)st.st_size;
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    time_to_ft(st.st_mtime, &d->ftLastWriteTime);
    time_to_ft(st.st_ctime, &d->ftCreationTime);
    time_to_ft(st.st_atime, &d->ftLastAccessTime);
    return 1;
}

BOOL GetDiskFreeSpaceExA(const char *p, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_)
{
    struct statvfs v;
    if (statvfs(p, &v))
        return 0;
    if (avail)
        avail->QuadPart = (ULONGLONG)v.f_bavail * v.f_frsize;
    if (total)
        total->QuadPart = (ULONGLONG)v.f_blocks * v.f_frsize;
    if (free_)
        free_->QuadPart = (ULONGLONG)v.f_bfree * v.f_frsize;
    return 1;
}

BOOL CopyFileA(const char *from, const char *to, BOOL fail_if_exists)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return 0;
    int out = open(to, O_WRONLY | O_CREAT | (fail_if_exists ? O_EXCL : O_TRUNC), 0666);
    if (out < 0) {
        close(in);
        return 0;
    }
    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) {
            ok = 0;
            break;
        }
    if (n < 0)
        ok = 0;
    close(in);
    close(out);
    return ok;
}

DWORD GetFullPathNameA(const char *p, DWORD n, char *out, char **file_part)
{
    char tmp[PATH_MAX], cwd[PATH_MAX];
    if (file_part)
        *file_part = NULL;
    if (realpath(p, tmp) == NULL) {
        if (p[0] == '/' || !getcwd(cwd, sizeof cwd))
            snprintf(tmp, sizeof tmp, "%s", p);
        else
            snprintf(tmp, sizeof tmp, "%s/%s", cwd, p);
    }
    size_t len = strlen(tmp);
    if (len + 1 > n)
        return (DWORD)(len + 1);
    memcpy(out, tmp, len + 1);
    return (DWORD)len;
}

static char exe_dir[MAX_PATH] = ".";
void gt_android_set_exe_dir(const char *dir) { snprintf(exe_dir, sizeof exe_dir, "%s", dir); }
DWORD GetModuleFileNameA(HMODULE m, char *out, DWORD n)
{
    (void)m;
    int len = snprintf(out, n, "%s/gtracing", exe_dir);
    return len < 0 || (DWORD)len >= n ? 0 : (DWORD)len;
}

static BOOL find_fill(HANDLE h, WIN32_FIND_DATAA *d)
{
    struct dirent *e;
    while ((e = readdir(h->dir))) {
        memset(d, 0, sizeof *d);
        snprintf(d->cFileName, sizeof d->cFileName, "%s", e->d_name);
        char full[MAX_PATH * 2];
        snprintf(full, sizeof full, "%s/%s", h->dirpath, e->d_name);
        struct stat st;
        if (stat(full, &st))
            continue;
        d->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        d->nFileSizeLow = (DWORD)st.st_size;
        d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        ULONGLONG v = ((ULONGLONG)st.st_mtime + 11644473600ull) * 10000000ull;
        d->ftLastWriteTime.dwLowDateTime = (DWORD)v;
        d->ftLastWriteTime.dwHighDateTime = (DWORD)(v >> 32);
        return 1;
    }
    return 0;
}

/* pattern: "<folder>/<name pattern>"; only "*" (every entry) is used by the host, other patterns are
   matched as an exact name */
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *d)
{
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", pattern);
    char *s = strrchr(dir, '/');
    const char *name = "*";
    if (s) {
        *s = 0;
        name = s + 1;
    }
    if (strcmp(name, "*")) {
        char full[MAX_PATH * 2];
        snprintf(full, sizeof full, "%s/%s", dir, name);
        struct stat st;
        if (stat(full, &st))
            return INVALID_HANDLE_VALUE;
        HANDLE h = h_new(H_FIND);
        if (!h)
            return INVALID_HANDLE_VALUE;
        memset(d, 0, sizeof *d);
        snprintf(d->cFileName, sizeof d->cFileName, "%s", name);
        d->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        d->nFileSizeLow = (DWORD)st.st_size;
        return h;                      /* dir NULL: FindNextFileA ends at once */
    }
    DIR *dp = opendir(s ? dir : ".");
    if (!dp)
        return INVALID_HANDLE_VALUE;
    HANDLE h = h_new(H_FIND);
    if (!h) {
        closedir(dp);
        return INVALID_HANDLE_VALUE;
    }
    h->dir = dp;
    snprintf(h->dirpath, sizeof h->dirpath, "%s", s ? dir : ".");
    if (!find_fill(h, d)) {
        FindClose(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *d)
{
    if (!h || h == INVALID_HANDLE_VALUE || h->kind != H_FIND || !h->dir)
        return 0;
    return find_fill(h, d);
}

BOOL FindClose(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE || h->kind != H_FIND)
        return 0;
    if (h->dir)
        closedir(h->dir);
    h->dir = NULL;
    h_unref(h);
    return 1;
}

/* text */

int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, WCHAR *w, int nw)
{
    (void)cp, (void)flags;
    const unsigned char *p = (const unsigned char *)s;
    size_t len = n < 0 ? strlen(s) + 1 : (size_t)n;
    int o = 0;
    for (size_t i = 0; i < len;) {
        uint32_t c = p[i];
        int k = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        if (k)
            c &= 0x3Fu >> k;
        if (k && i + (size_t)k >= len)
            break;                     /* a sequence cut short */
        for (int j = 1; j <= k; j++)
            c = c << 6 | (p[i + (size_t)j] & 0x3Fu);
        i += (size_t)k + 1;
        if (c >= 0x10000) {
            if (w && o + 1 < nw) {
                c -= 0x10000;
                w[o] = (WCHAR)(0xD800 | (c >> 10));
                w[o + 1] = (WCHAR)(0xDC00 | (c & 0x3FF));
            }
            o += 2;
        } else {
            if (w && o < nw)
                w[o] = (WCHAR)c;
            o++;
        }
    }
    if (w && nw && o > nw)
        return 0;
    return o;
}

int WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *w, int nw, char *s, int n, const char *def, BOOL *lossy)
{
    (void)cp, (void)flags, (void)def;
    if (lossy)
        *lossy = 0;
    size_t len = 0;
    if (nw < 0)
        while (w[len++]) {
        }
    else
        len = (size_t)nw;
    int o = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00u);
            i++;
        }
        char b[4];
        int k;
        if (c < 0x80)
            b[0] = (char)c, k = 1;
        else if (c < 0x800)
            b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3F)), k = 2;
        else if (c < 0x10000)
            b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 0x3F)), b[2] = (char)(0x80 | (c & 0x3F)),
            k = 3;
        else
            b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 0x3F)),
            b[2] = (char)(0x80 | ((c >> 6) & 0x3F)), b[3] = (char)(0x80 | (c & 0x3F)), k = 4;
        if (s && n) {
            if (o + k > n)
                return 0;
            memcpy(s + o, b, (size_t)k);
        }
        o += k;
    }
    return o;
}

#endif
