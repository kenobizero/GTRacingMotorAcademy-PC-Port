/* cf.c -- the rest of CoreFoundation the game imports, on the host's Foundation objects (CF types are
   toll-free bridged on iOS, so a CFBundleRef is an NSBundle, a CFURLRef an NSURL, and so on):
   CFBundle, CFURL, CFUUID, CFReadStream (files), CFBitVector, CFNotificationCenter (Darwin
   notifications: nobody posts any). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "ns.h"

/* CFBundle */

u32 h_CFBundleGetMainBundle(void) { return msg0(ns_class("NSBundle"), "mainBundle"); }
u32 h_CFBundleGetIdentifier(u32 b) { return msg0(b, "bundleIdentifier"); }
u32 h_CFBundleGetInfoDictionary(u32 b) { return msg0(b, "infoDictionary"); }
u32 h_CFBundleGetValueForInfoDictionaryKey(u32 b, u32 key) { return msg1(b, "objectForInfoDictionaryKey:", key); }
u32 h_CFBundleCopyBundleURL(u32 b)
{
    return objc_retain(msg1(ns_class("NSURL"), "fileURLWithPath:", msg0(b, "bundlePath")));
}
/* NumVersion: major (BCD), minor and bug nibbles, stage (0x80 final), non-release */
u32 h_CFBundleGetVersionNumber(u32 b)
{
    const char *v = ns_utf8(msg1(b, "objectForInfoDictionaryKey:", ns_string("CFBundleVersion")));
    unsigned ma = 0, mi = 0, bug = 0;
    sscanf(v, "%u.%u.%u", &ma, &mi, &bug);
    return ((ma / 10 % 10) << 28 | (ma % 10) << 24) | (mi & 15) << 20 | (bug & 15) << 16 | 0x8000;
}

/* CFURL */

u32 h_CFURLCopyFileSystemPath(u32 url, u32 style)
{
    (void)style;
    return objc_retain(msg0(url, "path"));
}
u32 h_CFURLCreateWithFileSystemPath(u32 alloc, u32 path, u32 style, u32 is_dir)
{
    (void)alloc, (void)style, (void)is_dir;
    return objc_retain(msg1(ns_class("NSURL"), "fileURLWithPath:", path));
}
/* percent-escapes everything but unreserved characters and `leave`, and also `legal` */
u32 h_CFURLCreateStringByAddingPercentEscapes(u32 alloc, u32 str, u32 leave, u32 legal, u32 enc)
{
    (void)alloc, (void)enc;
    const char *s = ns_utf8(str), *keep = leave ? ns_utf8(leave) : "", *esc = legal ? ns_utf8(legal) : "";
    Buf b = {0};
    for (const u8 *p = (const u8 *)s; *p; p++) {
        int unres = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                    strchr("-_.~!*'();:@&=+$,/?#[]", *p);
        if ((unres && !strchr(esc, *p)) || (*p < 0x80 && strchr(keep, *p) && *p))
            buf_put(&b, p, 1);
        else
            buf_printf(&b, "%%%02X", *p);
    }
    id r = ns_string_new(b.p ? b.p : "", b.n);
    buf_free(&b);
    return r;
}
u32 h_CFURLCreateStringByReplacingPercentEscapesUsingEncoding(u32 alloc, u32 str, u32 leave, u32 enc)
{
    (void)alloc, (void)leave, (void)enc;
    return objc_retain(msg1(str, "stringByReplacingPercentEscapesUsingEncoding:", 4));
}

/* CFUUID */

typedef struct { u32 isa; u8 b[16]; } Uuid;
u32 h_CFUUIDCreate(u32 alloc)
{
    (void)alloc;
    static u64 seed;
    if (!seed)
        seed = (u64)GetTickCount64() * 0x9E3779B97F4A7C15ull ^ (u64)(uintptr_t)&seed;
    id o = ns_alloc(ns_class("__NSConcreteUUID"), sizeof(Uuid));
    Uuid *u = GP(o);
    for (int i = 0; i < 16; i += 8) {
        u64 z = (seed += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        memcpy(u->b + i, &z, 8);
    }
    u->b[6] = (u8)((u->b[6] & 0x0f) | 0x40);
    u->b[8] = (u8)((u->b[8] & 0x3f) | 0x80);
    return o;
}
u32 h_CFUUIDCreateString(u32 alloc, u32 uuid)
{
    (void)alloc;
    const u8 *b = ((Uuid *)GP(uuid))->b;
    char s[40];
    snprintf(s, sizeof s, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2],
             b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return ns_string_new(s, 36);
}

/* CFReadStream on files */

typedef struct { u32 isa; FILE *f; u32 path; int status; } Stream;
u32 h_CFReadStreamCreateWithFile(u32 alloc, u32 url)
{
    (void)alloc;
    id o = ns_alloc(ns_class("__NSCFInputStream"), sizeof(Stream));
    ((Stream *)GP(o))->path = objc_retain(msg0(url, "path"));
    return o;
}
u32 h_CFReadStreamOpen(u32 s)
{
    Stream *x = GP(s);
    char host[1024];
    x->f = fopen(gt_host_path(ns_utf8(x->path), host, sizeof host), "rb");
    x->status = x->f ? 2 : 7;     /* kCFStreamStatusOpen / Error */
    return x->f != NULL;
}
u32 h_CFReadStreamRead(u32 s, u32 buf, u32 len)
{
    Stream *x = GP(s);
    if (!x->f)
        return (u32)-1;
    return (u32)fread(GP(buf), 1, len, x->f);
}
void h_CFReadStreamClose(u32 s)
{
    Stream *x = GP(s);
    if (x->f)
        fclose(x->f);
    x->f = NULL;
    x->status = 6;                /* kCFStreamStatusClosed */
}

/* CFBitVector: bit 0 is the most significant bit of the first byte */

typedef struct { u32 isa; u32 bits; u8 *p; } Bits;
static id bits_new(const u8 *src, u32 nbits, u32 cap)
{
    id o = ns_alloc(ns_class("__NSCFBitVector"), sizeof(Bits));
    Bits *b = GP(o);
    u32 n = (cap > nbits ? cap : nbits) / 8 + 1;
    b->p = calloc(1, n);
    b->bits = nbits;
    if (src)
        memcpy(b->p, src, (nbits + 7) / 8);
    return o;
}
u32 h_CFBitVectorCreate(u32 alloc, u32 bytes, u32 nbits) { (void)alloc; return bits_new(GP(bytes), nbits, nbits); }
u32 h_CFBitVectorCreateMutableCopy(u32 alloc, u32 cap, u32 bv)
{
    (void)alloc;
    Bits *s = GP(bv);
    return bits_new(s->p, s->bits, cap);
}
u32 h_CFBitVectorGetBitAtIndex(u32 bv, u32 i)
{
    Bits *b = GP(bv);
    return i < b->bits ? (b->p[i / 8] >> (7 - i % 8)) & 1 : 0;
}
void h_CFBitVectorSetBitAtIndex(u32 bv, u32 i, u32 v)
{
    Bits *b = GP(bv);
    if (i >= b->bits) {
        u32 n = i / 8 + 1;
        b->p = realloc(b->p, n + 1);
        memset(b->p + (b->bits + 7) / 8, 0, n + 1 - (b->bits + 7) / 8);
        b->bits = i + 1;
    }
    if (v)
        b->p[i / 8] |= (u8)(0x80 >> i % 8);
    else
        b->p[i / 8] &= (u8)~(0x80 >> i % 8);
}

/* CFNotificationCenter (Darwin / local): nobody posts */

static u32 center_cell = 0xCF00;
u32 h_CFNotificationCenterGetLocalCenter(void) { return GA(&center_cell); }
void h_CFNotificationCenterAddObserver(u32 c, u32 obs, u32 cb, u32 name, u32 obj, u32 how)
{
    (void)c, (void)obs, (void)cb, (void)obj, (void)how;
    rt_log("[cf] notification observer for %s (never posted)\n", name ? ns_utf8(name) : "(all)");
}
