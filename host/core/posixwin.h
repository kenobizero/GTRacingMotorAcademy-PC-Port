/* posixwin.h -- the Win32 calls the host makes, on POSIX (Android's bionic).  Included through plat.h only.

   Only what the host uses is here, with the meaning the host relies on:
   - CRITICAL_SECTION is a recursive mutex, SRWLOCK a read-write lock (exclusive owners do not recurse, as on
     Windows), CONDITION_VARIABLE a pthread condition (SleepConditionVariableSRW waits on a mutex of its own);
   - HANDLE is a thread, an event or a waitable timer made here (posixwin.c), or a FindFirstFileA listing;
   - Interlocked* are the GCC atomics (full barriers, as on Windows);
   - times: GetTickCount / QueryPerformanceCounter from CLOCK_MONOTONIC, FILETIME from CLOCK_REALTIME;
   - files: '/' paths, attributes from stat(); the Find* calls list a folder with a "*" pattern only (the one
     the host uses: "<folder>/ *");
   - VirtualAlloc / VirtualFree / VirtualProtect / VirtualQuery are mmap / munmap / mprotect and a readability
     probe (process_vm_readv) of the page asked about. */
#ifndef GT_POSIXWIN_H
#define GT_POSIXWIN_H

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* types: as on 32-bit Windows (long is 32 bits on armeabi-v7a, so the host's "%lu" of a DWORD stays right) */
_Static_assert(sizeof(long) == 4 && sizeof(void *) == 4, "the host is a 32-bit program (armeabi-v7a)");
typedef unsigned long DWORD;
typedef int BOOL;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned int UINT;
typedef uint32_t UINT32;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint16_t WCHAR;
typedef uintptr_t ULONG_PTR;
typedef intptr_t LONG_PTR;
typedef uintptr_t SIZE_T;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef void *LPVOID;
typedef const char *LPCSTR;
typedef char *LPSTR;
typedef union { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct gt_handle *HANDLE;
typedef void *HMODULE;

#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#ifndef MAX_PATH
#define MAX_PATH 1024              /* PATH_MAX on Linux is 4096; 1024 covers app storage paths */
#endif
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define ERROR_TIMEOUT 1460u
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u

/* the C library's Microsoft names */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _putenv_s(k, v) ((v)[0] ? setenv((k), (v), 1) : unsetenv(k))
#define SecureZeroMemory(p, n) explicit_bzero_gt((p), (n))
static inline void explicit_bzero_gt(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--)
        *v++ = 0;
}

/* random words (the session keys): the system's own generator */
static inline int rand_s(unsigned int *r)
{
    *r = arc4random();
    return 0;
}

/* errors: the calling thread's last Win32-style error (set by the calls here that can time out) */
DWORD GetLastError(void);
void SetLastError(DWORD e);

/* atomics */
#define MemoryBarrier() __sync_synchronize()
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedAdd(volatile LONG *p, LONG v) { return __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG x, LONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
static inline void *InterlockedCompareExchangePointer(void *volatile *p, void *x, void *cmp)
{
    __atomic_compare_exchange_n(p, &cmp, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}
static inline void *InterlockedExchangePointer(void *volatile *p, void *x)
{
    return __atomic_exchange_n(p, x, __ATOMIC_SEQ_CST);
}

/* locks */
typedef pthread_mutex_t CRITICAL_SECTION;
void InitializeCriticalSection(CRITICAL_SECTION *cs);
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(cs); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(cs); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION *cs) { return pthread_mutex_trylock(cs) == 0; }
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_destroy(cs); }

typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void InitializeSRWLock(SRWLOCK *l) { pthread_rwlock_init(l, NULL); }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_wrlock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_rwlock_rdlock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline BOOL TryAcquireSRWLockExclusive(SRWLOCK *l) { return pthread_rwlock_trywrlock(l) == 0; }
static inline BOOL TryAcquireSRWLockShared(SRWLOCK *l) { return pthread_rwlock_tryrdlock(l) == 0; }

typedef pthread_cond_t CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT PTHREAD_COND_INITIALIZER
static inline void InitializeConditionVariable(CONDITION_VARIABLE *c) { pthread_cond_init(c, NULL); }
/* the wakes take the SRW waits' own mutex (posixwin.c), so a waiter between dropping its lock and waiting is not
   missed */
void WakeConditionVariable(CONDITION_VARIABLE *c);
void WakeAllConditionVariable(CONDITION_VARIABLE *c);
/* 0 and GetLastError() == ERROR_TIMEOUT when ms passed first */
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *c, CRITICAL_SECTION *cs, DWORD ms);
/* the SRW lock is held exclusively by the caller (flags 0) */
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, ULONG flags);

typedef struct { volatile int done; pthread_mutex_t m; } INIT_ONCE;
#define INIT_ONCE_STATIC_INIT {0, PTHREAD_MUTEX_INITIALIZER}
typedef BOOL (*PINIT_ONCE_FN)(INIT_ONCE *o, void *param, void **ctx);
static inline BOOL InitOnceBeginInitialize(INIT_ONCE *o, DWORD flags, BOOL *pending, void **ctx)
{
    (void)flags, (void)ctx;
    if (__atomic_load_n(&o->done, __ATOMIC_ACQUIRE)) {
        *pending = 0;
        return 1;
    }
    pthread_mutex_lock(&o->m);
    *pending = !o->done;
    if (!*pending)
        pthread_mutex_unlock(&o->m);
    return 1;
}
static inline BOOL InitOnceComplete(INIT_ONCE *o, DWORD flags, void *ctx)
{
    (void)flags, (void)ctx;
    __atomic_store_n(&o->done, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&o->m);
    return 1;
}
static inline BOOL InitOnceExecuteOnce(INIT_ONCE *o, PINIT_ONCE_FN fn, void *param, void **ctx)
{
    BOOL pending;
    InitOnceBeginInitialize(o, 0, &pending, ctx);
    if (pending) {
        fn(o, param, ctx);
        InitOnceComplete(o, 0, NULL);
    }
    return 1;
}

/* time */
static inline ULONGLONG gt_mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (ULONGLONG)t.tv_sec * 1000000000ull + (ULONGLONG)t.tv_nsec;
}
static inline DWORD GetTickCount(void) { return (DWORD)(gt_mono_ns() / 1000000ull); }
static inline ULONGLONG GetTickCount64(void) { return gt_mono_ns() / 1000000ull; }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *q) { q->QuadPart = (LONGLONG)gt_mono_ns(); return 1; }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000ll; return 1; }
/* 100 ns since 1601, as Windows counts */
static inline void GetSystemTimePreciseAsFileTime(FILETIME *ft)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    ULONGLONG v = ((ULONGLONG)t.tv_sec + 11644473600ull) * 10000000ull + (ULONGLONG)t.tv_nsec / 100;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}
#define GetSystemTimeAsFileTime GetSystemTimePreciseAsFileTime
static inline void Sleep(DWORD ms)
{
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}
static inline BOOL SwitchToThread(void) { sched_yield(); return 1; }
/* the thread's and the process's CPU time (kernel time is reported as 0) */
BOOL GetThreadTimes(HANDLE h, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user);
BOOL GetProcessTimes(HANDLE h, FILETIME *create, FILETIME *exit_, FILETIME *kernel, FILETIME *user);

/* threads, events, timers */
DWORD GetCurrentThreadId(void);
static inline DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE GetCurrentThread(void);           /* a pseudo handle, as on Windows */
HANDLE GetCurrentProcess(void);
typedef DWORD (*LPTHREAD_START_ROUTINE)(void *);
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u
#define CREATE_SUSPENDED 0x4u
HANDLE CreateThread(void *attr, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *id);
HANDLE CreateEventA(void *attr, BOOL manual, BOOL initial, const char *name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);
/* GetCurrentThread() made a real handle: the calling thread, joinable by WaitForSingleObject */
#define DUPLICATE_SAME_ACCESS 2u
BOOL DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *out, DWORD access, BOOL inherit, DWORD opts);
BOOL TerminateThread(HANDLE h, DWORD code);  /* not possible on POSIX: logs, returns 0 */
__attribute__((noreturn)) static inline void ExitProcess(UINT code) { _exit((int)code); }

/* the CPU count; a callback on a thread of its own (TrySubmitThreadpoolCallback: a new detached thread each time,
   the host only hands it PVRTC bands while a texture loads) */
typedef struct { DWORD dwNumberOfProcessors, dwPageSize; } SYSTEM_INFO;
static inline void GetSystemInfo(SYSTEM_INFO *si)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
    si->dwPageSize = 4096;
}
typedef void *PTP_CALLBACK_INSTANCE;
typedef void (*PTP_SIMPLE_CALLBACK)(PTP_CALLBACK_INSTANCE, void *);
BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK fn, void *arg, void *env);

/* memory */
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u
#define MEM_FREE 0x10000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_GUARD 0x100u
typedef struct {
    void *BaseAddress, *AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
} MEMORY_BASIC_INFORMATION;
void *VirtualAlloc(void *at, SIZE_T n, DWORD type, DWORD prot);
BOOL VirtualFree(void *p, SIZE_T n, DWORD type);
BOOL VirtualProtect(void *p, SIZE_T n, DWORD prot, DWORD *old);
/* the page at p: MEM_COMMIT + PAGE_READWRITE when it can be read, else MEM_FREE; RegionSize covers the
   readable pages from p's page on (up to 64 KB) */
SIZE_T VirtualQuery(const void *p, MEMORY_BASIC_INFORMATION *m, SIZE_T n);

/* files */
DWORD GetFileAttributesA(const char *p);
BOOL CreateDirectoryA(const char *p, void *attr);
static inline BOOL DeleteFileA(const char *p) { return unlink(p) == 0; }
static inline BOOL RemoveDirectoryA(const char *p) { return rmdir(p) == 0; }
static inline BOOL MoveFileA(const char *a, const char *b) { return rename(a, b) == 0; }
#define MOVEFILE_REPLACE_EXISTING 1u
#define MOVEFILE_WRITE_THROUGH 8u
static inline BOOL MoveFileExA(const char *a, const char *b, DWORD f) { (void)f; return rename(a, b) == 0; }
/* the whole path (relative paths joined to the working folder, "." and ".." resolved by realpath when it exists) */
DWORD GetFullPathNameA(const char *p, DWORD n, char *out, char **file_part);
/* the program's own path: on Android the app's files folder (set by gt_android_set_exe_dir) + "/gtracing" */
DWORD GetModuleFileNameA(HMODULE m, char *out, DWORD n);
void gt_android_set_exe_dir(const char *dir);
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    char cFileName[256];
} WIN32_FIND_DATAA;
typedef struct {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
BOOL GetFileAttributesExA(const char *p, GET_FILEEX_INFO_LEVELS level, void *out);
typedef union { struct { DWORD LowPart, HighPart; } u; ULONGLONG QuadPart; } ULARGE_INTEGER;
BOOL GetDiskFreeSpaceExA(const char *p, ULARGE_INTEGER *avail, ULARGE_INTEGER *total, ULARGE_INTEGER *free_);
BOOL CopyFileA(const char *from, const char *to, BOOL fail_if_exists);
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_FILE_NOT_FOUND 2u
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *d);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *d);
BOOL FindClose(HANDLE h);

/* text: UTF-8 is the only 8-bit code page here (CP_ACP is UTF-8 too) */
#define CP_ACP 0u
#define CP_UTF8 65001u
int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, WCHAR *w, int nw);
int WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *w, int nw, char *s, int n, const char *def, BOOL *lossy);

#endif
