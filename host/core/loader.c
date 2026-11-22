/* loader.c -- maps the game image at its own addresses and binds its imports.

   The exe reserved GT_RESERVE_START..GT_RESERVE_END as its first section (reserve.S), so the
   segments are copied there; guest bytes below GT_LOW_END go to the low copy (layout.h).
   Every import gets a value for its slots:
     - data the host provides (rt_data_import: Objective-C classes, CF constants, std data...),
     - a function: a 4-byte address of its own in the import range; a call through it reaches
       the import's bridge (bridges.c) via rt_call_indirect,
     - data the host does not have: an address in a no-access guard page (the fault names it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "loader.h"
#include "macho.h"
#include "rt.h"

#define FAIL(...) do { snprintf(err, errsz, __VA_ARGS__); return -1; } while (0)

u32 gt_entry, gt_mh_header;
u32 gt_init_funcs, gt_ninit;
static macho M;
static uint8_t *file_data;

RtBridge gt_import_fn[GT_IMPORT_SIZE / 4];
const char *gt_import_name[GT_IMPORT_SIZE / 4];
unsigned gt_import_count;

static const char *guard_name[GT_DATAGUARD_SIZE / 16];
static unsigned nguard;

const macho *gt_macho(void) { return &M; }

RtBridge rt_bridge_by_name(const char *name)
{
    unsigned lo = 0, hi = rt_bridge_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int d = strcmp(rt_bridges[mid].name, name);
        if (!d)
            return rt_bridges[mid].fn;
        if (d < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

u32 rt_import_addr(const char *name)
{
    for (unsigned i = 0; i < gt_import_count; i++)
        if (!strcmp(gt_import_name[i], name))
            return GT_IMPORT_BASE + 4 * i;
    RtBridge fn = rt_bridge_by_name(name);
    if (!fn)
        return 0;
    if (gt_import_count == GT_IMPORT_SIZE / 4)
        rt_fatal("too many function imports");
    gt_import_fn[gt_import_count] = fn;
    gt_import_name[gt_import_count] = name;
    return GT_IMPORT_BASE + 4 * gt_import_count++;
}

const char *gt_guard_name(u32 addr)
{
    if (addr - GT_DATAGUARD_BASE >= GT_DATAGUARD_SIZE)
        return NULL;
    unsigned i = (addr - GT_DATAGUARD_BASE) / 16;
    return i < nguard ? guard_name[i] : "?";
}

/* Imports seen only as data words: by name, data or a function whose address is taken. */
static int data_by_name(const char *n)
{
    static const char *const prefix[] = {"_OBJC_CLASS_$_", "_OBJC_METACLASS_$_", "_OBJC_EHTYPE_",
                                         "__ZTV", "__ZTI", "__ZTS", "_k", "__NSConcrete", NULL};
    for (int i = 0; prefix[i]; i++)
        if (!strncmp(n, prefix[i], strlen(prefix[i])))
            return strcmp(prefix[i], "_k") || (n[2] >= 'A' && n[2] <= 'Z');
    static const char *const exact[] = {"___CFConstantStringClassReference", "___stderrp", "___stdinp",
                                        "___stdoutp", "__DefaultRuneLocale", "__objc_empty_cache",
                                        "__objc_empty_vtable", "__dispatch_main_q", NULL};
    for (int i = 0; exact[i]; i++)
        if (!strcmp(n, exact[i]))
            return 1;
    size_t len = strlen(n);
    static const char *const suffix[] = {"Notification", "Key", "Mode", "Zero", "Null", "Identity",
                                         "Exception", "Domain", "Property", "Label", "Invalid", "Alert",
                                         "StatusBar", "Date", "Size", "Calendar", "Code",
                                         "Artist", "Title", "Image", "Value", "Path", NULL};
    for (int i = 0; suffix[i]; i++) {
        size_t sl = strlen(suffix[i]);
        if (len > sl && !strcmp(n + len - sl, suffix[i]))
            return 1;
    }
    return 0;
}

typedef struct {
    unsigned slots, data, funcs, guards, skipped_weak;
    char *err;
    size_t errsz;
    int failed;
} bind_ctx;

static u32 resolve(bind_ctx *b, const char *name, int lazy)
{
    u32 v = lazy ? 0 : rt_data_import(name);
    if (v) {
        b->data++;
        return v;
    }
    if (lazy || !data_by_name(name)) {
        v = rt_import_addr(name);
        if (v) {
            b->funcs++;
            return v;
        }
    }
    for (unsigned i = 0; i < nguard; i++)
        if (!strcmp(guard_name[i], name))
            return GT_DATAGUARD_BASE + 16 * i;
    if (nguard == GT_DATAGUARD_SIZE / 16)
        rt_fatal("too many missing data imports");
    guard_name[nguard] = name;
    b->guards++;
    rt_log("[load] data import %s: not provided (guard 0x%08x)\n", name, GT_DATAGUARD_BASE + 16 * nguard);
    return GT_DATAGUARD_BASE + 16 * nguard++;
}

static int bind_cb(void *ctx, uint32_t slot, const char *name, int ordinal, int32_t addend, int flags, int lazy)
{
    bind_ctx *b = ctx;
    (void)ordinal;
    (void)flags;
    if (slot < GT_RESERVE_START || slot + 4 > GT_MACHO_END) {
        snprintf(b->err, b->errsz, "bind slot 0x%x for %s outside the image", slot, name);
        b->failed = 1;
        return 1;
    }
    wr32(slot, resolve(b, name, lazy) + (u32)addend);
    b->slots++;
    return 0;
}

static int bind_regular(void *ctx, uint32_t slot, const char *name, int ordinal, int32_t addend, int flags)
{
    return bind_cb(ctx, slot, name, ordinal, addend, flags, 0);
}

static int bind_lazy(void *ctx, uint32_t slot, const char *name, int ordinal, int32_t addend, int flags)
{
    return bind_cb(ctx, slot, name, ordinal, addend, flags, 1);
}

static int bind_weak(void *ctx, uint32_t slot, const char *name, int ordinal, int32_t addend, int flags)
{
    bind_ctx *b = ctx;
    if (flags & BINDF_STRONG_DEF)
        return 0;
    if (macho_lookup(&M, name)) {           /* the game's own definition: the slot already holds it */
        b->skipped_weak++;
        return 0;
    }
    return bind_cb(ctx, slot, name, ordinal, addend, flags, 0);
}

static void copy_range(u32 lo, u32 hi, const uint8_t *src)
{
    if (lo < GT_LOW_END) {
        u32 e = hi < GT_LOW_END ? hi : GT_LOW_END;
        memcpy(GP(lo + GT_LOW_DELTA), src, e - lo);
    }
    if (hi > GT_RESERVE_START) {
        u32 s = lo > GT_RESERVE_START ? lo : GT_RESERVE_START;
        memcpy(GP(s), src + (s - lo), hi - s);
    }
}

int gt_load(const char *path, char *err, size_t errsz)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        FAIL("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    file_data = malloc((size_t)sz);
    if (!file_data || fread(file_data, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        FAIL("cannot read %s", path);
    }
    fclose(f);
    if (macho_parse(&M, file_data, (uint32_t)sz, err, errsz))
        return -1;
    for (int i = 0; i < M.nsegs; i++) {
        const macho_seg *s = &M.segs[i];
        if (!strcmp(s->name, "__PAGEZERO"))
            continue;
        if (s->vmaddr < GT_LOW_START || s->vmaddr + s->vmsize > GT_MACHO_END)
            FAIL("segment %s 0x%x+0x%x does not fit the reserved range", s->name, s->vmaddr, s->vmsize);
        copy_range(s->vmaddr, s->vmaddr + s->filesize, M.data + s->fileoff);
    }
    gt_entry = M.entry;
    gt_mh_header = GT_LOWCOPY_BASE;         /* the header is at 0x1000 in the low copy */

    /* Pointers to the Mach-O header (__program_vars, the local _mh_execute_header slot). */
    unsigned mh = 0;
    for (int i = 0; i < M.nsects; i++) {
        const macho_sect *s = &M.sects[i];
        if (strcmp(s->name, "__program_vars") && strcmp(s->name, "__nl_symbol_ptr"))
            continue;
        for (u32 a = s->addr; a + 4 <= s->addr + s->size; a += 4)
            if (rd32(a) == GT_LOW_START) {
                wr32(a, GT_LOWCOPY_BASE);
                mh++;
            }
    }

    bind_ctx b = {0};
    b.err = err;
    b.errsz = errsz;
    if (macho_binds(&M, BIND_REGULAR, bind_regular, &b, err, errsz) || b.failed)
        return -1;
    if (macho_binds(&M, BIND_LAZY, bind_lazy, &b, err, errsz) || b.failed)
        return -1;
    if (macho_binds(&M, BIND_WEAK, bind_weak, &b, err, errsz) || b.failed)
        return -1;

    DWORD old;
    VirtualProtect(GP(GT_IMPORT_BASE), GT_IMPORT_SIZE + GT_DATAGUARD_SIZE + GT_HOSTFN_SIZE, PAGE_NOACCESS, &old);

    const macho_sect *init = macho_sect_named(&M, "__DATA", "__mod_init_func");
    gt_init_funcs = init ? init->addr : 0;
    gt_ninit = init ? init->size / 4 : 0;
    rt_log("[load] %s: entry 0x%x, %u bind slots (%u data, %u functions, %u missing data, %u weak "
           "kept), %u function imports, %u header pointers, %u initializers\n",
           path, gt_entry, b.slots, b.data, b.funcs, b.guards, b.skipped_weak, gt_import_count, mh, gt_ninit);
    return 0;
}
