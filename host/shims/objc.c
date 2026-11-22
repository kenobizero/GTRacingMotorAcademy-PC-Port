/* objc.c -- Objective-C 2 runtime for the recompiled game (see objc.h).

   Guest structures (armv7, clang's ObjC 2 ABI, 32-bit):
     class_t      isa, superclass, cache, vtable, data (-> class_ro_t)            20 bytes
     class_ro_t   flags, instanceStart, instanceSize, ivarLayout, name, baseMethods,
                  baseProtocols, ivars, weakIvarLayout, baseProperties
     method_list  entsize|flags, count, then method_t {name (SEL), types, imp}
     ivar_list    entsize, count, then ivar_t {offset *, name, type, alignment, size}
     category_t   name, cls, instanceMethods, classMethods, protocols, instanceProperties
   Host classes (UIKit, Foundation...) get class_t / class_ro_t made the same way.  Retain counts
   live in a side table (as on 32-bit iOS), autorelease pools per thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "abi.h"
#include "hmap.h"
#include "layout.h"
#include "loader.h"
#include "objc.h"

struct ObjcClass {
    u32 addr;                  /* class_t */
    const char *name;
    ObjcClass *super;
    ObjcClass *meta;           /* class: its metaclass */
    ObjcClass *nonmeta;        /* metaclass: its class */
    int is_meta, host, init_state;
    u32 size;                  /* instance size */
    u32 ro;
    Map32 methods;             /* SEL -> method_t address */
    Map32 cache;               /* SEL -> IMP (inherited too; 1 = known missing) */
};

static CRITICAL_SECTION lock;
static Map32 by_addr;          /* class_t address -> ObjcClass * */
static MapStr by_name;         /* class name -> ObjcClass * (classes only) */
static MapStr sels;            /* name -> SEL */
static ObjcClass **all;
static unsigned nall, capall;
static int strict;

static u32 empty_cache[4], empty_vtable[16];

#define MISS 1u

/* selectors */

static SEL sel_intern(const char *name, u32 prefer)
{
    u32 v;
    if (ms_get(&sels, name, &v))
        return v;
    if (!prefer) {
        char *s = strdup(name);
        prefer = GA(s);
    }
    ms_put(&sels, (const char *)GP(prefer), prefer);
    return prefer;
}

SEL sel_get(const char *name)
{
    EnterCriticalSection(&lock);
    SEL s = sel_intern(name, 0);
    LeaveCriticalSection(&lock);
    return s;
}

const char *sel_name(SEL s) { return s ? (const char *)GP(s) : "(null)"; }

/* classes */

static ObjcClass *new_class(u32 addr, const char *name, int is_meta)
{
    ObjcClass *k = calloc(1, sizeof *k);
    k->addr = addr;
    k->name = name;
    k->is_meta = is_meta;
    k->size = 4;
    m32_put(&by_addr, addr, GA(k));
    if (nall == capall) {
        capall = capall ? capall * 2 : 1024;
        all = realloc(all, capall * sizeof *all);
    }
    all[nall++] = k;
    if (!is_meta)
        ms_put(&by_name, name, GA(k));
    return k;
}

static ObjcClass *objc_class_at(u32 cls)
{
    u32 v;
    if (!cls || !m32_get(&by_addr, cls, &v))
        return NULL;
    return (ObjcClass *)GP(v);
}

static ObjcClass *objc_class_named(const char *name)
{
    u32 v;
    EnterCriticalSection(&lock);
    int ok = ms_get(&by_name, name, &v);
    LeaveCriticalSection(&lock);
    return ok ? (ObjcClass *)GP(v) : NULL;
}

u32 objc_class_addr(ObjcClass *k) { return k ? k->addr : 0; }

u32 objc_getClass_addr(const char *name)
{
    ObjcClass *k = objc_class_named(name);
    return k ? k->addr : 0;
}

const char *objc_class_name(u32 cls)
{
    ObjcClass *k = objc_class_at(cls);
    return k ? k->name : "?";
}

static u32 objc_super_addr(u32 cls)
{
    ObjcClass *k = objc_class_at(cls);
    return k && k->super ? k->super->addr : 0;
}

static int objc_is_meta(u32 cls)
{
    ObjcClass *k = objc_class_at(cls);
    return k && k->is_meta;
}

int objc_is_kind(id obj, u32 cls)
{
    if (!obj)
        return 0;
    for (ObjcClass *k = objc_class_at(rd32(obj)); k; k = k->super)
        if (k->addr == cls)
            return 1;
    return 0;
}

u32 objc_instance_size(u32 cls)
{
    ObjcClass *k = objc_class_at(cls);
    return k ? k->size : 0;
}

/* Superclasses of the classes the host provides (unlisted: NSObject). */
static const char *const host_supers[][2] = {
    {"UIResponder", "NSObject"}, {"UIView", "UIResponder"}, {"UIViewController", "UIResponder"},
    {"UIApplication", "UIResponder"}, {"UIWindow", "UIView"}, {"UIControl", "UIView"},
    {"UIButton", "UIControl"}, {"UILabel", "UIView"}, {"UIImageView", "UIView"},
    {"UIScrollView", "UIView"}, {"UITableView", "UIScrollView"}, {"UITextView", "UIScrollView"},
    {"UITextField", "UIControl"}, {"UIWebView", "UIView"}, {"UIAlertView", "UIView"},
    {"UIActionSheet", "UIView"}, {"UIActivityIndicatorView", "UIView"}, {"UINavigationBar", "UIView"},
    {"UIToolbar", "UIView"}, {"UISearchBar", "UIView"}, {"UISegmentedControl", "UIControl"},
    {"UIProgressView", "UIView"}, {"UITableViewCell", "UIView"}, {"UISlider", "UIControl"},
    {"UISwitch", "UIControl"}, {"UINavigationController", "UIViewController"},
    {"UITableViewController", "UIViewController"}, {"UITabBarController", "UIViewController"},
    {"UIImagePickerController", "UINavigationController"},
    {"MPMoviePlayerViewController", "UIViewController"},
    {"MFMailComposeViewController", "UINavigationController"},
    {"MFMessageComposeViewController", "UINavigationController"}, {"ADBannerView", "UIView"},
    {"UIBarButtonItem", "UIBarItem"}, {"UITapGestureRecognizer", "UIGestureRecognizer"},
    {"CAEAGLLayer", "CALayer"}, {"CAGradientLayer", "CALayer"}, {"CATransition", "CAAnimation"},
    {"NSMutableArray", "NSArray"}, {"NSMutableDictionary", "NSDictionary"},
    {"NSMutableString", "NSString"}, {"NSMutableData", "NSData"}, {"NSMutableSet", "NSSet"},
    {"NSMutableURLRequest", "NSURLRequest"}, {"NSMutableCharacterSet", "NSCharacterSet"},
    {"NSHTTPURLResponse", "NSURLResponse"}, {"NSInputStream", "NSStream"},
    {"NSOutputStream", "NSStream"}, {"NSDecimalNumber", "NSNumber"}, {"NSNumber", "NSValue"},
    {"NSInvocationOperation", "NSOperation"}, {"NSKeyedArchiver", "NSCoder"},
    {"NSKeyedUnarchiver", "NSCoder"}, {"NSMachPort", "NSPort"},
    {"__NSCFConstantString", "NSString"}, {"__NSCFString", "NSMutableString"},
    {"__NSArrayM", "NSMutableArray"}, {"__NSDictionaryM", "NSMutableDictionary"}, {"__NSSetM", "NSMutableSet"},
    {"__NSCFNumber", "NSNumber"}, {"__NSCFBoolean", "NSNumber"}, {"__NSCFData", "NSMutableData"},
    {"NSConcreteValue", "NSValue"}, {"__NSFastEnumerationEnumerator", "NSEnumerator"},
    {"NSRecursiveLock", "NSObject"}, {"NSDateFormatter", "NSFormatter"}, {"NSNumberFormatter", "NSFormatter"},
    {"NSMutableAttributedString", "NSAttributedString"},
    {"__NSGlobalBlock__", "NSBlock"}, {"__NSStackBlock__", "NSBlock"}, {"__NSMallocBlock__", "NSBlock"},
    {NULL, NULL}};

static ObjcClass *host_class_locked(const char *name)
{
    u32 v;
    if (ms_get(&by_name, name, &v))
        return (ObjcClass *)GP(v);
    const char *sup = strcmp(name, "NSObject") ? "NSObject" : NULL;
    for (int i = 0; host_supers[i][0]; i++)
        if (!strcmp(host_supers[i][0], name))
            sup = host_supers[i][1];
    ObjcClass *s = sup ? host_class_locked(sup) : NULL;
    u32 *cls = calloc(5, 4), *meta = calloc(5, 4), *ro = calloc(10, 4), *mro = calloc(10, 4);
    char *nm = strdup(name);
    ObjcClass *k = new_class(GA(cls), nm, 0);
    ObjcClass *m = new_class(GA(meta), nm, 1);
    k->meta = m;
    m->nonmeta = k;
    k->host = m->host = 1;
    k->ro = GA(ro);
    m->ro = GA(mro);
    k->super = s;
    k->size = s ? s->size : 4;
    k->init_state = m->init_state = 0;
    ObjcClass *root = s ? s : k;
    while (root->super)
        root = root->super;
    m->super = s ? s->meta : k;           /* the root metaclass inherits from the root class */
    cls[0] = GA(meta);
    cls[1] = s ? s->addr : 0;
    cls[2] = GA(empty_cache);
    cls[3] = GA(empty_vtable);
    cls[4] = GA(ro);
    meta[0] = root->meta ? root->meta->addr : GA(meta);
    meta[1] = m->super->addr;
    meta[2] = GA(empty_cache);
    meta[3] = GA(empty_vtable);
    meta[4] = GA(mro);
    ro[0] = s ? 0 : 2;                     /* RO_ROOT */
    ro[1] = ro[2] = k->size;
    ro[4] = GA(nm);
    mro[0] = 1 | (s ? 0 : 2);              /* RO_META */
    mro[1] = mro[2] = 20;
    mro[4] = GA(nm);
    return k;
}

ObjcClass *objc_host_class(const char *name)
{
    EnterCriticalSection(&lock);
    ObjcClass *k = host_class_locked(name);
    LeaveCriticalSection(&lock);
    return k;
}

static void flush_caches(void)
{
    for (unsigned i = 0; i < nall; i++)
        m32_clear(&all[i]->cache);
}

/* method_t records for host methods (the game may ask for Method pointers). */
static u32 make_method(SEL sel, u32 imp, const char *types)
{
    u32 *m = calloc(3, 4);
    m[0] = sel;
    m[1] = GA(types ? types : "");
    m[2] = imp;
    return GA(m);
}

void objc_add_method(const char *cls, const char *sel, ObjcHostIMP fn, int class_method)
{
    ObjcClass *k = objc_host_class(cls);
    if (class_method)
        k = k->meta;
    char what[160];
    snprintf(what, sizeof what, "%c[%s %s]", class_method ? '+' : '-', cls, sel);
    u32 imp = rt_host_addr(fn, strdup(what));
    EnterCriticalSection(&lock);
    SEL s = sel_intern(sel, 0);
    m32_put(&k->methods, s, make_method(s, imp, NULL));
    flush_caches();
    LeaveCriticalSection(&lock);
}

/* the game's image */

static void add_method_list(ObjcClass *k, u32 list)
{
    if (!list)
        return;
    u32 entsize = rd32(list) & ~3u, count = rd32(list + 4);
    if (entsize < 12)
        entsize = 12;
    for (u32 i = 0; i < count; i++) {
        u32 m = list + 8 + i * entsize;
        SEL s = sel_intern((const char *)GP(rd32(m)), rd32(m));
        wr32(m, s);                        /* method_getName sees the canonical selector */
        m32_put(&k->methods, s, m);
    }
}

static ObjcClass *realize(u32 addr);
static u32 n_slid;

static ObjcClass *realize_meta(u32 maddr, ObjcClass *cls)
{
    ObjcClass *m = objc_class_at(maddr);
    if (m)
        return m;
    u32 ro = rd32(maddr + 16) & ~3u;
    m = new_class(maddr, cls->name, 1);
    m->nonmeta = cls;
    m->ro = ro;
    u32 sup = rd32(maddr + 4);
    m->super = objc_class_at(sup);
    if (!m->super && sup)
        rt_log("[objc] metaclass of %s: superclass 0x%08x unknown\n", cls->name, sup);
    add_method_list(m, rd32(ro + 20));
    return m;
}

static ObjcClass *realize(u32 addr)
{
    ObjcClass *k = objc_class_at(addr);
    if (k)
        return k;
    u32 ro = rd32(addr + 16) & ~3u;
    const char *name = (const char *)GP(rd32(ro + 16));
    u32 sup = rd32(addr + 4);
    ObjcClass *s = NULL;
    if (sup) {
        s = objc_class_at(sup);
        if (!s)
            s = realize(sup);              /* a game class later in the list */
    }
    k = new_class(addr, name, 0);
    k->ro = ro;
    k->super = s;
    /* non-fragile ivars: slide if the superclass is bigger than the compiler thought */
    u32 start = rd32(ro + 4), size = rd32(ro + 8);
    if (s && s->size > start) {
        /* objc4's moveIvars: the slide keeps the largest ivar alignment of the class */
        u32 delta = s->size - start, iv = rd32(ro + 28), align = 1;
        u32 es = iv ? rd32(iv) : 0, n = iv ? rd32(iv + 4) : 0;
        for (u32 i = 0; i < n; i++) {
            u32 a = 1u << rd32(iv + 8 + i * es + 12);
            if (a > align && a <= 16)
                align = a;
        }
        delta = (delta + align - 1) & ~(align - 1);
        for (u32 i = 0; i < n; i++) {
            u32 off = rd32(iv + 8 + i * es);
            if (off)
                wr32(off, rd32(off) + delta);
        }
        if (rt_trace)
            rt_log("[objc] %s: ivars slid by %u (superclass %s is %u bytes, compiled for %u)\n", name, delta,
                   s->name, s->size, start);
        n_slid++;
        size += delta;
    }
    k->size = size;
    add_method_list(k, rd32(ro + 20));
    k->meta = realize_meta(rd32(addr), k);
    return k;
}

static u32 n_classes, n_cats, n_sels;

/* The image's classes, categories and selector references (after binding). */
void gt_objc_image(void)
{
    const macho *M = gt_macho();
    EnterCriticalSection(&lock);
    const macho_sect *s = macho_sect_named(M, "__DATA", "__objc_selrefs");
    for (u32 a = s ? s->addr : 0; s && a < s->addr + s->size; a += 4) {
        u32 p = rd32(a);
        wr32(a, sel_intern((const char *)GP(p), p));
        n_sels++;
    }
    /* host class sizes: what the game's subclasses were compiled against (the smallest first-ivar
       offset: a subclass whose first ivar needs 4-byte alignment starts after padding) */
    s = macho_sect_named(M, "__DATA", "__objc_classlist");
    Map32 minstart = {0};
    for (u32 a = s ? s->addr : 0; s && a < s->addr + s->size; a += 4) {
        u32 cls = rd32(a), sup = rd32(cls + 4), v;
        ObjcClass *h = objc_class_at(sup);
        if (h && h->host) {
            u32 start = rd32((rd32(cls + 16) & ~3u) + 4);
            if (!m32_get(&minstart, sup, &v) || start < v)
                m32_put(&minstart, sup, start);
        }
    }
    for (unsigned i = 0; i < nall; i++) {
        u32 v;
        if (m32_get(&minstart, all[i]->addr, &v) && v > all[i]->size)
            all[i]->size = v;
    }
    for (unsigned i = 0; i < nall; i++)    /* a host class is at least as big as its superclass */
        for (ObjcClass *k = all[i]; k->host && !k->is_meta && k->super; k = k->super)
            if (k->super->size > k->size)
                k->size = k->super->size;
    for (u32 a = s ? s->addr : 0; s && a < s->addr + s->size; a += 4) {
        realize(rd32(a));
        n_classes++;
    }
    s = macho_sect_named(M, "__DATA", "__objc_catlist");
    for (u32 a = s ? s->addr : 0; s && a < s->addr + s->size; a += 4) {
        u32 cat = rd32(a);
        ObjcClass *k = objc_class_at(rd32(cat + 4));
        if (!k) {
            rt_log("[objc] category %s on unknown class 0x%08x\n", (const char *)GP(rd32(cat)), rd32(cat + 4));
            continue;
        }
        add_method_list(k, rd32(cat + 8));
        add_method_list(k->meta, rd32(cat + 12));
        n_cats++;
    }
    flush_caches();
    LeaveCriticalSection(&lock);
    rt_log("[objc] %u classes (%u with ivars slid past their superclass), %u categories, %u selector references, "
           "%u selectors\n", n_classes, n_slid, n_cats, n_sels, sels.n);
}

/* +load of every class (superclasses first) and category, as the runtime does at image load. */
void gt_objc_load(void)
{
    const macho *M = gt_macho();
    SEL load = sel_get("load");
    const macho_sect *s = macho_sect_named(M, "__DATA", "__objc_classlist");
    for (u32 a = s ? s->addr : 0; s && a < s->addr + s->size; a += 4) {
        ObjcClass *k = objc_class_at(rd32(a));
        u32 m;
        if (k && m32_get(&k->meta->methods, load, &m)) {
            rt_log("[objc] +[%s load]\n", k->name);
            u32 args[2] = {k->addr, load};
            rt_guest_call(rt_cpu(), rd32(m + 8), 2, args);
        }
    }
    s = macho_sect_named(M, "__DATA", "__objc_catlist");
    for (u32 a = s ? s->addr : 0; s && a < s->addr + s->size; a += 4) {
        u32 cat = rd32(a), list = rd32(cat + 12);
        if (!list)
            continue;
        u32 es = rd32(list) & ~3u, n = rd32(list + 4);
        for (u32 i = 0; i < n; i++) {
            u32 m = list + 8 + i * es;
            if (rd32(m) == load) {
                rt_log("[objc] +[%s(%s) load]\n", objc_class_name(rd32(cat + 4)), (const char *)GP(rd32(cat)));
                u32 args[2] = {rd32(cat + 4), load};
                rt_guest_call(rt_cpu(), rd32(m + 8), 2, args);
            }
        }
    }
}

/* lookup and messaging */

static void initialize(ObjcClass *k);

static u32 lookup_locked(ObjcClass *k, SEL sel)
{
    u32 imp;
    if (!sel)
        return 0;
    if (m32_get(&k->cache, sel, &imp))
        return imp == MISS ? 0 : imp;
    imp = 0;
    for (ObjcClass *p = k; p; p = p->super) {
        u32 m;
        if (m32_get(&p->methods, sel, &m)) {
            imp = rd32(m + 8);
            break;
        }
    }
    m32_put(&k->cache, sel, imp ? imp : MISS);
    return imp;
}

u32 objc_lookup(u32 cls, SEL sel)
{
    ObjcClass *k = objc_class_at(cls);
    if (!k)
        return 0;
    EnterCriticalSection(&lock);
    u32 imp = lookup_locked(k, sel);
    LeaveCriticalSection(&lock);
    return imp;
}

static void initialize(ObjcClass *k)
{
    ObjcClass *c = k->is_meta ? k->nonmeta : k;
    if (!c || c->init_state)
        return;
    c->init_state = 1;
    if (c->super)
        initialize(c->super);
    SEL s = sel_get("initialize");
    u32 imp = objc_lookup(c->meta->addr, s);
    if (imp) {
        u32 args[2] = {c->addr, s};
        rt_guest_call(rt_cpu(), imp, 2, args);
    }
    c->init_state = 2;
}

/* The IMP for a message to receiver class cls, +initialize first. */
static u32 send_lookup(u32 cls, SEL sel)
{
    ObjcClass *k = objc_class_at(cls);
    if (!k)
        return 0;
    ObjcClass *c = k->is_meta ? k->nonmeta : k;
    if (c && !c->init_state)
        initialize(k);
    EnterCriticalSection(&lock);
    u32 imp = lookup_locked(k, sel);
    LeaveCriticalSection(&lock);
    return imp;
}

static void not_found(CPU *c, id self, u32 cls, SEL sel)
{
    static Map32 seen;
    ObjcClass *k = objc_class_at(cls);
    u32 key = hm_hash32(cls) ^ sel, v;
    EnterCriticalSection(&lock);
    int first = !m32_get(&seen, key, &v);
    if (first)
        m32_put(&seen, key, 1);
    LeaveCriticalSection(&lock);
    if (!k)
        rt_fatal("message %s to 0x%08x, whose isa 0x%08x is not a class (return address 0x%08x)", sel_name(sel),
                 self, cls, c->r[14]);
    if (first)
        rt_log("[objc] %c[%s %s] not implemented (return address 0x%08x)\n", k->is_meta ? '+' : '-', k->name,
               sel_name(sel), c->r[14]);
    if (strict)
        rt_fatal("missing method (GT_OBJC_STRICT)");
    c->r[0] = 0;
    c->r[1] = 0;
}

void I_objc_msgSend(CPU *c)
{
    id self = c->r[0];
    if (!self) {
        c->r[1] = 0;
        return;
    }
    u32 cls = rd32(self), imp = send_lookup(cls, c->r[1]);
    if (rt_trace)
        rt_log("[msg] %c[%s %s]\n", objc_is_meta(cls) ? '+' : '-', objc_class_name(cls), sel_name(c->r[1]));
    if (!imp) {
        not_found(c, self, cls, c->r[1]);
        return;
    }
    rt_call_indirect(c, imp);
}

void I_objc_msgSend_stret(CPU *c)
{
    id self = c->r[1];
    if (!self)
        return;
    u32 cls = rd32(self), imp = send_lookup(cls, c->r[2]);
    if (!imp) {
        u32 r0 = c->r[0];
        not_found(c, self, cls, c->r[2]);
        c->r[0] = r0;
        return;
    }
    rt_call_indirect(c, imp);
}

/* struct objc_super { id receiver; Class current_class; }: lookup starts at its superclass. */
void I_objc_msgSendSuper2(CPU *c)
{
    u32 sup = c->r[0], self = rd32(sup), cls = objc_super_addr(rd32(sup + 4));
    u32 imp = cls ? send_lookup(cls, c->r[1]) : 0;
    c->r[0] = self;
    if (!imp) {
        not_found(c, self, cls ? cls : rd32(sup + 4), c->r[1]);
        return;
    }
    rt_call_indirect(c, imp);
}

void I_objc_msgSendSuper2_stret(CPU *c)
{
    u32 sup = c->r[1], self = rd32(sup), cls = objc_super_addr(rd32(sup + 4));
    u32 imp = cls ? send_lookup(cls, c->r[2]) : 0;
    c->r[1] = self;
    if (!imp) {
        u32 r0 = c->r[0];
        not_found(c, self, cls ? cls : rd32(sup + 4), c->r[2]);
        c->r[0] = r0;
        return;
    }
    rt_call_indirect(c, imp);
}

u32 objc_msg(id self, const char *sel, int n, const u32 *args)
{
    if (!self)
        return 0;
    CPU *c = rt_cpu();
    SEL s = sel_get(sel);
    u32 cls = rd32(self), imp = send_lookup(cls, s);
    if (!imp) {
        u32 save[2] = {c->r[0], c->r[1]};
        not_found(c, self, cls, s);
        c->r[0] = save[0];
        c->r[1] = save[1];
        return 0;
    }
    u32 w[8] = {self, s};
    for (int i = 0; i < n && i < 6; i++)
        w[2 + i] = args[i];
    return rt_guest_call(c, imp, 2 + n, w);
}

u32 msg0(id self, const char *sel) { return objc_msg(self, sel, 0, NULL); }
u32 msg1(id self, const char *sel, u32 a) { return objc_msg(self, sel, 1, &a); }
u32 msg2(id self, const char *sel, u32 a, u32 b)
{
    u32 v[2] = {a, b};
    return objc_msg(self, sel, 2, v);
}

int objc_responds(id self, const char *sel)
{
    return self && objc_lookup(rd32(self), sel_get(sel)) != 0;
}

/* objects, retain counts, autorelease pools */

static Map32 extra_rc;         /* object -> retain count - 1 */
static CRITICAL_SECTION rc_lock;

id objc_alloc_instance(u32 cls, u32 extra)
{
    u32 size = objc_instance_size(cls) + extra;
    if (size < 4)
        size = 4;
    u8 *p = _aligned_malloc((size + 15) & ~15u, 16);
    if (!p)
        rt_fatal("out of memory (object of %u bytes)", size);
    memset(p, 0, (size + 15) & ~15u);
    *(u32 *)p = cls;
    return GA(p);
}

void objc_free_instance(id obj)
{
    if (!obj)
        return;
    EnterCriticalSection(&rc_lock);
    m32_del(&extra_rc, obj);
    LeaveCriticalSection(&rc_lock);
    _aligned_free(GP(obj));
}

id objc_retain(id obj)
{
    if (!obj)
        return 0;
    EnterCriticalSection(&rc_lock);
    u32 v = 0;
    m32_get(&extra_rc, obj, &v);
    m32_put(&extra_rc, obj, v + 1);
    LeaveCriticalSection(&rc_lock);
    return obj;
}

static u32 objc_retain_count(id obj)
{
    u32 v = 0;
    EnterCriticalSection(&rc_lock);
    m32_get(&extra_rc, obj, &v);
    LeaveCriticalSection(&rc_lock);
    return v + 1;
}

void objc_release(id obj)
{
    if (!obj)
        return;
    EnterCriticalSection(&rc_lock);
    u32 v = 0;
    int dealloc = !m32_get(&extra_rc, obj, &v) || v == 0;
    if (!dealloc)
        m32_put(&extra_rc, obj, v - 1);
    LeaveCriticalSection(&rc_lock);
    if (dealloc)
        msg0(obj, "dealloc");
}

typedef struct { u32 *objs; u32 n, cap; } Pool;

static Pool *pool(void)
{
    RtThread *t = rt_thread();
    Pool *p = t->objc_pool;
    if (!p) {
        p = calloc(1, sizeof *p);
        t->objc_pool = p;
    }
    return p;
}

id objc_autorelease(id obj)
{
    if (!obj)
        return 0;
    Pool *p = pool();
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 256;
        p->objs = realloc(p->objs, p->cap * 4);
    }
    p->objs[p->n++] = obj;
    return obj;
}

u32 objc_pool_push(void) { return pool()->n; }

void objc_pool_pop(u32 mark)
{
    Pool *p = pool();
    while (p->n > mark)
        objc_release(p->objs[--p->n]);
}

/* NSObject (the root class), NSAutoreleasePool */

#define SELF c->r[0]
#define ARG(i) arg_word(c, 2 + (i))

/* +alloc goes through +allocWithZone: as in Apple's runtime: singletons override that one to keep
   their instance (GLiveViewController, the ad SDKs' managers) */
static u32 alloc_imp;
static void m_allocWithZone(CPU *c) { ret_i(c, objc_alloc_instance(SELF, 0)); }
static void m_alloc(CPU *c)
{
    u32 imp = objc_lookup(rd32(SELF), sel_get("allocWithZone:"));
    if (imp && imp != alloc_imp) {
        u32 zone = 0;
        ret_i(c, objc_msg(SELF, "allocWithZone:", 1, &zone));
        return;
    }
    ret_i(c, objc_alloc_instance(SELF, 0));
}
static void m_new(CPU *c) { ret_i(c, msg0(msg0(SELF, "alloc"), "init")); }
static void m_self(CPU *c) { (void)c; }
static void m_class_c(CPU *c) { (void)c; }
static void m_class_i(CPU *c) { ret_i(c, rd32(SELF)); }
static void m_superclass_c(CPU *c) { ret_i(c, objc_super_addr(SELF)); }
static void m_superclass_i(CPU *c) { ret_i(c, objc_super_addr(rd32(SELF))); }
static void m_retain(CPU *c) { objc_retain(SELF); }
static void m_release(CPU *c) { objc_release(SELF); }
static void m_autorelease(CPU *c) { objc_autorelease(SELF); }
static void m_retainCount(CPU *c) { ret_i(c, objc_retain_count(SELF)); }
static void m_dealloc(CPU *c) { objc_free_instance(SELF); }
static void m_nop_c(CPU *c) { (void)c; }
static void m_zero(CPU *c) { ret_i(c, 0); }
static void m_hash(CPU *c) { (void)c; }
static void m_isEqual(CPU *c) { ret_i(c, SELF == ARG(0)); }
static void m_isKindOfClass(CPU *c) { ret_i(c, objc_is_kind(SELF, ARG(0))); }
static void m_isMemberOfClass(CPU *c) { ret_i(c, rd32(SELF) == ARG(0)); }
static void m_respondsToSelector(CPU *c) { ret_i(c, objc_lookup(rd32(SELF), ARG(0)) != 0); }
static void m_instancesRespond(CPU *c) { ret_i(c, objc_lookup(SELF, ARG(0)) != 0); }
static void m_isSubclassOfClass(CPU *c)
{
    u32 r = 0;
    for (ObjcClass *k = objc_class_at(SELF); k; k = k->super)
        if (k->addr == ARG(0))
            r = 1;
    ret_i(c, r);
}
static void m_conformsToProtocol(CPU *c) { ret_i(c, 0); }
static void m_performSelector(CPU *c)
{
    /* -performSelector:(SEL) [withObject: [withObject:]]: the same message with the arguments moved down */
    id self = SELF;
    SEL s = c->r[2];
    u32 imp = send_lookup(rd32(self), s);
    if (!imp) {
        not_found(c, self, rd32(self), s);
        return;
    }
    u32 w[4] = {self, s, c->r[3], arg_word(c, 4)};
    ret_i(c, rt_guest_call(c, imp, 4, w));
}
static void m_copy(CPU *c) { ret_i(c, msg1(SELF, "copyWithZone:", 0)); }
static void m_mutableCopy(CPU *c) { ret_i(c, msg1(SELF, "mutableCopyWithZone:", 0)); }

static void m_pool_init(CPU *c) { wr32(SELF + 4, objc_pool_push() + 1); }
static void m_pool_drain(CPU *c)
{
    id self = SELF;
    objc_pool_pop(rd32(self + 4) - 1);
    objc_free_instance(self);
}
static void m_pool_add_c(CPU *c) { objc_autorelease(ARG(0)); }

/* runtime functions the game imports */

static Map32 sync_locks;       /* object -> CRITICAL_SECTION * (recursive, as @synchronized) */

u32 h_objc_sync_enter(u32 obj)
{
    if (!obj)
        return 0;
    u32 v;
    EnterCriticalSection(&rc_lock);
    if (!m32_get(&sync_locks, obj, &v)) {
        CRITICAL_SECTION *cs = malloc(sizeof *cs);
        InitializeCriticalSection(cs);
        v = GA(cs);
        m32_put(&sync_locks, obj, v);
    }
    LeaveCriticalSection(&rc_lock);
    EnterCriticalSection(GP(v));
    return 0;
}
u32 h_objc_sync_exit(u32 obj)
{
    u32 v;
    if (!obj)
        return 0;
    EnterCriticalSection(&rc_lock);
    int ok = m32_get(&sync_locks, obj, &v);
    LeaveCriticalSection(&rc_lock);
    if (ok)
        LeaveCriticalSection(GP(v));
    return ok ? 0 : (u32)-1;
}

u32 h_class_getName(u32 cls) { return cls ? GA(objc_class_name(cls)) : GA("nil"); }
u32 h_class_getSuperclass(u32 cls) { return objc_super_addr(cls); }
u32 h_object_getClass(u32 obj) { return obj ? rd32(obj) : 0; }
u32 h_sel_getName(u32 sel) { return sel ? sel : GA("<null selector>"); }
u32 h_sel_registerName(u32 name) { return sel_get(GP(name)); }
u32 h_objc_lookUpClass(u32 name) { return objc_getClass_addr(GP(name)); }

/* method_t of sel in cls or its superclasses */
static u32 method_record(u32 cls, SEL sel)
{
    ObjcClass *k = objc_class_at(cls);
    u32 m = 0;
    EnterCriticalSection(&lock);
    for (; k && !m; k = k->super)
        if (!m32_get(&k->methods, sel, &m))
            m = 0;
    LeaveCriticalSection(&lock);
    return m;
}
u32 h_class_getInstanceMethod(u32 cls, u32 sel) { return method_record(cls, sel); }
u32 h_class_getClassMethod(u32 cls, u32 sel) { return cls ? method_record(rd32(cls), sel) : 0; }
u32 h_method_getImplementation(u32 m) { return m ? rd32(m + 8) : 0; }
u32 h_method_getName(u32 m) { return m ? rd32(m) : 0; }
u32 h_method_getTypeEncoding(u32 m) { return m ? rd32(m + 4) : 0; }
u32 h_method_setImplementation(u32 m, u32 imp)
{
    if (!m)
        return 0;
    EnterCriticalSection(&lock);
    u32 old = rd32(m + 8);
    wr32(m + 8, imp);
    flush_caches();
    LeaveCriticalSection(&lock);
    return old;
}
u32 h_class_addMethod(u32 cls, u32 sel, u32 imp, u32 types)
{
    ObjcClass *k = objc_class_at(cls);
    u32 m;
    if (!k)
        return 0;
    EnterCriticalSection(&lock);
    int has = m32_get(&k->methods, sel, &m);
    if (!has) {
        m32_put(&k->methods, sel, make_method(sel, imp, types ? GP(types) : NULL));
        flush_caches();
    }
    LeaveCriticalSection(&lock);
    return !has;
}
u32 h_class_copyMethodList(u32 cls, u32 out_count)
{
    ObjcClass *k = objc_class_at(cls);
    u32 n = 0, *list = NULL;
    if (k) {
        EnterCriticalSection(&lock);
        list = malloc(4 * ((size_t)k->methods.n + 1));
        for (u32 i = 0; k->methods.k && i <= k->methods.mask; i++)
            if (k->methods.k[i])
                list[n++] = k->methods.v[i];
        list[n] = 0;
        LeaveCriticalSection(&lock);
    }
    if (out_count)
        wr32(out_count, n);
    if (!n) {
        free(list);
        return 0;
    }
    return GA(list);
}
/* property_list_t {entsize, count, property_t {name, attributes}} in class_ro_t +36 */
static u32 prop_list(u32 cls)
{
    ObjcClass *k = objc_class_at(cls);
    return k && k->ro ? rd32(k->ro + 36) : 0;
}
u32 h_class_copyPropertyList(u32 cls, u32 out_count)
{
    u32 l = prop_list(cls), n = l ? rd32(l + 4) : 0, es = l ? rd32(l) : 8;
    if (out_count)
        wr32(out_count, n);
    if (!n)
        return 0;
    u32 *list = malloc(4 * ((size_t)n + 1));
    for (u32 i = 0; i < n; i++)
        list[i] = l + 8 + i * es;
    list[n] = 0;
    return GA(list);
}
u32 h_class_getProperty(u32 cls, u32 name)
{
    for (u32 k = cls; k; k = objc_super_addr(k)) {
        u32 l = prop_list(k), n = l ? rd32(l + 4) : 0, es = l ? rd32(l) : 8;
        for (u32 i = 0; i < n; i++)
            if (!strcmp(GP(rd32(l + 8 + i * es)), GP(name)))
                return l + 8 + i * es;
    }
    return 0;
}
/* objc_property_t: {const char *name, const char *attributes} in the class's property list */
u32 h_property_getName(u32 p) { return p ? rd32(p) : 0; }
u32 h_property_getAttributes(u32 p) { return p ? rd32(p + 4) : 0; }

u32 h_objc_getProperty(u32 self, u32 cmd, u32 offset, u32 atomic)
{
    (void)cmd;
    u32 v = rd32(self + offset);
    if ((u8)atomic && v) {
        objc_retain(v);
        objc_autorelease(v);
    }
    return v;
}
void h_objc_setProperty(u32 self, u32 cmd, u32 offset, u32 v, u32 atomic, u32 copy)
{
    (void)cmd;
    (void)atomic;
    u32 nv = v ? ((s8)copy ? msg1(v, (s8)copy == 2 ? "mutableCopyWithZone:" : "copyWithZone:", 0) : objc_retain(v)) : 0;
    u32 old = rd32(self + offset);
    wr32(self + offset, nv);
    objc_release(old);
}
void h_objc_copyStruct(u32 dst, u32 src, u32 size, u32 atomic, u32 strong)
{
    (void)atomic;
    (void)strong;
    memmove(GP(dst), GP(src), size);
}
void h_objc_enumerationMutation(u32 obj)
{
    rt_fatal("a collection (%s 0x%08x) was mutated while being enumerated", objc_class_name(rd32(obj)), obj);
}
u32 h_objc_getClassList(u32 buf, u32 count)
{
    u32 n = 0;
    EnterCriticalSection(&lock);
    for (unsigned i = 0; i < nall; i++)
        if (!all[i]->is_meta) {
            if (buf && n < count)
                wr32(buf + 4 * n, all[i]->addr);
            n++;
        }
    LeaveCriticalSection(&lock);
    return n;
}

/* Host state lives inside host objects (UIView, CALayer...): the class gets at least `bytes`;
   the game's subclasses' ivars slide past it when the image is realized.  Call before gt_load. */
void objc_host_size(const char *name, u32 bytes)
{
    ObjcClass *k = objc_host_class(name);
    if (k->size < bytes) {
        k->size = bytes;
        wr32(k->ro + 4, bytes);
        wr32(k->ro + 8, bytes);
    }
}

/* --objc-dump: every class with game methods (game classes and game categories on host ones). */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
void gt_objc_dump(FILE *f)
{
    for (unsigned i = 0; i < nall; i++) {
        ObjcClass *k = all[i];
        if (k->is_meta)
            continue;
        const char *lines[4096];
        unsigned n = 0;
        for (int meta = 0; meta < 2; meta++) {
            Map32 *m = meta ? &k->meta->methods : &k->methods;
            for (u32 j = 0; m->k && j <= m->mask; j++) {
                if (!m->k[j])
                    continue;
                u32 imp = rd32(m->v[j] + 8);
                if (imp >= GT_HOSTFN_BASE && imp < GT_HOSTFN_BASE + GT_HOSTFN_SIZE)
                    continue;
                if (n < 4096) {
                    char *s = malloc(strlen(sel_name(m->k[j])) + 32);
                    sprintf(s, "%c%s  0x%x", meta ? '+' : '-', sel_name(m->k[j]), imp);
                    lines[n++] = s;
                }
            }
        }
        if (!n && k->host)
            continue;
        fprintf(f, "%s%s : %s  (%u bytes)\n", k->host ? "[host] " : "", k->name, k->super ? k->super->name : "-",
                k->size);
        qsort(lines, n, sizeof *lines, cmp_str);
        for (unsigned j = 0; j < n; j++) {
            fprintf(f, "    %s\n", lines[j]);
            free((void *)lines[j]);
        }
    }
}

void gt_objc_init(void)
{
    InitializeCriticalSection(&lock);
    InitializeCriticalSection(&rc_lock);
    strict = dev_getenv("GT_OBJC_STRICT") != NULL;
    objc_host_class("NSObject");
    static const struct { const char *sel; ObjcHostIMP fn; int cls; } root[] = {
        {"alloc", m_alloc, 1}, {"allocWithZone:", m_allocWithZone, 1}, {"new", m_new, 1},
        {"class", m_class_c, 1}, {"superclass", m_superclass_c, 1}, {"initialize", m_nop_c, 1},
        {"instancesRespondToSelector:", m_instancesRespond, 1}, {"isSubclassOfClass:", m_isSubclassOfClass, 1},
        {"self", m_self, 0}, {"init", m_self, 0}, {"class", m_class_i, 0}, {"superclass", m_superclass_i, 0},
        {"retain", m_retain, 0}, {"release", m_release, 0}, {"autorelease", m_autorelease, 0},
        {"retainCount", m_retainCount, 0}, {"dealloc", m_dealloc, 0}, {"zone", m_zero, 0},
        {"hash", m_hash, 0}, {"isEqual:", m_isEqual, 0}, {"isKindOfClass:", m_isKindOfClass, 0},
        {"isMemberOfClass:", m_isMemberOfClass, 0}, {"respondsToSelector:", m_respondsToSelector, 0},
        {"conformsToProtocol:", m_conformsToProtocol, 0}, {"conformsToProtocol:", m_conformsToProtocol, 1},
        {"performSelector:", m_performSelector, 0}, {"performSelector:withObject:", m_performSelector, 0},
        {"performSelector:withObject:withObject:", m_performSelector, 0}, {"copy", m_copy, 0},
        {"mutableCopy", m_mutableCopy, 0}, {NULL, NULL, 0}};
    for (int i = 0; root[i].sel; i++)
        objc_add_method("NSObject", root[i].sel, root[i].fn, root[i].cls);
    alloc_imp = objc_lookup(objc_host_class("NSObject")->meta->addr, sel_get("allocWithZone:"));
    /* class objects answer instance methods of the root class too (metaclass chain), so -retain
       etc. on a class work; +class above returns self. */
    ObjcClass *pool_cls = objc_host_class("NSAutoreleasePool");
    if (pool_cls->size < 8)
        pool_cls->size = 8;
    objc_add_method("NSAutoreleasePool", "init", m_pool_init, 0);
    objc_add_method("NSAutoreleasePool", "drain", m_pool_drain, 0);
    objc_add_method("NSAutoreleasePool", "release", m_pool_drain, 0);
    objc_add_method("NSAutoreleasePool", "addObject:", m_pool_add_c, 1);
}

/* data the Objective-C side provides */

typedef struct { u32 vtable, name, cls; } EhType;
static u32 eh_vtable[4];
u32 objc_ehtype_vtable(void) { return GA(eh_vtable) + 8; }

u32 objc_data_import(const char *name)
{
    if (!strncmp(name, "_OBJC_CLASS_$_", 14))
        return objc_host_class(name + 14)->addr;
    if (!strncmp(name, "_OBJC_METACLASS_$_", 18))
        return objc_host_class(name + 18)->meta->addr;
    if (!strcmp(name, "__objc_empty_cache"))
        return GA(empty_cache);
    if (!strcmp(name, "__objc_empty_vtable"))
        return GA(empty_vtable);
    if (!strcmp(name, "___CFConstantStringClassReference"))
        return objc_host_class("__NSCFConstantString")->addr;
    if (!strcmp(name, "__NSConcreteGlobalBlock"))
        return objc_host_class("__NSGlobalBlock__")->addr;
    if (!strcmp(name, "__NSConcreteStackBlock"))
        return objc_host_class("__NSStackBlock__")->addr;
    if (!strncmp(name, "_OBJC_EHTYPE_", 13)) {
        EhType *t = calloc(1, sizeof *t);
        t->vtable = GA(eh_vtable) + 8;
        if (!strcmp(name, "_OBJC_EHTYPE_id")) {
            t->name = GA("id");
        } else if (!strncmp(name, "_OBJC_EHTYPE_$_", 15)) {
            t->name = GA(strdup(name + 15));
            t->cls = objc_host_class(name + 15)->addr;
        }
        return GA(t);
    }
    return 0;
}

int objc_ehtype_matches(u32 ehtype, id exception)
{
    u32 cls = rd32(ehtype + 8);
    return !cls || objc_is_kind(exception, cls);
}
