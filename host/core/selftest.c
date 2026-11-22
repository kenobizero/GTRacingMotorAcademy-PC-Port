/* selftest.c -- checks of the host's Foundation through the same message paths the game uses
   (gtracing.exe --selftest, after the image is loaded).  Prints each failure and a count. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "game_hooks.h"
#include "gl.h"
#include "liveblocks.h"
#include "ns.h"

void I_objc_msgSend_stret(CPU *c);
u32 h_NSSearchPathForDirectoriesInDomains(u32 dir, u32 mask, u32 expand);

static int fails, checks;

/* the file's bare name: __FILE__ was its whole path, the Windows user's folder in the exe */
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; rt_log("[selftest] FAIL selftest.c:%d: ", __LINE__); rt_log(__VA_ARGS__); rt_log("\n"); } } while (0)

static u32 cls(const char *n) { return ns_class(n); }
static id S(const char *s) { return ns_string(s); }
static int streq(id s, const char *want) { return s && !strcmp(ns_utf8(s), want); }

/* a message with up to 6 argument words */
static u32 M(id self, const char *sel, int n, ...)
{
    u32 a[6] = {0};
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        a[i] = va_arg(ap, u32);
    va_end(ap);
    return objc_msg(self, sel, n, a);
}

/* a struct-returning message through objc_msgSend_stret, as the game sends it */
static void stret(u32 *out, id self, const char *sel, int n, const u32 *args)
{
    CPU *c = rt_cpu();
    CPU save = *c;
    u32 sp = (c->r[13] - 256) & ~7u;
    c->r[0] = GA(out);
    c->r[1] = self;
    c->r[2] = sel_get(sel);
    for (int i = 0; i < n; i++) {
        if (i == 0)
            c->r[3] = args[0];
        else
            wr32(sp + 4 * (u32)(i - 1), args[i]);
    }
    c->r[13] = sp;
    I_objc_msgSend_stret(c);
    *c = save;
}

/* NSString formats get their words as the variadic part */
static id fmt(const char *f, int n, const u32 *w)
{
    Buf b = {0};
    ns_format(&b, f, w);
    (void)n;
    id s = ns_string(b.p ? b.p : "");
    buf_free(&b);
    return s;
}

static void strings(void)
{
    u32 w1[] = {S("abc"), 42, 7, 0, (u32)d2bits(3.14159), (u32)(d2bits(3.14159) >> 32)};
    /* %@ %d %lld %.2f: the 64-bit values take two words, no alignment */
    u64 ll = 1234567890123ull;
    u32 w2[] = {S("x"), (u32)-5, (u32)ll, (u32)(ll >> 32), (u32)d2bits(2.5), (u32)(d2bits(2.5) >> 32), GA("cs")};
    id s = fmt("%@ %d %lld %.2f %s", 7, w2);
    CHECK(streq(s, "x -5 1234567890123 2.50 cs"), "format gave '%s'", ns_utf8(s));
    (void)w1;
    u32 w3[] = {5, GA("ab"), 0x41};
    s = fmt("[%3d][%-4s][%C][%%]", 3, w3);
    CHECK(streq(s, "[  5][ab  ][A][%]"), "format widths gave '%s'", ns_utf8(s));

    id p = S("/var/mobile/a.app/Data/file.tar.gz");
    CHECK(streq(M(p, "lastPathComponent", 0), "file.tar.gz"), "lastPathComponent");
    CHECK(streq(M(p, "pathExtension", 0), "gz"), "pathExtension");
    CHECK(streq(M(p, "stringByDeletingPathExtension", 0), "/var/mobile/a.app/Data/file.tar"), "deletingPathExtension");
    CHECK(streq(M(p, "stringByDeletingLastPathComponent", 0), "/var/mobile/a.app/Data"), "deletingLastPathComponent");
    CHECK(streq(M(S("/a/b/"), "stringByAppendingPathComponent:", 1, S("c")), "/a/b/c"), "appendingPathComponent");
    CHECK(streq(M(S(""), "stringByAppendingPathComponent:", 1, S("c")), "c"), "appending to empty");
    CHECK(streq(M(S("/a"), "stringByDeletingLastPathComponent", 0), "/"), "deleting to root");
    CHECK(streq(M(S("/a/./b/../c"), "stringByStandardizingPath", 0), "/a/c"), "standardizingPath");
    id comps = M(S("/a/b"), "pathComponents", 0);
    CHECK(ns_array_count(comps) == 3 && streq(ns_array_at(comps, 0), "/") && streq(ns_array_at(comps, 2), "b"),
          "pathComponents");

    id h = S("Hello, World");
    CHECK(M(h, "length", 0) == 12, "length");
    CHECK(M(h, "characterAtIndex:", 1, 4) == 'o', "characterAtIndex");
    CHECK(M(h, "hasPrefix:", 1, S("Hell")) & 1, "hasPrefix");
    CHECK(M(h, "hasSuffix:", 1, S("World")) & 1, "hasSuffix");
    CHECK(streq(M(h, "substringFromIndex:", 1, 7), "World"), "substringFromIndex");
    CHECK(streq(M(h, "substringWithRange:", 2, 7, 3), "Wor"), "substringWithRange");
    CHECK(streq(M(h, "uppercaseString", 0), "HELLO, WORLD"), "uppercaseString");
    CHECK(streq(M(h, "stringByReplacingOccurrencesOfString:withString:", 2, S("o"), S("0")), "Hell0, W0rld"), "replace");
    u32 r[2] = {0, 0}, a1[1] = {S("World")};
    stret(r, h, "rangeOfString:", 1, a1);
    CHECK(r[0] == 7 && r[1] == 5, "rangeOfString: {%u, %u}", r[0], r[1]);
    u32 a2[2] = {S("world"), 1};
    stret(r, h, "rangeOfString:options:", 2, a2);
    CHECK(r[0] == 7 && r[1] == 5, "rangeOfString:options: case-insensitive {%u, %u}", r[0], r[1]);
    u32 a3[1] = {S("zz")};
    stret(r, h, "rangeOfString:", 1, a3);
    CHECK(r[0] == NS_NOT_FOUND && r[1] == 0, "rangeOfString: missing");
    id parts = M(S("a,b,,c"), "componentsSeparatedByString:", 1, S(","));
    CHECK(ns_array_count(parts) == 4 && streq(ns_array_at(parts, 2), ""), "componentsSeparatedByString");
    CHECK(streq(M(parts, "componentsJoinedByString:", 1, S("-")), "a-b--c"), "componentsJoinedByString");
    CHECK(M(S("  42abc"), "intValue", 0) == 42, "intValue");
    CHECK(M(S("YES"), "boolValue", 0) == 1, "boolValue");
    CHECK((s32)M(S("abc"), "compare:", 1, S("abd")) == -1, "compare");
    CHECK((s32)M(S("file10"), "compare:options:", 2, S("file9"), 64) == 1, "numeric compare");
    id ws = M(cls("NSCharacterSet"), "whitespaceAndNewlineCharacterSet", 0);
    CHECK(streq(M(S(" \t hi \n"), "stringByTrimmingCharactersInSet:", 1, ws), "hi"), "trimming");

    /* UTF-8 with a non-ASCII character: indices count UTF-16 units */
    id u = S("caf\xC3\xA9!");
    CHECK(M(u, "length", 0) == 5, "non-ASCII length %u", M(u, "length", 0));
    CHECK(M(u, "characterAtIndex:", 1, 3) == 0xE9, "non-ASCII characterAtIndex");
    CHECK(streq(M(u, "substringFromIndex:", 1, 4), "!"), "non-ASCII substring");

    id m = M(M(cls("NSMutableString"), "alloc", 0), "init", 0);
    M(m, "appendString:", 1, S("ab"));
    M(m, "appendFormat:", 3, S("%d-%@"), 9, S("z"));
    M(m, "insertString:atIndex:", 2, S("X"), 1);
    CHECK(streq(m, "aXb9-z"), "mutable string '%s'", ns_utf8(m));
    objc_release(m);

    id hs = M(cls("NSString"), "stringWithFormat:", 3, S("%@=%d"), S("k"), 3);
    CHECK(streq(hs, "k=3"), "stringWithFormat: '%s'", ns_utf8(hs));
    id d = M(S("bytes"), "dataUsingEncoding:", 1, 4);
    CHECK(ns_is_data(d) && M(d, "length", 0) == 5, "dataUsingEncoding");
    id back = M(M(cls("NSString"), "alloc", 0), "initWithData:encoding:", 2, d, 4);
    CHECK(streq(back, "bytes"), "initWithData:encoding:");
    objc_release(back);
}

static void collections(void)
{
    id a = M(cls("NSMutableArray"), "array", 0);
    M(a, "addObject:", 1, S("pear"));
    M(a, "addObject:", 1, S("apple"));
    M(a, "addObject:", 1, S("fig"));
    M(a, "sortUsingSelector:", 1, sel_get("compare:"));
    CHECK(streq(ns_array_at(a, 0), "apple") && streq(ns_array_at(a, 2), "pear"), "sortUsingSelector");
    CHECK(M(a, "indexOfObject:", 1, S("fig")) == 1, "indexOfObject (equal, not identical)");
    CHECK(M(a, "containsObject:", 1, S("pear")) & 1, "containsObject");
    M(a, "removeObjectAtIndex:", 1, 0);
    CHECK(M(a, "count", 0) == 2 && streq(M(a, "lastObject", 0), "pear"), "removeObjectAtIndex");

    /* fast enumeration as the compiler emits it: state struct, itemsPtr */
    u32 state[8] = {0}, buf[16];
    u32 n = M(a, "countByEnumeratingWithState:objects:count:", 3, GA(state), GA(buf), 16);
    CHECK(n == 2 && rd32(state[1]) == ns_array_at(a, 0), "fast enumeration");

    id d = M(cls("NSMutableDictionary"), "dictionary", 0);
    for (int i = 0; i < 200; i++)
        M(d, "setObject:forKey:", 2, ns_num_ll(i), ns_stringf("key%d", i));
    CHECK(M(d, "count", 0) == 200, "dictionary count");
    id v = M(d, "objectForKey:", 1, S("key123"));
    CHECK(v && ns_num_int(v) == 123, "objectForKey after 200 inserts");
    M(d, "removeObjectForKey:", 1, S("key5"));
    CHECK(M(d, "count", 0) == 199 && !M(d, "objectForKey:", 1, S("key5")) && M(d, "objectForKey:", 1, S("key6")),
          "removeObjectForKey");
    CHECK(ns_num_int(M(d, "objectForKey:", 1, S("key199"))) == 199, "lookup after a removal");

    id lit = M(cls("NSDictionary"), "dictionaryWithObjectsAndKeys:", 5, S("v1"), S("k1"), ns_num_ll(2), S("k2"), 0);
    CHECK(M(lit, "count", 0) == 2 && streq(M(lit, "objectForKey:", 1, S("k1")), "v1"), "dictionaryWithObjectsAndKeys");
    id keys = M(M(lit, "allKeys", 0), "sortedArrayUsingSelector:", 1, sel_get("compare:"));
    CHECK(streq(ns_array_at(keys, 1), "k2"), "allKeys sorted");

    id num = M(cls("NSNumber"), "numberWithFloat:", 1, f2bits(1.5f));
    CHECK(bits2f(M(num, "floatValue", 0)) == 1.5f && M(num, "intValue", 0) == 1, "NSNumber float");
    id n2 = M(cls("NSNumber"), "numberWithInt:", 1, 1);
    CHECK(M(n2, "isEqual:", 1, M(cls("NSNumber"), "numberWithDouble:", 2, (u32)d2bits(1.0), (u32)(d2bits(1.0) >> 32))) & 1,
          "NSNumber 1 == 1.0");
    CHECK(M(cls("NSNumber"), "numberWithBool:", 1, 1) == ns_num_bool(1), "shared YES");

    id set = M(cls("NSMutableSet"), "set", 0);
    M(set, "addObject:", 1, S("a"));
    M(set, "addObject:", 1, S("a"));
    M(set, "addObject:", 1, S("b"));
    CHECK(M(set, "count", 0) == 2 && (M(set, "containsObject:", 1, S("b")) & 1), "NSSet");

    /* property list round trip through a file in the home folder */
    id root = M(cls("NSMutableDictionary"), "dictionary", 0);
    M(root, "setObject:forKey:", 2, S("text <&>"), S("s"));
    M(root, "setObject:forKey:", 2, ns_num_ll(-7), S("i"));
    M(root, "setObject:forKey:", 2, ns_num_d(0.25), S("r"));
    M(root, "setObject:forKey:", 2, ns_num_bool(1), S("b"));
    M(root, "setObject:forKey:", 2, a, S("a"));
    M(root, "setObject:forKey:", 2, objc_autorelease(ns_data_new("\x01\x02\xff", 3)), S("d"));
    id path = ns_stringf("%s/tmp/selftest.plist", gt_guest_home);
    CHECK(M(root, "writeToFile:atomically:", 2, path, 1) & 1, "writeToFile");
    id rd = M(cls("NSDictionary"), "dictionaryWithContentsOfFile:", 1, path);
    CHECK(rd && M(rd, "count", 0) == 6, "dictionaryWithContentsOfFile");
    CHECK(rd && streq(M(rd, "objectForKey:", 1, S("s")), "text <&>"), "plist string");
    CHECK(rd && ns_num_int(M(rd, "objectForKey:", 1, S("i"))) == -7, "plist integer");
    CHECK(rd && ns_num_double(M(rd, "objectForKey:", 1, S("r"))) == 0.25, "plist real");
    CHECK(rd && M(rd, "objectForKey:", 1, S("b")) == ns_num_bool(1), "plist bool");
    CHECK(rd && ns_array_count(M(rd, "objectForKey:", 1, S("a"))) == 2, "plist array");
    id dd = rd ? M(rd, "objectForKey:", 1, S("d")) : 0;
    CHECK(dd && M(dd, "length", 0) == 3 && ((const u8 *)GP(M(dd, "bytes", 0)))[2] == 0xff, "plist data");
}

static void sys_checks(void)
{
    id b = M(cls("NSBundle"), "mainBundle", 0);
    id ident = M(b, "bundleIdentifier", 0);
    CHECK(ident && ns_utf8_len(ident) > 0, "bundleIdentifier (Info.plist)");
    rt_log("[selftest] bundle %s, identifier %s, version %s\n", ns_utf8(M(b, "bundlePath", 0)), ns_utf8(ident),
           ns_describe(M(b, "objectForInfoDictionaryKey:", 1, S("CFBundleVersion"))));
    id plist = M(b, "pathForResource:ofType:", 2, S("Info"), S("plist"));
    CHECK(plist != 0, "pathForResource Info.plist");
    id fm = M(cls("NSFileManager"), "defaultManager", 0);
    CHECK(M(fm, "fileExistsAtPath:", 1, plist) & 1, "fileExistsAtPath");
    u8 isdir = 0;
    CHECK((M(fm, "fileExistsAtPath:isDirectory:", 2, M(b, "bundlePath", 0), GA(&isdir)) & 1) && isdir,
          "bundle is a directory");
    id docs = ns_array_at(h_NSSearchPathForDirectoriesInDomains(9, 1, 1), 0);
    id sub = M(docs, "stringByAppendingPathComponent:", 1, S("selftest/deep"));
    CHECK(M(fm, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", 4, sub, 1, 0, 0) & 1, "mkdir -p");
    CHECK(M(fm, "removeItemAtPath:error:", 2, M(docs, "stringByAppendingPathComponent:", 1, S("selftest")), 0) & 1,
          "remove tree");

    id ud = M(cls("NSUserDefaults"), "standardUserDefaults", 0);
    M(ud, "setInteger:forKey:", 2, 77, S("selftest_int"));
    M(ud, "setFloat:forKey:", 2, f2bits(0.5f), S("selftest_float"));
    CHECK(M(ud, "integerForKey:", 1, S("selftest_int")) == 77, "user defaults integer");
    CHECK(bits2f(M(ud, "floatForKey:", 1, S("selftest_float"))) == 0.5f, "user defaults float");
    M(ud, "removeObjectForKey:", 1, S("selftest_int"));
    M(ud, "removeObjectForKey:", 1, S("selftest_float"));
    CHECK(M(ud, "synchronize", 0) & 1, "synchronize");

    double t0 = ns_date_value(M(cls("NSDate"), "date", 0));
    CHECK(t0 > 7e8 && t0 < 2e9, "NSDate now %.0f", t0);

    /* a thread (pthread_create -> host entry -> message on the new thread's own guest stack) */
    id box = M(cls("NSMutableArray"), "array", 0);
    M(cls("NSThread"), "detachNewThreadSelector:toTarget:withObject:", 3, sel_get("addObject:"), box, S("from thread"));
    for (int i = 0; i < 200 && ns_array_count(box) == 0; i++)
        Sleep(5);
    CHECK(ns_array_count(box) == 1 && streq(ns_array_at(box, 0), "from thread"), "NSThread detach");

    /* a repeating timer on this thread's run loop: the timer passes itself to addObject: */
    id ticks = M(cls("NSMutableArray"), "array", 0);
    double iv = 0.01;
    u32 targs[6] = {(u32)d2bits(iv), (u32)(d2bits(iv) >> 32), ticks, sel_get("addObject:"), 0, 1};
    id timer = objc_msg(cls("NSTimer"), "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", 6, targs);
    for (int i = 0; i < 100 && ns_array_count(ticks) < 3; i++) {
        Sleep(5);
        ns_run_loop_once();
    }
    CHECK(ns_array_count(ticks) >= 3 && ns_array_at(ticks, 0) == timer, "timer fired %u times", ns_array_count(ticks));
    M(timer, "invalidate", 0);
    u32 before = ns_array_count(ticks);
    Sleep(30);
    ns_run_loop_once();
    CHECK(ns_array_count(ticks) == before, "invalidated timer stays quiet");
}

/* an import through its generated bridge, with the argument words as the game passes them */
static u32 imp(const char *name, int n, const u32 *w)
{
    RtBridge fn = rt_bridge_by_name(name);
    if (!fn)
        return 0xdeadbeef;
    CPU *c = rt_cpu();
    CPU save = *c;
    u32 sp = (c->r[13] - 256) & ~7u;
    for (int i = 0; i < n; i++) {
        if (i < 4)
            c->r[i] = w[i];
        else
            wr32(sp + 4 * (u32)(i - 4), w[i]);
    }
    c->r[13] = sp;
    fn(c);
    u32 r = c->r[0];
    *c = save;
    return r;
}
#define FB(x) f2bits(x)
static int feq(float a, float b) { return a - b < 1e-4f && b - a < 1e-4f; }

static void coregraphics(void)
{
    float t[6] = {0};
    u32 w1[] = {GA(t), FB(1.5707964f)};
    imp("_CGAffineTransformMakeRotation", 2, w1);
    CHECK(feq(t[0], 0) && feq(t[1], 1) && feq(t[2], -1) && feq(t[3], 0), "MakeRotation gave %g %g %g %g",
          t[0], t[1], t[2], t[3]);
    /* Concat(scale 2, translate 10,20): the second operand's words are on the stack */
    float u[6] = {0};
    u32 w2[] = {GA(u), FB(2), 0, 0, FB(2), 0, 0, FB(1), 0, 0, FB(1), FB(10), FB(20)};
    imp("_CGAffineTransformConcat", 13, w2);
    CHECK(feq(u[0], 2) && feq(u[3], 2) && feq(u[4], 10) && feq(u[5], 20), "Concat gave %g %g %g %g", u[0], u[3],
          u[4], u[5]);
    float r[4] = {0};
    u32 w3[] = {GA(r), FB(10), FB(20), FB(100), FB(50), FB(5), FB(-5)};
    imp("_CGRectInset", 7, w3);
    CHECK(feq(r[0], 15) && feq(r[1], 15) && feq(r[2], 90) && feq(r[3], 60), "Inset gave %g %g %g %g", r[0], r[1],
          r[2], r[3]);
    u32 w4[] = {FB(0), FB(0), FB(10), FB(10), FB(5), FB(9.5f)};
    CHECK(imp("_CGRectContainsPoint", 6, w4) == 1, "ContainsPoint inside");
    w4[5] = FB(10);
    CHECK(imp("_CGRectContainsPoint", 6, w4) == 0, "ContainsPoint on the far edge");
    u32 w5[] = {FB(-4), FB(0), FB(-6), FB(1)};
    CHECK(bits2f(imp("_CGRectGetMinX", 4, w5)) == -10 && bits2f(imp("_CGRectGetWidth", 4, w5)) == 6, "negative width");

    /* the IGP's pattern: an image context, a fill, the image, the end */
    u32 w6[] = {FB(480), FB(1024)};
    imp("_UIGraphicsBeginImageContext", 2, w6);
    u32 ctx = imp("_UIGraphicsGetCurrentContext", 0, NULL);
    CHECK(ctx != 0, "image context");
    u32 w7[] = {ctx, FB(0), FB(0), FB(0), FB(0.5f)};
    imp("_CGContextSetRGBFillColor", 5, w7);
    u32 w8[] = {ctx, FB(0), FB(0), FB(480), FB(1024)};
    imp("_CGContextFillRect", 5, w8);
    imp("_UIGraphicsGetImageFromCurrentImageContext", 0, NULL);
    imp("_UIGraphicsEndImageContext", 0, NULL);
    /* a bitmap context's image keeps the size; CFRelease works on it */
    u32 sp = imp("_CGColorSpaceCreateDeviceRGB", 0, NULL);
    u32 w9[] = {0, 64, 32, 8, 256, sp, 1};
    u32 bc = imp("_CGBitmapContextCreate", 7, w9);
    u32 img = imp("_CGBitmapContextCreateImage", 1, &bc);
    CHECK(imp("_CGImageGetWidth", 1, &img) == 64 && imp("_CGImageGetHeight", 1, &img) == 32, "bitmap image size");
    imp("_CFRelease", 1, &img);
    imp("_CGContextRelease", 1, &bc);
    imp("_CGColorSpaceRelease", 1, &sp);
}

/* FAST_DISPATCH (rt_core.c): the bucket table answers as the binary search does, for every function's address,
   the addresses just after (mid-function, or the next function), the Thumb bit set, and outside __text */
typedef struct { u32 addr; void (*fn)(CPU *); } StDispatch;
extern const StDispatch rt_dispatch[];
extern const unsigned rt_dispatch_count;
static void (*st_search(u32 a))(CPU *)
{
    unsigned lo = 0, hi = rt_dispatch_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rt_dispatch[mid].addr < a)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < rt_dispatch_count && rt_dispatch[lo].addr == a ? rt_dispatch[lo].fn : NULL;
}
static void dispatch_checks(void)
{
    unsigned bad = 0, n = 0;
    for (unsigned i = 0; i < rt_dispatch_count; i++)
        for (u32 d = 0; d < 6; d++) {
            u32 a = rt_dispatch[i].addr + d;
            n++;
            if (rt_lookup(a) != st_search(a) && bad++ < 5)
                rt_log("[selftest] dispatch 0x%08x: table and search differ\n", a);
        }
    static const u32 outside[] = {0, 0x10fff, 0x11000, 0x7fffffffu, 0xfffffffeu, 0xffffffffu};
    for (unsigned i = 0; i < sizeof outside / sizeof outside[0]; i++, n++)
        if (rt_lookup(outside[i]) != st_search(outside[i]))
            bad++;
    CHECK(bad == 0, "dispatch table: %u of %u addresses differ from the binary search", bad, n);
    CHECK(rt_dispatch_count && rt_lookup(rt_dispatch[0].addr) == rt_dispatch[0].fn &&
              rt_lookup(rt_dispatch[rt_dispatch_count - 1].addr) == rt_dispatch[rt_dispatch_count - 1].fn,
          "dispatch table: first and last function");
}

/* the game code's inline fmax / fmin (arm_rt.h, pass 4) give the C library's answers bit for bit, NaNs and signed
   zeros included (the library through a pointer, so gcc cannot fold it into its own) */
static void fminmax_checks(void)
{
    static const u64 bits[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
                               0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
                               0xfff8000000000001ull, 0x7ff0000000000001ull, 1, 0x8000000000000001ull,
                               0x400c000000000000ull, 0x400c000000000001ull};
    double (*volatile mx)(double, double) = fmax, (*volatile mn)(double, double) = fmin;
    unsigned n = sizeof bits / sizeof bits[0], bad = 0;
    for (unsigned i = 0; i < n; i++)
        for (unsigned j = 0; j < n; j++) {
            double a = bits2d(bits[i]), b = bits2d(bits[j]);
            bad += d2bits(imp_fmax_d(a, b)) != d2bits(mx(a, b));
            bad += d2bits(imp_fmin_d(a, b)) != d2bits(mn(a, b));
        }
    CHECK(bad == 0, "inline fmax/fmin: %u of %u answers differ from the C library's", bad, 2 * n * n);
}

/* a player's numeric setting (settings.c): a malformed or missing value is the default, an out-of-range one the
   nearer end, never NaN or infinite */
static void settings_checks(void)
{
    CHECK(rt_parse_int(" 42 ", 7, 0, 100) == 42 && rt_parse_int(NULL, 7, 0, 100) == 7, "settings: a whole number");
    CHECK(rt_parse_int("abc", 7, 0, 100) == 7 && rt_parse_int("12x", 7, 0, 100) == 7 && rt_parse_int("", 7, 0, 100) == 7,
          "settings: not a number -> the default");
    CHECK(rt_parse_int("-5", 7, 0, 100) == 0 && rt_parse_int("99999999999999999999", 7, 0, 100) == 100,
          "settings: out of range -> the nearer end");
    CHECK(rt_parse_float("2.5", 3, 1, 6) == 2.5f && rt_parse_float("9", 3, 1, 6) == 6 && rt_parse_float("-1", 3, 1, 6) == 1,
          "settings: a number with a fraction, clamped");
    CHECK(rt_parse_float("nan", 3, 1, 6) == 3 && rt_parse_float("inf", 3, 1, 6) == 3 && rt_parse_float("1e999", 3, 1, 6) == 3,
          "settings: NaN and infinities -> the default");
}

/* MISSING_CARS (missingcars.c): the garage written with its hidden cars back has the save's order */
static void missing_cars_checks(void)
{
    /* a garage of 9 whose cars 0, 3, 4 and 8 are hidden; the visible ones keep their order between */
    static const u32 place[] = {0, 3, 4, 8};
    int out[9], ok = 1;
    gt_missing_cars_order(place, 4, 5, out);
    static const int want[9] = {-1, 0, 1, -2, -3, 2, 3, 4, -4};
    for (int i = 0; i < 9; i++)
        ok &= out[i] == want[i];
    CHECK(ok, "missing cars: the whole garage's order");
    /* two cars sold since (3 visible left): the last hidden one goes to the end */
    int out2[7];
    gt_missing_cars_order(place, 4, 3, out2);
    static const int want2[7] = {-1, 0, 1, -2, -3, 2, -4};
    ok = 1;
    for (int i = 0; i < 7; i++)
        ok &= out2[i] == want2[i];
    CHECK(ok, "missing cars: the order with cars sold");
}

/* optimisation pass 6: FIXED_FAST, INDEX_FAST and PVRTC_FAST give the plain loops' bytes, LIVE_BLOCKS
   keeps the right blocks (gles.c, liveblocks.c) */
static void pass6_checks(void)
{
    int n = gles_selftest_fixed();
    CHECK(n == 0, "FIXED_FAST: %d of the GL_FIXED conversions differ from the plain loop", n);
    n = gles_selftest_index();
    CHECK(n == 0, "INDEX_FAST: %d of the index ranges differ from the plain loop", n);
    n = gles_selftest_pvrtc();
    CHECK(n == 0, "PVRTC_FAST: %d of 16 pictures decoded by bands differ (byte for byte) from one thread's", n);
    char why[200] = "";
    CHECK(gt_live_selftest(why, sizeof why) == 0, "LIVE_BLOCKS: %s", why);
}

/* CLOUD_BOARD and the leaderboard's names (board.c): SHA-256 / the server's alias, its answer read, the
   body sent, the URL rule, the merge (our own line never from outside, one line per player) and the rows' names */
static void cloud_board_checks(void)
{
    char why[200] = "";
    int n = board_selftest(why, sizeof why);
    CHECK(n == 0, "CLOUD_BOARD: %d check(s) failed, the first: %s", n, why);
}

/* MAX_STATS cheat code (maxmoney.c): the keyboard word IWANTMAXSTATS, the pad's LB + RB + Up Up Down Down
   X Y X Y, the resets (a wrong key or button, a bumper let go) and the main menu rule (nothing taken outside it) */
static void cheat_checks(void)
{
    char why[200] = "";
    int n = cheat_selftest(why, sizeof why);
    CHECK(n == 0, "MAX_STATS cheat: %d check(s) failed, the first: %s", n, why);
}

/* UTF16_CSTRING (ns_string.c): a plain ASCII string asked for in UTF-16 comes in UTF-16 (the profile's name,
   F_302bf0: [[UIDevice currentDevice] name] in UTF-16LE), and CFStringGetCStringPtr has no pointer for it */
u32 h_CFStringGetCStringPtr(u32 s, u32 enc);
static void utf16_checks(void)
{
    const char *e = getenv("GT_UTF16_CSTRING");
    if (e && *e == '0')
        return;
    id s = S("iPad");
    u32 p = M(s, "cStringUsingEncoding:", 1, 0x94000100u);
    static const u8 want[10] = {'i', 0, 'P', 0, 'a', 0, 'd', 0, 0, 0};
    CHECK(p && !memcmp(GP(p), want, sizeof want), "UTF-16LE C string of \"iPad\" is not UTF-16");
    CHECK(M(s, "lengthOfBytesUsingEncoding:", 1, 0x94000100u) == 8, "UTF-16LE length of \"iPad\" is not 8");
    u32 b = M(s, "cStringUsingEncoding:", 1, 0x90000100u);
    CHECK(b && !memcmp(GP(b), "\0i\0P\0a\0d", 8), "UTF-16BE C string of \"iPad\" is not UTF-16BE");
    CHECK(M(s, "cStringUsingEncoding:", 1, 4) == M(s, "UTF8String", 0), "the UTF-8 C string changed");
    CHECK(h_CFStringGetCStringPtr(s, 0x14000100u) == 0, "CFStringGetCStringPtr gave a UTF-16LE pointer");
    CHECK(h_CFStringGetCStringPtr(s, 0x08000100u) != 0, "CFStringGetCStringPtr gave no UTF-8 pointer");
}

/* the guest setjmp / longjmp and SjLj exceptions (rt_core.c, imp_unwind.c), through guest-style functions built
   like the generated code (runtime/rt_selftest.c): paths the menus and a race never take */
void rt_selftest_setjmp(CPU *c);
void rt_selftest_sjlj(CPU *c);
extern void (*rt_selftest_thrower)(CPU *c);
extern int rt_selftest_cleanups;
u32 cxx_data_import(const char *name);
u32 h_cxa_allocate_exception(u32 size);
void h_cxa_throw(CPU *c, u32 obj, u32 tinfo, u32 dtor);
u32 h_cxa_begin_catch(CPU *c, u32 ue);
void h_cxa_end_catch(CPU *c);
void h_objc_exception_throw(CPU *c, u32 exc);
u32 objc_ehtype_vtable(void);

static u32 jmp_buf_g[64];
static u32 eh_obj, eh_objc;            /* the object thrown last */
static u32 fc_outer[16], fc_inner[16];

/* LSDA (SjLj): call site 1 catches std::exception (filter 1), call site 2 is a cleanup, call site 3 catches an
   Objective-C NSObject (filter 2), else anything (filter 3); landing pads 5, 6, 7 */
static u8 lsda[64];

static void lsda_make(void)
{
    u32 ti_re = cxx_data_import("__ZTISt13runtime_error");
    static u32 objc_ti[3];
    objc_ti[0] = objc_ehtype_vtable();
    objc_ti[1] = GA("NSObject");
    objc_ti[2] = cls("NSObject");
    /* the types table counts down from its base: filter 3, 2, 1 */
    u32 types[3] = {0, GA(objc_ti), rd32(ti_re + 8)};   /* catch (...), @catch (NSObject *), std::exception */
    static const u8 cs[] = {5, 1, 6, 0, 7, 3};           /* (landing pad, action) per call site */
    static const u8 act[] = {1, 0, 2, 1, 3, 0};          /* (filter, next) records: 1; 2 then 3 */
    u8 *p = lsda;
    *p++ = 0xff;                       /* no landing pad base */
    *p++ = 0x00;                       /* types: absolute pointers */
    u8 *off = p++;                     /* uleb128: from after it to the types' base */
    *p++ = 0x01;                       /* call sites: uleb128 */
    *p++ = sizeof cs;
    memcpy(p, cs, sizeof cs), p += sizeof cs;
    memcpy(p, act, sizeof act), p += sizeof act;
    while ((u32)(p - lsda) % 4)
        *p++ = 0;
    memcpy(p, types, sizeof types), p += sizeof types;
    *off = (u8)(p - (off + 1));
}

static void throw_cxx(CPU *c)
{
    u32 ti_re = cxx_data_import("__ZTISt13runtime_error");
    eh_obj = h_cxa_allocate_exception(8);
    wr32(eh_obj, 0x5e1f7e57u);
    h_cxa_throw(c, eh_obj, ti_re, 0);
}
static void throw_objc(CPU *c)
{
    eh_objc = M(M(cls("NSObject"), "alloc", 0), "init", 0);
    h_objc_exception_throw(c, eh_objc);
}
static void throw_nested(CPU *c)       /* an inner function with a cleanup (call site 2) around the throw */
{
    memset(fc_inner, 0, sizeof fc_inner);
    fc_inner[7] = GA(lsda);            /* +0x1c */
    rt_selftest_thrower = throw_cxx;
    c->r[13] -= 64;
    c->r[0] = GA(fc_inner), c->r[1] = 2;
    rt_selftest_sjlj(c);
}

/* one call of rt_selftest_sjlj on fc_outer at call site cs; out = r0..r2 (landing pad, selector, exception) */
static void sjlj_run(u32 cs, void (*thrower)(CPU *), u32 out[3])
{
    CPU *c = rt_cpu();
    RtThread *t = c->rt;
    CPU save = *c;
    memset(fc_outer, 0, sizeof fc_outer);
    fc_outer[7] = GA(lsda);            /* +0x1c */
    c->r[13] = (c->r[13] - 512) & ~7u;
    c->r[0] = GA(fc_outer), c->r[1] = cs;
    rt_selftest_thrower = thrower;
    RtFrame *top = t->top;
    u32 head = t->sjlj_head;
    rt_selftest_sjlj(c);
    out[0] = c->r[0], out[1] = c->r[1], out[2] = c->r[2];
    CHECK(t->top == top && t->sjlj_head == head, "SjLj: the frame list or the context list was not put back");
    *c = save;
}

static void unwind_checks(void)
{
    CPU *c = rt_cpu();
    RtThread *t = c->rt;
    /* setjmp / longjmp: the second return gives val (1 for 0), the registers come back, the frames above go */
    for (int k = 0; k < 2; k++) {
        CPU save = *c;
        RtFrame *top = t->top;
        u32 sp = (c->r[13] - 512) & ~7u, val = k ? 0 : 7;
        c->r[13] = sp;
        c->r[0] = GA(jmp_buf_g), c->r[1] = val;
        rt_selftest_setjmp(c);
        CHECK(c->r[0] == (val ? val : 1), "longjmp(buf, %u): setjmp returned %u", val, c->r[0]);
        CHECK(c->r[4] == 0x44444444u && c->r[5] == 0x55555555u && c->r[6] == 0x66666666u &&
              c->r[7] == 0x77777777u && c->r[8] == 0x88888888u && c->r[10] == 0xaaaaaaaau &&
              c->r[11] == 0xbbbbbbbbu, "longjmp: callee-saved registers not restored (r4 %08x r7 %08x)",
              c->r[4], c->r[7]);
        CHECK(c->r[13] == sp, "longjmp: sp %08x, setjmp's was %08x", c->r[13], sp);
        CHECK(c->V.d[8] == 0x0808080808080808ull, "longjmp: d8 not restored");
        CHECK(t->top == top, "longjmp: the callee's SjLj frame was left on the list");
        *c = save;
    }

    lsda_make();
    u32 out[3];
    /* a C++ exception caught by its base class: landing pad 5, selector 1, the object as thrown */
    sjlj_run(1, throw_cxx, out);
    CHECK(out[0] == 5 && out[1] == 1, "C++ throw: landing pad %u selector %d (want 5, 1)", out[0], (int)out[1]);
    if (out[2]) {
        CHECK(h_cxa_begin_catch(c, out[2]) == eh_obj, "C++ catch: begin_catch did not give the thrown object");
        h_cxa_end_catch(c);
    }
    /* an Objective-C exception: @catch (NSObject *) at call site 3, selector 2 */
    sjlj_run(3, throw_objc, out);
    CHECK(out[0] == 7 && out[1] == 2, "ObjC throw: landing pad %u selector %d (want 7, 2)", out[0], (int)out[1]);
    if (out[2]) {
        CHECK(h_cxa_begin_catch(c, out[2]) == eh_objc, "ObjC catch: begin_catch did not give the exception");
        h_cxa_end_catch(c);
    }
    /* a C++ exception at call site 3: the Objective-C filter does not take it, catch (...) does (selector 3) */
    sjlj_run(3, throw_cxx, out);
    CHECK(out[0] == 7 && out[1] == 3, "catch (...): landing pad %u selector %d (want 7, 3)", out[0], (int)out[1]);
    if (out[2]) {
        h_cxa_begin_catch(c, out[2]);
        h_cxa_end_catch(c);
    }
    /* through an inner function's cleanup (_Unwind_SjLj_Resume) to the outer handler */
    int before = rt_selftest_cleanups;
    sjlj_run(1, throw_nested, out);
    CHECK(rt_selftest_cleanups == before + 1, "nested throw: the inner cleanup ran %d times",
          rt_selftest_cleanups - before);
    CHECK(out[0] == 5 && out[1] == 1, "nested throw: landing pad %u selector %d (want 5, 1)", out[0], (int)out[1]);
    if (out[2]) {
        h_cxa_begin_catch(c, out[2]);
        h_cxa_end_catch(c);
    }
    CHECK(t->eh_caught == 0 && t->eh_uncaught == 0, "exceptions: the catch stacks were not emptied");
}

int gt_selftest(void)
{
    u32 mark = objc_pool_push();
    dispatch_checks();
    fminmax_checks();
    settings_checks();
    missing_cars_checks();
    pass6_checks();
    cloud_board_checks();
    cheat_checks();
    utf16_checks();
    unwind_checks();
    strings();
    collections();
    sys_checks();
    coregraphics();
    objc_pool_pop(mark);
    rt_log("[selftest] %d of %d checks failed\n", fails, checks);
    return fails != 0;
}
