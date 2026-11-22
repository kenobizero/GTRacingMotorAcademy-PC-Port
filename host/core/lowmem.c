/* lowmem.c -- a 64-bit host (arm64-v8a): everything the game can see in the process's lowest 4 GB.

   The port shares memory with the game 1:1: a guest address is a host address (rt.h GP / GA, the generated
   code's ARM_GPTR), and the game's 32-bit words hold host pointers (a string the host made, an object, a
   buffer).  On a 32-bit host every address fits.  On a 64-bit one, the host's memory has to be below 4 GB for
   that to hold; this file puts it there:
   - the host's code and data: libgame.so is loaded into a low reservation by the small libmain.so
     (android/app/jni/stub.c);
   - the heap: malloc, calloc, realloc and free of libgame.so (the host, SDL and libc++ linked into it) go
     through liveblocks.c's wrappers (lld --wrap, build.py) to this file's allocator (gt_lm_*); memalign,
     posix_memalign, aligned_alloc, malloc_usable_size, strdup and strndup are wrapped here (CMakeLists.txt).
     All on pages mapped below 4 GB; a pointer the C library allocated itself (above) is still freed by it;
   - the stacks: every thread libgame.so makes (pthread_create, also SDL's and CreateThread's) runs on a stack
     mapped below 4 GB, so the address of a local is a guest address too;
   - VirtualAlloc (posixwin.c), the fault handler's stacks (fault_android.c): gt_low_map.
   A host address above 4 GB that still reaches the game is logged with its source line (GA, rt.h: LOW_CHECK).

   The allocator: 40 size classes up to 32 KB, each block of a class carved from spans of pages, a free list per
   class (a lock each); bigger blocks are runs of 4 KB pages from a page heap (free runs coalesced, in bins by
   length; a free run of 16 MB or more has its pages given back to the system, madvise).  A table of the 4 GB's
   pages (5 MB) says what each page is.  Tested against glibc's malloc (6 threads, 1.8 million calls, every
   block's bytes checked): as fast, nothing wrong. */
#if defined(__aarch64__) && defined(__ANDROID__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "plat.h"
#include "rt.h"

#define LOW_FLOOR 0x10000000ull            /* the low maps from 256 MB (below: the game's range, what Android has) */
#define LOW_CEIL 0xFFFF0000ull

/* -- the low 4 GB's address space -- */

static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t map_cursor = LOW_FLOOR;
static u64 low_mapped;                     /* bytes mapped here, for the log */

/* the first gap of n bytes at or above from (and below LOW_CEIL) in /proc/self/maps; 0 none.  read() into a
   buffer of its own: no allocation (the allocator calls it) */
static uintptr_t find_gap(uintptr_t from, size_t n)
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    static char buf[16384];
    char line[256];
    size_t ln = 0;
    uintptr_t prev_end = 0, found = 0;
    for (;;) {
        ssize_t k = read(fd, buf, sizeof buf);
        if (k <= 0)
            break;
        for (ssize_t j = 0; j < k && !found; j++) {
            char ch = buf[j];
            if (ch != '\n') {
                if (ln < sizeof line - 1)
                    line[ln++] = ch;
                continue;
            }
            line[ln] = 0;
            ln = 0;
            unsigned long lo = 0, hi = 0;
            if (sscanf(line, "%lx-%lx", &lo, &hi) != 2)
                continue;
            uintptr_t s = prev_end > from ? prev_end : from;
            if (lo > s && lo - s >= n && s + n <= LOW_CEIL)
                found = s;
            if (hi > prev_end)
                prev_end = hi;
            if (prev_end >= LOW_CEIL)
                break;
        }
        if (found || prev_end >= LOW_CEIL)
            break;
    }
    close(fd);
    if (!found) {
        uintptr_t s = prev_end > from ? prev_end : from;
        if (s + n <= LOW_CEIL)
            found = s;
    }
    return found;
}

void *gt_low_map_prot(size_t n, int prot)
{
    n = (n + 0xFFF) & ~(size_t)0xFFF;
    pthread_mutex_lock(&map_lock);
    for (int attempt = 0; attempt < 8; attempt++) {
        uintptr_t at = map_cursor;
        if (at + n > LOW_CEIL)
            at = map_cursor = find_gap(LOW_FLOOR, n);
        if (!at)
            break;
        void *p = mmap((void *)at, n, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == (void *)at) {
            map_cursor = at + n;
            low_mapped += n;
            pthread_mutex_unlock(&map_lock);
            return p;
        }
        if (p != MAP_FAILED)
            munmap(p, n);                  /* a kernel that took the flag as a hint */
        uintptr_t g = find_gap(at, n);
        if (!g)
            g = find_gap(LOW_FLOOR, n);
        if (!g)
            break;
        map_cursor = g;
    }
    pthread_mutex_unlock(&map_lock);
    static int told;
    if (!told++)
        rt_log("[lowmem] no room below 4 GB for %zu bytes (%llu MB mapped there)\n", n,
               (unsigned long long)(low_mapped >> 20));
    return NULL;
}

void *gt_low_map(size_t n) { return gt_low_map_prot(n, PROT_READ | PROT_WRITE); }

/* LOW_CHECK (rt.h GA): a host address above 4 GB given to the game -- the first 32 places, with their lines */
void gt_ga_high(const void *p, const char *file, int line)
{
    static struct { const char *f; int l; } seen[32];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i].f == file && seen[i].l == line)
            return;
    if (n < 32) {
        seen[n].f = file, seen[n].l = line;
        n++;
        rt_log("[lowmem] LOW_CHECK: a host address above 4 GB (%p) given to the game at %s:%d\n", p, file, line);
    }
}

/* -- the page table -- */

#define PG 4096u
#define NPG (1u << 20)                     /* the pages of 4 GB */
#define NCLASS 40
#define K_LARGE 0xFE                       /* a big block's first page */
#define K_LARGE_IN 0xFC                    /* a big block's other pages */
#define K_FREE 0xFD                        /* a free run's first and last page */
static u8 pg_kind[NPG];                    /* 0 not the heap's, 1..NCLASS a class's span */
static u32 pg_aux[NPG];                    /* span page: the span's first page; big block / free run: its length */

static inline u32 class_of(size_t n)        /* n 1..32768: the class's index */
{
    if (n <= 128)
        return (u32)((n - 1) >> 4);
    u32 e = 63u - (u32)__builtin_clzll((unsigned long long)(n - 1));
    return 8 + (e - 7) * 4 + (u32)((n - 1) >> (e - 2)) - 4;
}

static inline u32 class_size(u32 i)
{
    if (i < 8)
        return 16 * (i + 1);
    u32 g = (i - 8) / 4, j = (i - 8) % 4;
    return (128u << g) + (j + 1) * (32u << g);
}

/* -- the page heap -- */

typedef struct Run { struct Run *prev, *next; } Run;
#define NBIN 128
static Run *bins[NBIN + 1];                /* runs of k pages (k < NBIN); bins[NBIN]: longer */
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 heap_pages;

static inline Run *run_at(u32 pg) { return (Run *)(uintptr_t)((u64)pg * PG); }
static inline int bin_of(u32 len) { return len < NBIN ? (int)len : NBIN; }

static void run_link(u32 pg, u32 len)
{
    pg_kind[pg] = pg_kind[pg + len - 1] = K_FREE;
    pg_aux[pg] = pg_aux[pg + len - 1] = len;
    Run *r = run_at(pg);
    int b = bin_of(len);
    r->prev = NULL;
    r->next = bins[b];
    if (bins[b])
        bins[b]->prev = r;
    bins[b] = r;
}

static void run_unlink(u32 pg, u32 len)
{
    Run *r = run_at(pg);
    if (r->prev)
        r->prev->next = r->next;
    else
        bins[bin_of(len)] = r->next;
    if (r->next)
        r->next->prev = r->prev;
}

/* a free run back, joined with the free runs on either side */
static void heap_give_locked(u32 pg, u32 len)
{
    if (pg > 0 && pg_kind[pg - 1] == K_FREE) {
        u32 l2 = pg_aux[pg - 1], p2 = pg - l2;
        run_unlink(p2, l2);
        pg = p2, len += l2;
    }
    if (pg + len < NPG && pg_kind[pg + len] == K_FREE) {
        u32 l2 = pg_aux[pg + len];
        run_unlink(pg + len, l2);
        len += l2;
    }
    run_link(pg, len);
}

static int heap_grow_locked(u32 len)
{
    u32 n = len > 16384 ? len : 16384;     /* 64 MB at a time */
    u8 *p = gt_low_map((size_t)n * PG);
    if (!p && n > len)
        p = gt_low_map((size_t)(n = len) * PG);
    if (!p)
        return 0;
    heap_pages += n;
    heap_give_locked((u32)((uintptr_t)p / PG), n);
    return 1;
}

/* a run of len pages: its first page, 0 none */
static u32 heap_take(u32 len, u8 kind)
{
    pthread_mutex_lock(&heap_lock);
    for (int tries = 0; tries < 2; tries++) {
        u32 pg = 0, have = 0;
        for (int b = bin_of(len); b < NBIN && !pg; b++)
            if (bins[b]) {
                pg = (u32)((uintptr_t)bins[b] / PG);
                have = (u32)b;
            }
        if (!pg)
            for (Run *r = bins[NBIN]; r; r = r->next) {   /* the longer ones: the shortest that fits */
                u32 p = (u32)((uintptr_t)r / PG), l = pg_aux[p];
                if (l >= len && (!pg || l < have))
                    pg = p, have = l;
            }
        if (pg) {
            run_unlink(pg, have);
            if (have > len)
                run_link(pg + len, have - len);
            pg_kind[pg] = kind;
            pg_aux[pg] = len;
            if (kind == K_LARGE) {
                if (len > 1)
                    pg_kind[pg + len - 1] = K_LARGE_IN;
            }
            pthread_mutex_unlock(&heap_lock);
            return pg;
        }
        if (!heap_grow_locked(len))
            break;
    }
    pthread_mutex_unlock(&heap_lock);
    return 0;
}

static void heap_give(u32 pg, u32 len)
{
    if (len >= 4096)                       /* 16 MB and more: the pages back to the system (the first kept); smaller
                                              runs keep theirs (given back and touched again, they cost a fault a
                                              page: a stress test 3x slower than bionic's own at 1 MB) */
        madvise(run_at(pg + 1), (size_t)(len - 1) * PG, MADV_DONTNEED);
    pthread_mutex_lock(&heap_lock);
    heap_give_locked(pg, len);
    pthread_mutex_unlock(&heap_lock);
}

/* -- the classes -- */

typedef struct {
    pthread_mutex_t lock;
    void *free;
    u8 *bump, *end;
} Class;
static Class classes[NCLASS] = {[0 ... NCLASS - 1] = {PTHREAD_MUTEX_INITIALIZER, NULL, NULL, NULL}};

static void *small_alloc(u32 ci)
{
    Class *k = &classes[ci];
    u32 size = class_size(ci);
    pthread_mutex_lock(&k->lock);
    void *p = k->free;
    if (p)
        k->free = *(void **)p;
    else {
        if (!k->bump || k->bump + size > k->end) {
            u32 pages = (8 * size + PG - 1) / PG;
            if (pages < 16)
                pages = 16;
            u32 pg = heap_take(pages, (u8)(ci + 1));
            if (!pg) {
                pthread_mutex_unlock(&k->lock);
                return NULL;
            }
            for (u32 j = 0; j < pages; j++)
                pg_kind[pg + j] = (u8)(ci + 1), pg_aux[pg + j] = pg;
            k->bump = (u8 *)run_at(pg);
            k->end = k->bump + (size_t)pages * PG;
        }
        p = k->bump;
        k->bump += size;
    }
    pthread_mutex_unlock(&k->lock);
    return p;
}

/* a small block's own start (an aligned pointer inside it: memalign) */
static inline u8 *small_start(uintptr_t a, u32 ci)
{
    uintptr_t s = (uintptr_t)pg_aux[a / PG] * PG;
    return (u8 *)(a - (a - s) % class_size(ci));
}

static void small_free(void *p, u32 ci)
{
    Class *k = &classes[ci];
    pthread_mutex_lock(&k->lock);
    *(void **)p = k->free;
    k->free = p;
    pthread_mutex_unlock(&k->lock);
}

/* -- the wrapped calls -- */

void *__real_malloc(size_t n);
void __real_free(void *p);
void *__real_realloc(void *p, size_t n);
size_t __real_malloc_usable_size(const void *p);

static void *large_alloc(size_t n, u32 align)
{
    u32 pages = (u32)((n + PG - 1) / PG), extra = align > PG ? align / PG - 1 : 0;
    if ((u64)n > 0xF0000000ull)
        return NULL;
    u32 pg = heap_take(pages + extra, K_LARGE);
    if (!pg)
        return NULL;
    if (extra) {                           /* aligned: the pages before and after back */
        u32 total = pages + extra, apg = (u32)((((u64)pg * PG + align - 1) & ~((u64)align - 1)) / PG);
        u32 front = apg - pg, back = total - front - pages;
        pg_kind[apg] = K_LARGE, pg_aux[apg] = pages;
        if (pages > 1)
            pg_kind[apg + pages - 1] = K_LARGE_IN;
        if (front) {
            pg_kind[pg] = K_LARGE_IN;
            pthread_mutex_lock(&heap_lock);
            heap_give_locked(pg, front);
            pthread_mutex_unlock(&heap_lock);
        }
        if (back) {
            pthread_mutex_lock(&heap_lock);
            heap_give_locked(apg + pages, back);
            pthread_mutex_unlock(&heap_lock);
        }
        pg = apg;
    }
    return run_at(pg);
}

static int ours(const void *p, u8 *kind)
{
    uintptr_t a = (uintptr_t)p;
    if (!p || a >> 32)
        return 0;
    *kind = pg_kind[a / PG];
    return *kind != 0 && *kind != K_FREE && *kind != K_LARGE_IN;
}

void *gt_lm_malloc(size_t n)
{
    if (!n)
        n = 1;
    void *p = n <= 32768 ? small_alloc(class_of(n)) : large_alloc(n, 0);
    if (!p)
        errno = ENOMEM;
    return p;
}

void gt_lm_free(void *p)
{
    u8 k;
    if (!p)
        return;
    if (!ours(p, &k)) {
        __real_free(p);                    /* the C library's own */
        return;
    }
    if (k <= NCLASS)
        small_free(small_start((uintptr_t)p, k - 1u), k - 1u);
    else
        heap_give((u32)((uintptr_t)p / PG), pg_aux[(uintptr_t)p / PG]);
}

size_t __wrap_malloc_usable_size(const void *p)
{
    u8 k;
    if (!p)
        return 0;
    if (!ours(p, &k))
        return __real_malloc_usable_size(p);
    uintptr_t a = (uintptr_t)p;
    if (k <= NCLASS)
        return class_size(k - 1u) - (size_t)(a - (uintptr_t)small_start(a, k - 1u));
    return (size_t)pg_aux[a / PG] * PG;
}

void *gt_lm_calloc(size_t a, size_t b)
{
    size_t n;
    if (__builtin_mul_overflow(a, b, &n)) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = gt_lm_malloc(n);
    if (p)
        memset(p, 0, n ? n : 1);
    return p;
}

void *gt_lm_realloc(void *p, size_t n)
{
    u8 k;
    if (!p)
        return gt_lm_malloc(n);
    if (!ours(p, &k))
        return __real_realloc(p, n);
    if (!n) {
        gt_lm_free(p);
        return NULL;
    }
    size_t have = __wrap_malloc_usable_size(p);
    if (n <= have && (k <= NCLASS || n > have / 2))
        return p;
    void *q = gt_lm_malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, n < have ? n : have);
    gt_lm_free(p);
    return q;
}

static void *aligned(size_t align, size_t n)
{
    if (align <= 16)
        return gt_lm_malloc(n);
    if (align & (align - 1))
        return NULL;
    if (align <= 2048 && n + align <= 32768) {
        u8 *raw = small_alloc(class_of(n + align - 1));
        return raw ? (void *)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1)) : NULL;
    }
    return align > 0x80000000u ? NULL : large_alloc(n ? n : 1, (u32)align);
}

void *__wrap_memalign(size_t align, size_t n) { return aligned(align, n); }
void *__wrap_aligned_alloc(size_t align, size_t n) { return aligned(align, n); }
int __wrap_posix_memalign(void **out, size_t align, size_t n)
{
    if (align < sizeof(void *) || (align & (align - 1)))
        return EINVAL;
    void *p = aligned(align, n);
    if (!p)
        return ENOMEM;
    *out = p;
    return 0;
}

char *__wrap_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = gt_lm_malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

char *__wrap_strndup(const char *s, size_t m)
{
    size_t n = strnlen(s, m);
    char *d = gt_lm_malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

/* -- the threads' stacks -- */

int __real_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);

typedef struct {
    void *(*fn)(void *);
    void *arg;
    u8 *map;
    size_t total;
} Tramp;
static pthread_key_t tramp_key;
static pthread_once_t tramp_once = PTHREAD_ONCE_INIT;
static struct { u8 *map; size_t total; u64 at; } zombies[64];
static pthread_mutex_t zombie_lock = PTHREAD_MUTEX_INITIALIZER;

static u64 now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000 + (u64)t.tv_nsec / 1000000;
}

/* a stack no longer used, kept to be used again once its thread has surely gone (2 s) */
static void stack_put(u8 *map, size_t total, u64 at)
{
    pthread_mutex_lock(&zombie_lock);
    for (int i = 0; i < 64; i++)
        if (!zombies[i].map) {
            zombies[i].map = map, zombies[i].total = total, zombies[i].at = at;
            map = NULL;
            break;
        }
    pthread_mutex_unlock(&zombie_lock);
    (void)map;                             /* no slot: the stack stays mapped (rare) */
}

static u8 *stack_get(size_t total)
{
    u64 t = now_ms();
    pthread_mutex_lock(&zombie_lock);
    for (int i = 0; i < 64; i++)
        if (zombies[i].map && zombies[i].total == total && t - zombies[i].at > 2000) {
            u8 *m = zombies[i].map;
            zombies[i].map = NULL;
            pthread_mutex_unlock(&zombie_lock);
            return m;
        }
    pthread_mutex_unlock(&zombie_lock);
    u8 *m = gt_low_map(total);
    if (m)
        mprotect(m, PG, PROT_NONE);        /* the guard page below the stack */
    return m;
}

static void tramp_done(void *v)
{
    Tramp *t = v;
    stack_put(t->map, t->total, now_ms());
    gt_lm_free(t);
}

static void tramp_key_make(void) { pthread_key_create(&tramp_key, tramp_done); }

static void *tramp_start(void *v)
{
    Tramp *t = v;
    pthread_setspecific(tramp_key, t);     /* tramp_done when the thread ends (return or pthread_exit) */
    return t->fn(t->arg);
}

int __wrap_pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    pthread_once(&tramp_once, tramp_key_make);
    pthread_attr_t a;
    size_t sz = 1u << 20;
    if (attr) {
        void *ustack = NULL;
        size_t usz = 0;
        a = *attr;
        pthread_attr_getstack(&a, &ustack, &usz);
        if (ustack)                        /* the caller's own stack */
            return __real_pthread_create(th, attr, fn, arg);
        pthread_attr_getstacksize(&a, &sz);
    } else
        pthread_attr_init(&a);
    size_t total = ((sz + PG - 1) & ~(size_t)(PG - 1)) + PG;
    u8 *map = stack_get(total);
    Tramp *t = map ? gt_lm_malloc(sizeof *t) : NULL;
    if (!t) {
        if (map)
            stack_put(map, total, 0);
        static int told;
        if (!told++)
            rt_log("[lowmem] a thread's stack could not be put below 4 GB\n");
        return __real_pthread_create(th, attr, fn, arg);
    }
    t->fn = fn, t->arg = arg, t->map = map, t->total = total;
    pthread_attr_setstack(&a, map + PG, total - PG);
    int r = __real_pthread_create(th, &a, tramp_start, t);
    if (r) {
        stack_put(map, total, 0);
        gt_lm_free(t);
    }
    return r;
}

/* the log's line at the end of the start (main.c) */
void gt_lowmem_report(void)
{
    rt_log("[lowmem] 64-bit host: the game's memory below 4 GB (%llu MB mapped there, %llu MB of it the heap)\n",
           (unsigned long long)(low_mapped >> 20), (unsigned long long)(heap_pages * PG >> 20));
}
#endif
