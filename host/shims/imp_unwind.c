/* imp_unwind.c -- C++ and Objective-C exceptions: the SjLj unwinder, the personality routine and
   the __cxa_* / objc_*catch* support the game imports.

   The game was built with setjmp/longjmp exceptions (armv7 iOS).  A function with handlers keeps
   a function context on its stack and registers it:
       fc+0x00 prev   fc+0x04 call_site   fc+0x08 data[4] (exception, selector)
       fc+0x18 personality   fc+0x1c lsda   fc+0x20 jbuf: fp (r7), resume address, sp
   Throwing walks the registered contexts (search, then cleanup/handler phase as libgcc does);
   resuming one sets call_site to the landing pad's index and jumps back into the recompiled
   function's host frame (rt_resume_context), whose dispatch code then switches on call_site.

   The exception header is GCC's __cxa_exception (72 bytes on 32-bit ARM, the _Unwind_Exception at
   +48), the thrown object right after it. */
#include <stdlib.h>
#include <string.h>

#include "abi.h"
#include "loader.h"
#include "objc.h"
#include "rt.h"

#define raise eh_raise                 /* not <signal.h>'s raise (bionic's headers bring it in) */

__attribute__((noreturn)) void rt_resume_context(CPU *c, u32 fc);

#define HDR_SIZE 72
#define UE_OFF 48
enum { X_TYPE = 0, X_DTOR = 4, X_NEXT = 16, X_HANDLERS = 20, X_SWITCH = 24, X_ADJUSTED = 40 };
#define OBJ(h) ((h) + HDR_SIZE)
#define HDR_OF_OBJ(o) ((o) - HDR_SIZE)
#define HDR_OF_UE(u) ((u) - UE_OFF)

/* Typeinfo kinds: the vtable addresses the loader binds (__ZTVN10__cxxabiv1...), +8. */
static u32 vt_class[4], vt_si[4], vt_vmi[4];
static u32 objc_marker;        /* exceptionType of an Objective-C exception */

typedef struct { u32 vptr, name; } TypeInfo;
typedef struct { u32 vptr, name, base; } SiTypeInfo;
static TypeInfo ti_exception;
static SiTypeInfo ti_runtime_error;

u32 cxx_data_import(const char *name)
{
    if (!strcmp(name, "__ZTVN10__cxxabiv117__class_type_infoE"))
        return GA(vt_class);
    if (!strcmp(name, "__ZTVN10__cxxabiv120__si_class_type_infoE"))
        return GA(vt_si);
    if (!strcmp(name, "__ZTVN10__cxxabiv121__vmi_class_type_infoE"))
        return GA(vt_vmi);
    if (!strcmp(name, "__ZTISt13runtime_error")) {
        ti_exception.vptr = GA(vt_class) + 8;
        ti_exception.name = GA("St9exception");
        ti_runtime_error.vptr = GA(vt_si) + 8;
        ti_runtime_error.name = GA("St13runtime_error");
        ti_runtime_error.base = GA(&ti_exception);
        return GA(&ti_runtime_error);
    }
    return 0;
}

/* function contexts */

void h_Unwind_SjLj_Register(CPU *c, u32 fc)
{
    RtThread *t = c->rt;
    wr32(fc, t->sjlj_head);
    t->sjlj_head = fc;
    if (!t->top)
        rt_fatal("_Unwind_SjLj_Register(0x%08x) from a function without a host frame", fc);
    t->top->fc = fc;
}

void h_Unwind_SjLj_Unregister(CPU *c, u32 fc)
{
    RtThread *t = c->rt;
    t->sjlj_head = rd32(fc);
}

/* LSDA */

static u32 uleb(u32 *p)
{
    u32 v = 0, sh = 0;
    u8 b;
    do {
        b = *(u8 *)GP((*p)++);
        v |= (u32)(b & 0x7F) << sh;
        sh += 7;
    } while (b & 0x80);
    return v;
}

static s32 sleb(u32 *p)
{
    u32 v = 0, sh = 0;
    u8 b;
    do {
        b = *(u8 *)GP((*p)++);
        v |= (u32)(b & 0x7F) << sh;
        sh += 7;
    } while (b & 0x80);
    if (sh < 32 && (b & 0x40))
        v |= ~0u << sh;
    return (s32)v;
}

static u32 enc_size(u8 enc)
{
    switch (enc & 0x0F) {
    case 0x00: case 0x03: case 0x0B: return 4;
    case 0x02: case 0x0A: return 2;
    case 0x04: case 0x0C: return 8;
    }
    return 4;
}

static u32 read_enc(u32 *p, u8 enc)
{
    if (enc == 0xFF)
        return 0;
    u32 at = *p, v;
    switch (enc & 0x0F) {
    case 0x01: v = uleb(p); break;
    case 0x09: v = (u32)sleb(p); break;
    case 0x02: v = *(u16 *)GP(at); *p += 2; break;
    case 0x0A: v = (u32)(s32)*(s16 *)GP(at); *p += 2; break;
    default: v = rd32(at); *p += 4; break;
    }
    if (v && (enc & 0x70) == 0x10)
        v += at;
    if (v && (enc & 0x80))
        v = rd32(v);
    return v;
}

/* type matching */

static int same_type(u32 a, u32 b)
{
    if (a == b)
        return 1;
    const char *na = GP(rd32(a + 4)), *nb = GP(rd32(b + 4));
    if (*na == '*' || *nb == '*')
        return 0;
    return !strcmp(na, nb);
}

/* Can a handler for catch_ti take an object of thrown_ti at *obj (adjusted to the base)? */
static int can_catch(u32 catch_ti, u32 thrown_ti, u32 *obj)
{
    if (same_type(catch_ti, thrown_ti))
        return 1;
    u32 vp = rd32(thrown_ti);
    if (vp == GA(vt_si) + 8)
        return can_catch(catch_ti, rd32(thrown_ti + 8), obj);
    if (vp == GA(vt_vmi) + 8) {
        u32 n = rd32(thrown_ti + 12);
        for (u32 i = 0; i < n; i++) {
            u32 base = rd32(thrown_ti + 16 + 8 * i), of = rd32(thrown_ti + 20 + 8 * i);
            if (!(of & 2))
                continue;                  /* not public */
            s32 off = (s32)of >> 8;
            u32 p = *obj;
            if (of & 1)
                off = (s32)rd32(rd32(p) + (u32)off);
            u32 q = p + (u32)off;
            if (can_catch(catch_ti, base, &q)) {
                *obj = q;
                return 1;
            }
        }
    }
    return 0;
}

static u32 eh_vtable_objc(void);

/* Does the action chain at `action` (1-based offset into the action table) catch the
   exception?  Returns the selector (> 0 catch, 0 cleanup only, -1 nothing). */
static s32 match_actions(u32 action_table, u32 ttype_base, u8 ttype_enc, u32 action, u32 hdr, int *cleanup)
{
    u32 a = action_table + action - 1;
    *cleanup = 0;
    for (;;) {
        u32 p = a;
        s32 filter = sleb(&p);
        u32 q = p;
        s32 next = sleb(&q);
        if (filter > 0) {
            u32 tp = ttype_base - (u32)filter * enc_size(ttype_enc);
            u32 ti = read_enc(&tp, ttype_enc);
            u32 type = rd32(hdr + X_TYPE);
            if (!ti)
                return filter;             /* catch (...) */
            if (type == GA(&objc_marker)) {
                if (rd32(ti) == eh_vtable_objc() && objc_ehtype_matches(ti, rd32(OBJ(hdr))))
                    return filter;
            } else if (rd32(ti) != eh_vtable_objc()) {
                u32 obj = OBJ(hdr);
                if (can_catch(ti, type, &obj)) {
                    wr32(hdr + X_ADJUSTED, obj);
                    return filter;
                }
            }
        } else if (filter == 0) {
            *cleanup = 1;
        }
        if (!next)
            return *cleanup ? 0 : -1;
        a = p + (u32)next;
    }
}

/* What the function context fc does with the exception: -1 nothing, 0 cleanup, >0 handler.
   *lp is the landing pad index for call_site. */
static s32 personality(u32 fc, u32 hdr, u32 *lp)
{
    s32 cs = (s32)rd32(fc + 4);
    if (cs < 0)
        return -1;
    if (cs == 0)
        rt_fatal("exception through a no-throw region (function context 0x%08x)", fc);
    u32 p = rd32(fc + 0x1C);
    u8 lpstart_enc = *(u8 *)GP(p++);
    if (lpstart_enc != 0xFF)
        read_enc(&p, lpstart_enc);
    u8 ttype_enc = *(u8 *)GP(p++);
    u32 ttype_base = 0;
    if (ttype_enc != 0xFF) {
        u32 off = uleb(&p);
        ttype_base = p + off;
    }
    p++;                                    /* call-site encoding: uleb128 for SjLj */
    u32 cs_len = uleb(&p), cs_table = p, action_table = cs_table + cs_len;
    u32 lpi = 0, action = 0;
    for (s32 i = 0; i < cs; i++) {
        lpi = uleb(&p);
        action = uleb(&p);
    }
    *lp = lpi;
    if (!action)
        return 0;                           /* cleanup only */
    int cleanup;
    return match_actions(action_table, ttype_base, ttype_enc, action, hdr, &cleanup);
}

/* raising */

static const char *type_name(u32 hdr)
{
    u32 t = rd32(hdr + X_TYPE);
    if (t == GA(&objc_marker))
        return objc_class_name(rd32(rd32(OBJ(hdr))));
    return t ? (const char *)GP(rd32(t + 4)) : "?";
}

static __attribute__((noreturn)) void uncaught(CPU *c, u32 hdr)
{
    u32 t = rd32(hdr + X_TYPE);
    if (t == GA(&objc_marker)) {
        id e = rd32(OBJ(hdr));
        rt_fatal("uncaught Objective-C exception %s (object 0x%08x)", objc_class_name(rd32(e)), e);
    }
    rt_fatal("uncaught C++ exception of type %s (return address 0x%08x)", type_name(hdr), c->r[14]);
}

/* Phase 2 from the current head: run cleanups, then the handler found in phase 1. */
static __attribute__((noreturn)) void unwind_phase2(CPU *c, u32 hdr)
{
    RtThread *t = c->rt;
    for (u32 fc = t->sjlj_head; fc; fc = rd32(fc)) {
        u32 lp = 0;
        s32 sel = personality(fc, hdr, &lp);
        if (sel < 0)
            continue;
        if (rt_trace)
            rt_log("[eh] %s: landing pad %u in context 0x%08x (selector %d)\n", type_name(hdr), lp, fc, sel);
        t->sjlj_head = fc;
        wr32(fc + 8, hdr + UE_OFF);
        wr32(fc + 12, (u32)sel);
        wr32(fc + 4, lp);
        rt_resume_context(c, fc);
    }
    uncaught(c, hdr);
}

static __attribute__((noreturn)) void raise(CPU *c, u32 hdr)
{
    RtThread *t = c->rt;
    u32 fc;
    for (fc = t->sjlj_head; fc; fc = rd32(fc)) {
        u32 lp;
        if (personality(fc, hdr, &lp) > 0)
            break;
    }
    if (!fc)
        uncaught(c, hdr);
    unwind_phase2(c, hdr);
}

void h_Unwind_SjLj_Resume(CPU *c, u32 ue) { unwind_phase2(c, HDR_OF_UE(ue)); }

/* the C++ ABI */

u32 h_cxa_allocate_exception(u32 size)
{
    u8 *p = calloc(1, HDR_SIZE + size);
    if (!p)
        rt_fatal("out of memory (exception)");
    return GA(p) + HDR_SIZE;
}

void h_cxa_free_exception(u32 obj) { free(GP(HDR_OF_OBJ(obj))); }

void h_cxa_throw(CPU *c, u32 obj, u32 tinfo, u32 dtor)
{
    u32 h = HDR_OF_OBJ(obj);
    wr32(h + X_TYPE, tinfo);
    wr32(h + X_DTOR, dtor);
    wr32(h + X_ADJUSTED, obj);
    memcpy(GP(h + UE_OFF), "GNUCC++\0", 8);
    ((RtThread *)c->rt)->eh_uncaught++;
    if (rt_trace)
        rt_log("[eh] throw %s (return address 0x%08x)\n", type_name(h), c->r[14]);
    raise(c, h);
}

u32 h_cxa_begin_catch(CPU *c, u32 ue)
{
    RtThread *t = c->rt;
    u32 h = HDR_OF_UE(ue);
    s32 n = (s32)rd32(h + X_HANDLERS);
    n = n < 0 ? -n + 1 : n + 1;
    wr32(h + X_HANDLERS, (u32)n);
    if (t->eh_caught != h) {
        wr32(h + X_NEXT, t->eh_caught);
        t->eh_caught = h;
    }
    if (t->eh_uncaught)
        t->eh_uncaught--;
    if (rd32(h + X_TYPE) == GA(&objc_marker))
        return rd32(OBJ(h));
    return rd32(h + X_ADJUSTED);
}

void h_cxa_end_catch(CPU *c)
{
    RtThread *t = c->rt;
    u32 h = t->eh_caught;
    if (!h)
        return;
    s32 n = (s32)rd32(h + X_HANDLERS);
    if (n < 0) {                            /* rethrown: still alive */
        n++;
        wr32(h + X_HANDLERS, (u32)n);
        if (!n)
            t->eh_caught = rd32(h + X_NEXT);
        return;
    }
    wr32(h + X_HANDLERS, (u32)--n);
    if (n)
        return;
    t->eh_caught = rd32(h + X_NEXT);
    u32 dtor = rd32(h + X_DTOR);
    if (dtor && rd32(h + X_TYPE) != GA(&objc_marker))
        rt_call1(dtor, OBJ(h));
    free(GP(h));
}

void h_cxa_rethrow(CPU *c)
{
    RtThread *t = c->rt;
    u32 h = t->eh_caught;
    if (!h)
        rt_fatal("rethrow with no exception being handled");
    wr32(h + X_HANDLERS, (u32)-(s32)rd32(h + X_HANDLERS));
    t->eh_uncaught++;
    raise(c, h);
}

void h_cxa_call_unexpected(CPU *c, u32 ue)
{
    rt_fatal("exception %s broke an exception specification (return address 0x%08x)", type_name(HDR_OF_UE(ue)),
             c->r[14]);
}

/* Objective-C exceptions */

u32 objc_ehtype_vtable(void);
static u32 eh_vtable_objc(void) { return objc_ehtype_vtable(); }

static void gt_objc_throw(CPU *c, id exc)
{
    u32 obj = h_cxa_allocate_exception(4);
    wr32(obj, exc);
    u32 h = HDR_OF_OBJ(obj);
    wr32(h + X_TYPE, GA(&objc_marker));
    memcpy(GP(h + UE_OFF), "GNUCOBJC", 8);
    ((RtThread *)c->rt)->eh_uncaught++;
    rt_log("[eh] Objective-C exception %s thrown (return address 0x%08x)\n", objc_class_name(rd32(exc)), c->r[14]);
    raise(c, h);
}

void h_objc_exception_throw(CPU *c, u32 exc) { gt_objc_throw(c, exc); }
u32 h_objc_begin_catch(CPU *c, u32 ue) { return h_cxa_begin_catch(c, ue); }
void h_objc_end_catch(CPU *c) { h_cxa_end_catch(c); }
void h_objc_exception_rethrow(CPU *c) { h_cxa_rethrow(c); }

/* std::__throw_*: the game does not catch these; stop with the reason. */
void h_ZSt17__throw_bad_allocv(CPU *c) { rt_fatal("std::bad_alloc (return address 0x%08x)", c->r[14]); }
void h_ZSt19__throw_logic_errorPKc(CPU *c, u32 m) { (void)c; rt_fatal("std::logic_error: %s", (const char *)GP(m)); }
void h_ZSt20__throw_length_errorPKc(CPU *c, u32 m) { (void)c; rt_fatal("std::length_error: %s", (const char *)GP(m)); }
void h_ZSt20__throw_out_of_rangePKc(CPU *c, u32 m) { (void)c; rt_fatal("std::out_of_range: %s", (const char *)GP(m)); }

/* std::runtime_error { vptr; std::string msg; } */

void h_ZNSsC1ERKSs(u32 self, u32 other);
void h_ZNSsD1Ev(u32 self);
static u32 runtime_error_vtable[5];

static void re_what(CPU *c) { c->r[0] = rd32(c->r[0] + 4); }
static void re_dtor(CPU *c) { h_ZNSsD1Ev(c->r[0] + 4); }

u32 h_ZNSt13runtime_errorC1ERKSs(u32 self, u32 msg)
{
    if (!runtime_error_vtable[2]) {
        cxx_data_import("__ZTISt13runtime_error");
        runtime_error_vtable[1] = GA(&ti_runtime_error);
        runtime_error_vtable[2] = rt_host_addr(re_dtor, "std::runtime_error::~runtime_error");
        runtime_error_vtable[3] = runtime_error_vtable[2];
        runtime_error_vtable[4] = rt_host_addr(re_what, "std::runtime_error::what");
    }
    wr32(self, GA(&runtime_error_vtable[2]));
    h_ZNSsC1ERKSs(self + 4, msg);
    return self;
}
void h_ZNSt13runtime_errorD2Ev(u32 self) { h_ZNSsD1Ev(self + 4); }
u32 h_ZNKSt13runtime_error4whatEv(u32 self) { return rd32(self + 4); }
