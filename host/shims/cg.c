/* cg.c -- CoreGraphics and UIGraphics, as far as the game's UIKit overlay code needs them.

   Every caller of these is UIKit overlay code: the IGP "Top Deals" browser window
   (-[FreemiumIGPViewController buttonPressed:], DoRotate:), the splash screen controller, the ad and
   offer libraries (Tapjoy TJCVGUtil, InMobi IMAdCloseButton / IMEmbeddedWebViewController, AdMob
   GADIntrinsicImages), the avatar picker and -[EAGLView saveViewToPhotoLibrary:].  The game itself
   draws with OpenGL ES only.  This port does not draw UIKit views (uikit.c keeps them for layout and
   touches), so what these functions paint would never be seen: the geometry (CGRect,
   CGAffineTransform) is exact, because view layout and hit testing use it, and the objects (contexts,
   images, colour spaces, gradients, data providers) are real, with the state and sizes the callers
   read back, but painting into a context (fills, strokes, images, gradients) leaves its pixels as they
   were.  The game reads no pixels back through CoreGraphics: it imports no CGBitmapContextGetData or
   CGImageGetDataProvider.  One image's pixels are read here: the garage's screenshot (at the end).

   The objects are host Objective-C objects (NSObject subclasses), so CFRetain / CFRelease work on them
   as on iOS, where they are CFTypes.  UIGraphicsGetImageFromCurrentImageContext returns nil: the
   port's UIImage has +imageWithCGImage: only (imageNamed: and friends give nil). */
#include <math.h>
#include <string.h>

#include "rt.h"
#include "abi.h"
#include "ns.h"

/* geometry */

static GtRect standard(GtRect r)
{
    if (r.w < 0)
        r.x += r.w, r.w = -r.w;
    if (r.h < 0)
        r.y += r.h, r.h = -r.h;
    return r;
}
static int is_null(GtRect r) { return isinf(r.x) || isinf(r.y); }
static void put_rect(u32 out, GtRect r) { memcpy(GP(out), &r, sizeof r); }
static void put_aff(u32 out, GtAffine t) { memcpy(GP(out), &t, sizeof t); }

float h_CGRectGetMinX(GtRect r) { return standard(r).x; }
float h_CGRectGetMinY(GtRect r) { return standard(r).y; }
float h_CGRectGetMaxX(GtRect r) { r = standard(r); return r.x + r.w; }
float h_CGRectGetWidth(GtRect r) { return fabsf(r.w); }
float h_CGRectGetHeight(GtRect r) { return fabsf(r.h); }
u32 h_CGRectIsNull(GtRect r) { return is_null(r); }
u32 h_CGRectIsEmpty(GtRect r) { return is_null(r) || r.w == 0 || r.h == 0; }
u32 h_CGRectEqualToRect(GtRect a, GtRect b)
{
    a = standard(a), b = standard(b);
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
u32 h_CGRectContainsPoint(GtRect r, GtPoint p)
{
    if (is_null(r))
        return 0;
    r = standard(r);
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}
void h_CGRectInset(u32 out, GtRect r, float dx, float dy)
{
    if (is_null(r)) {
        put_rect(out, r);
        return;
    }
    r = standard(r);
    r.x += dx, r.y += dy, r.w -= 2 * dx, r.h -= 2 * dy;
    if (r.w < 0 || r.h < 0)
        r = (GtRect){INFINITY, INFINITY, 0, 0};   /* CGRectNull */
    put_rect(out, r);
}
void h_CGRectOffset(u32 out, GtRect r, float dx, float dy)
{
    if (!is_null(r)) {
        r = standard(r);
        r.x += dx, r.y += dy;
    }
    put_rect(out, r);
}
void h_CGRectIntersection(u32 out, GtRect a, GtRect b)
{
    GtRect n = {INFINITY, INFINITY, 0, 0};
    if (is_null(a) || is_null(b)) {
        put_rect(out, n);
        return;
    }
    a = standard(a), b = standard(b);
    float x0 = fmaxf(a.x, b.x), y0 = fmaxf(a.y, b.y);
    float x1 = fminf(a.x + a.w, b.x + b.w), y1 = fminf(a.y + a.h, b.y + b.h);
    put_rect(out, x1 < x0 || y1 < y0 ? n : (GtRect){x0, y0, x1 - x0, y1 - y0});
}

/* t1 then t2 (CGAffineTransformConcat order) */
static GtAffine concat(GtAffine t1, GtAffine t2)
{
    return (GtAffine){t1.a * t2.a + t1.b * t2.c,  t1.a * t2.b + t1.b * t2.d,
                      t1.c * t2.a + t1.d * t2.c,  t1.c * t2.b + t1.d * t2.d,
                      t1.tx * t2.a + t1.ty * t2.c + t2.tx,  t1.tx * t2.b + t1.ty * t2.d + t2.ty};
}
static GtAffine rotation(float a)
{
    float s = sinf(a), c = cosf(a);
    return (GtAffine){c, s, -s, c, 0, 0};
}
void h_CGAffineTransformMakeRotation(u32 out, float a) { put_aff(out, rotation(a)); }
void h_CGAffineTransformMakeScale(u32 out, float sx, float sy) { put_aff(out, (GtAffine){sx, 0, 0, sy, 0, 0}); }
void h_CGAffineTransformMakeTranslation(u32 out, float tx, float ty) { put_aff(out, (GtAffine){1, 0, 0, 1, tx, ty}); }
void h_CGAffineTransformConcat(u32 out, GtAffine t1, GtAffine t2) { put_aff(out, concat(t1, t2)); }
/* Rotate / Scale / Translate apply the new step first, then t */
void h_CGAffineTransformRotate(u32 out, GtAffine t, float a) { put_aff(out, concat(rotation(a), t)); }
void h_CGAffineTransformScale(u32 out, GtAffine t, float sx, float sy)
{
    put_aff(out, concat((GtAffine){sx, 0, 0, sy, 0, 0}, t));
}
void h_CGAffineTransformTranslate(u32 out, GtAffine t, float tx, float ty)
{
    put_aff(out, concat((GtAffine){1, 0, 0, 1, tx, ty}, t));
}

/* objects */

typedef struct { u32 isa; u32 model; } Space;            /* kCGColorSpaceModelMonochrome 0, RGB 1 */
typedef struct { u32 isa; u32 w, h, bpc, bpp, bpr, info, data, size; } Image;   /* data: the provider's */
typedef struct { u32 isa; u32 info, data, size, release; } Provider;
typedef struct { u32 isa; u32 space, count; } Gradient;

#define GSTATES 16
typedef struct {
    GtAffine ctm;
    float fill[4], stroke[4], line;
} GState;
typedef struct {
    u32 isa;
    u32 w, h, bpc, bpr, info, data, space;
    u32 depth;
    GState gs[GSTATES];
} Context;
#define CX(o) ((Context *)GP(o))

static u32 cls_space, cls_image, cls_provider, cls_gradient, cls_context;

static id make(u32 *k, const char *name, u32 bytes)
{
    if (!*k)
        *k = ns_class(name);
    return ns_alloc(*k, bytes);
}

static int is_a(id o, u32 k) { return o && k && rd32(o) == k; }

/* drawing is not rendered (see the top of the file): say so once */
static void not_painted(const char *what)
{
    static int said;
    if (!said) {
        said = 1;
        rt_log("[cg] %s and the other CoreGraphics painting calls leave the pixels as they were: "
               "UIKit views are not drawn in this port\n", what);
    }
}

static id space_new(u32 model)
{
    id o = make(&cls_space, "CGColorSpace", sizeof(Space));
    ((Space *)GP(o))->model = model;
    return o;
}
u32 h_CGColorSpaceCreateDeviceRGB(void) { return space_new(1); }
u32 h_CGColorSpaceCreateDeviceGray(void) { return space_new(0); }
u32 h_CGColorSpaceGetModel(u32 s) { return is_a(s, cls_space) ? ((Space *)GP(s))->model : (u32)-1; }
void h_CGColorSpaceRelease(u32 s) { objc_release(s); }

/* CGColors are UIColors here (-[UIColor CGColor] returns self): {isa, r, g, b, a} */
u32 h_CGColorGetComponents(u32 col) { return col ? col + 4 : 0; }
u32 h_CGColorGetNumberOfComponents(u32 col) { return col ? 4 : 0; }
u32 h_CGColorGetColorSpace(u32 col)
{
    static id rgb;
    (void)col;
    if (!rgb)
        rgb = space_new(1);   /* shared, never released ("Get" rule) */
    return rgb;
}

static id image_new(u32 w, u32 h, u32 bpc, u32 bpp, u32 bpr, u32 info)
{
    id o = make(&cls_image, "CGImage", sizeof(Image));
    Image *x = GP(o);
    x->w = w, x->h = h, x->bpc = bpc, x->bpp = bpp, x->bpr = bpr, x->info = info;
    return o;
}
u32 h_CGImageCreate(u32 w, u32 h, u32 bpc, u32 bpp, u32 bpr, u32 space, u32 info, u32 provider, u32 decode,
                    u32 interpolate, u32 intent)
{
    (void)space, (void)decode, (void)interpolate, (void)intent;
    id o = image_new(w, h, bpc, bpp, bpr, info);
    if (is_a(provider, cls_provider)) {               /* its pixels, while the provider's buffer lives */
        ((Image *)GP(o))->data = ((Provider *)GP(provider))->data;
        ((Image *)GP(o))->size = ((Provider *)GP(provider))->size;
    }
    return o;
}
u32 h_CGImageCreateWithMask(u32 img, u32 mask)
{
    (void)mask;
    if (!is_a(img, cls_image))
        return 0;
    Image *x = GP(img);
    return image_new(x->w, x->h, x->bpc, x->bpp, x->bpr, x->info);
}
u32 h_CGImageGetWidth(u32 img) { return is_a(img, cls_image) ? ((Image *)GP(img))->w : 0; }
u32 h_CGImageGetHeight(u32 img) { return is_a(img, cls_image) ? ((Image *)GP(img))->h : 0; }
void h_CGImageRelease(u32 img) { objc_release(img); }

/* The image does not keep the provider (no pixels are kept), so the provider's release callback,
   which frees the caller's buffer, runs when the caller releases the provider. */
u32 h_CGDataProviderCreateWithData(u32 info, u32 data, u32 size, u32 release)
{
    id o = make(&cls_provider, "CGDataProvider", sizeof(Provider));
    Provider *x = GP(o);
    x->info = info, x->data = data, x->size = size, x->release = release;
    return o;
}
void h_CGDataProviderRelease(u32 p)
{
    if (!is_a(p, cls_provider))
        return;
    Provider *x = GP(p);
    if (x->release) {
        u32 fn = x->release;
        x->release = 0;
        rt_call3(fn, x->info, x->data, x->size);
    }
    objc_release(p);
}

u32 h_CGGradientCreateWithColorComponents(u32 space, u32 components, u32 locations, u32 count)
{
    (void)components, (void)locations;
    id o = make(&cls_gradient, "CGGradient", sizeof(Gradient));
    Gradient *x = GP(o);
    x->space = space, x->count = count;
    return o;
}
void h_CGGradientRelease(u32 g) { objc_release(g); }

/* contexts */

static id context_new(u32 w, u32 h, u32 bpc, u32 bpr, u32 info, u32 data)
{
    id o = make(&cls_context, "CGContext", sizeof(Context));
    Context *x = CX(o);
    x->w = w, x->h = h, x->bpc = bpc, x->bpr = bpr, x->info = info, x->data = data;
    GState *g = &x->gs[0];
    g->ctm = (GtAffine){1, 0, 0, 1, 0, 0};
    g->fill[3] = g->stroke[3] = 1;   /* opaque black */
    g->line = 1;
    return o;
}
static GState *gs(u32 ctx) { return is_a(ctx, cls_context) ? &CX(ctx)->gs[CX(ctx)->depth] : NULL; }

u32 h_CGBitmapContextCreate(u32 data, u32 w, u32 h, u32 bpc, u32 bpr, u32 space, u32 info)
{
    (void)space;
    return context_new(w, h, bpc, bpr, info, data);
}
u32 h_CGBitmapContextCreateImage(u32 ctx)
{
    if (!is_a(ctx, cls_context))
        return 0;
    Context *x = CX(ctx);
    return image_new(x->w, x->h, x->bpc, x->bpc * 4, x->bpr, x->info);
}
void h_CGContextRelease(u32 ctx) { objc_release(ctx); }

void h_CGContextSaveGState(u32 ctx)
{
    if (!is_a(ctx, cls_context))
        return;
    Context *x = CX(ctx);
    if (x->depth + 1 < GSTATES) {
        x->gs[x->depth + 1] = x->gs[x->depth];
        x->depth++;
    }
}
void h_CGContextRestoreGState(u32 ctx)
{
    if (is_a(ctx, cls_context) && CX(ctx)->depth)
        CX(ctx)->depth--;
}
void h_CGContextTranslateCTM(u32 ctx, float tx, float ty)
{
    GState *g = gs(ctx);
    if (g)
        g->ctm = concat((GtAffine){1, 0, 0, 1, tx, ty}, g->ctm);
}
void h_CGContextScaleCTM(u32 ctx, float sx, float sy)
{
    GState *g = gs(ctx);
    if (g)
        g->ctm = concat((GtAffine){sx, 0, 0, sy, 0, 0}, g->ctm);
}
void h_CGContextSetLineWidth(u32 ctx, float w)
{
    GState *g = gs(ctx);
    if (g)
        g->line = w;
}
void h_CGContextSetRGBFillColor(u32 ctx, float r, float g_, float b, float a)
{
    GState *g = gs(ctx);
    if (g)
        g->fill[0] = r, g->fill[1] = g_, g->fill[2] = b, g->fill[3] = a;
}
void h_CGContextSetRGBStrokeColor(u32 ctx, float r, float g_, float b, float a)
{
    GState *g = gs(ctx);
    if (g)
        g->stroke[0] = r, g->stroke[1] = g_, g->stroke[2] = b, g->stroke[3] = a;
}
/* components in the context's fill / stroke colour space: the callers set RGB spaces (4 floats) */
void h_CGContextSetFillColor(u32 ctx, u32 comps)
{
    GState *g = gs(ctx);
    if (g && comps)
        memcpy(g->fill, GP(comps), sizeof g->fill);
}
void h_CGContextSetStrokeColor(u32 ctx, u32 comps)
{
    GState *g = gs(ctx);
    if (g && comps)
        memcpy(g->stroke, GP(comps), sizeof g->stroke);
}
void h_CGContextSetStrokeColorSpace(u32 ctx, u32 space) { (void)ctx, (void)space; }

/* paths and clips: nothing is painted, so they are not kept */
void h_CGContextBeginPath(u32 ctx) { (void)ctx; }
void h_CGContextClosePath(u32 ctx) { (void)ctx; }
void h_CGContextMoveToPoint(u32 ctx, float x, float y) { (void)ctx, (void)x, (void)y; }
void h_CGContextAddLineToPoint(u32 ctx, float x, float y) { (void)ctx, (void)x, (void)y; }
void h_CGContextAddRect(u32 ctx, GtRect r) { (void)ctx, (void)r; }
void h_CGContextAddEllipseInRect(u32 ctx, GtRect r) { (void)ctx, (void)r; }
void h_CGContextAddArc(u32 ctx, float x, float y, float rad, float a0, float a1, u32 cw)
{
    (void)ctx, (void)x, (void)y, (void)rad, (void)a0, (void)a1, (void)cw;
}
void h_CGContextAddArcToPoint(u32 ctx, float x1, float y1, float x2, float y2, float rad)
{
    (void)ctx, (void)x1, (void)y1, (void)x2, (void)y2, (void)rad;
}
void h_CGContextClip(u32 ctx) { (void)ctx; }
void h_CGContextClipToRect(u32 ctx, GtRect r) { (void)ctx, (void)r; }

void h_CGContextFillRect(u32 ctx, GtRect r) { (void)ctx, (void)r; not_painted("CGContextFillRect"); }
void h_CGContextFillPath(u32 ctx) { (void)ctx; not_painted("CGContextFillPath"); }
void h_CGContextFillEllipseInRect(u32 ctx, GtRect r) { (void)ctx, (void)r; not_painted("CGContextFillEllipseInRect"); }
void h_CGContextStrokePath(u32 ctx) { (void)ctx; not_painted("CGContextStrokePath"); }
void h_CGContextStrokeLineSegments(u32 ctx, u32 pts, u32 n) { (void)ctx, (void)pts, (void)n; not_painted("CGContextStrokeLineSegments"); }
void h_CGContextDrawImage(u32 ctx, GtRect r, u32 img) { (void)ctx, (void)r, (void)img; not_painted("CGContextDrawImage"); }
void h_CGContextDrawLinearGradient(u32 ctx, u32 grad, GtPoint p0, GtPoint p1, u32 opts)
{
    (void)ctx, (void)grad, (void)p0, (void)p1, (void)opts;
    not_painted("CGContextDrawLinearGradient");
}

/* UIGraphics: a stack of image contexts (UIKit drawing happens on the main thread) */

#define UIG_DEPTH 16
static id uig_stack[UIG_DEPTH];
static int uig_n;
static id uig_view_ctx;   /* UIGraphicsGetCurrentContext outside an image context (in a drawRect:) */

void h_UIGraphicsBeginImageContext(GtSize s)
{
    u32 w = s.w > 0 ? (u32)ceilf(s.w) : 0, h = s.h > 0 ? (u32)ceilf(s.h) : 0;
    id ctx = context_new(w, h, 8, w * 4, 1 /* kCGImageAlphaPremultipliedLast */, 0);
    /* UIKit's image contexts have the origin at the top left: flip like UIKit does */
    GState *g = gs(ctx);
    g->ctm = (GtAffine){1, 0, 0, -1, 0, (float)h};
    if (uig_n < UIG_DEPTH)
        uig_stack[uig_n++] = ctx;
    else
        objc_release(ctx);
}
u32 h_UIGraphicsGetCurrentContext(void)
{
    if (uig_n)
        return uig_stack[uig_n - 1];
    /* a drawRect: of a view: UIKit would have pushed the view's context; this port never calls
       drawRect: itself, but a game method may (IMAdCloseButton drawRect:), so give it a context */
    if (!uig_view_ctx)
        uig_view_ctx = context_new(0, 0, 8, 0, 1, 0);
    return uig_view_ctx;
}
u32 h_UIGraphicsGetImageFromCurrentImageContext(void) { return 0; }   /* no UIImage in this port */
void h_UIGraphicsEndImageContext(void)
{
    if (uig_n)
        objc_release(uig_stack[--uig_n]);
}

/* the garage's Screenshot button
   Car Collection > View > Screenshot runs -[EAGLView saveViewToPhotoLibrary:] (0x309d68): glReadPixels of
   the drawable, its rows turned top down, CGImageCreate (RGBA, 8 bits), +[UIImage imageWithCGImage:],
   UIImageWriteToSavedPhotosAlbum(image, nil, nil, nil): the iPad's Photos.  The port had neither, and the
   game stopped (port fault: pressing the Screenshot button crashed the game).
   Here a UIImage is {isa, CGImage}, and the album is a folder: Pictures\GT Racing (GT_PHOTO_DIR for
   tests), a PNG per shot through GDI+, turned to the landscape the player sees (the drawable is
   portrait: its top row is the picture's left column, as eagl.c's present has it). */
#include "plat.h"
#ifdef _WIN32
#include <shlobj.h>
#else
#include <SDL3/SDL.h>
#include <sys/stat.h>
#endif
#include <stdio.h>
#include <stdlib.h>

typedef struct { u32 isa; u32 cgimage; } UiImage;

static void uiimage_withCGImage(CPU *c)
{
    id o = ns_alloc(ns_class("UIImage"), sizeof(UiImage));
    ((UiImage *)GP(o))->cgimage = objc_retain(ARG(0));
    ret_i(c, objc_autorelease(o));
}
static void uiimage_CGImage(CPU *c) { ret_i(c, ((UiImage *)GP(SELF))->cgimage); }
static void uiimage_dealloc(CPU *c)
{
    objc_release(((UiImage *)GP(SELF))->cgimage);
    objc_free_instance(SELF);
}

#ifdef _WIN32
typedef struct { UINT32 version; void *debug; BOOL no_thread, no_codecs; } GpStartup;
int __stdcall GdiplusStartup(ULONG_PTR *token, const GpStartup *in, void *out);
int __stdcall GdipCreateBitmapFromScan0(int w, int h, int stride, int format, BYTE *scan0, void **bitmap);
int __stdcall GdipSaveImageToFile(void *image, const WCHAR *file, const CLSID *encoder, const void *params);
int __stdcall GdipDisposeImage(void *image);

static int save_png(const WCHAR *path, u8 *bgrx, int w, int h)
{
    static ULONG_PTR token;
    static const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    if (!token) {
        GpStartup in = {1, NULL, FALSE, FALSE};
        if (GdiplusStartup(&token, &in, NULL))
            return 0;
    }
    void *bmp = NULL;
    int ok = !GdipCreateBitmapFromScan0(w, h, w * 4, 0x22009 /* PixelFormat32bppRGB */, bgrx, &bmp) && bmp &&
             !GdipSaveImageToFile(bmp, path, &png, NULL);
    if (bmp)
        GdipDisposeImage(bmp);
    return ok;
}
#else
/* Android: Pictures/GT Racing on the shared storage (with All files access), else Screenshots in the port's folder */
static int save_png_android(const u8 *bgrx, int w, int h, char *name, size_t nn)
{
    char dir[MAX_PATH], path[MAX_PATH + 64];
    const char *env = getenv("GT_PHOTO_DIR"), *ext = getenv("EXTERNAL_STORAGE");
    if (env && *env)
        snprintf(dir, sizeof dir, "%s", env);
    else {
        snprintf(dir, sizeof dir, "%s/Pictures", ext && *ext ? ext : "/storage/emulated/0");
        mkdir(dir, 0777);
        strncat(dir, "/GT Racing", sizeof dir - strlen(dir) - 1);
        mkdir(dir, 0777);
        if (access(dir, W_OK)) {
            char exe[MAX_PATH];
            GetModuleFileNameA(NULL, exe, sizeof exe);
            char *e = strrchr(exe, '/');
            if (e)
                *e = 0;
            snprintf(dir, sizeof dir, "%s/Screenshots", exe);
        }
    }
    mkdir(dir, 0777);
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    snprintf(name, nn, "GTRacing_%04d%02d%02d_%02d%02d%02d.png", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    SDL_Surface *sf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, (void *)bgrx, w * 4);
    int ok = sf && SDL_SavePNG(sf, path);
    SDL_DestroySurface(sf);
    return ok;
}
#endif

void h_UIImageWriteToSavedPhotosAlbum(u32 image, u32 target, u32 sel, u32 ctx)
{
    (void)target, (void)sel, (void)ctx;                 /* the game asks for no answer (nil, nil, nil) */
    u32 cg = image ? ((UiImage *)GP(image))->cgimage : 0;
    Image *x = is_a(cg, cls_image) ? GP(cg) : NULL;
    if (!x || !x->data || x->bpp != 32 || x->bpc != 8 || !x->w || !x->h || x->bpr < x->w * 4 || (unsigned long long)x->bpr * x->h > x->size) {
        rt_log("[cg] UIImageWriteToSavedPhotosAlbum: no pixels to save\n");
        return;
    }
    const u8 *src = GP(x->data);
    int portrait = x->h > x->w, w = portrait ? (int)x->h : (int)x->w, h = portrait ? (int)x->w : (int)x->h;
    u8 *out = malloc((size_t)w * h * 4);
    if (!out)
        return;
    for (int y = 0; y < h; y++)
        for (int k = 0; k < w; k++) {
            /* landscape (k, y) <- the image's row k, column (width - 1 - y) when portrait */
            const u8 *p = portrait ? src + (size_t)k * x->bpr + (size_t)(x->w - 1 - y) * 4 : src + (size_t)y * x->bpr + (size_t)k * 4;
            u8 *q = out + ((size_t)y * w + k) * 4;
            q[0] = p[2], q[1] = p[1], q[2] = p[0], q[3] = 255;
        }
#ifndef _WIN32
    char name[96];
    int ok_a = save_png_android(out, w, h, name, sizeof name);
    free(out);
    rt_log("[cg] screenshot %d x %d %s %s\n", w, h, ok_a ? "saved:" : "NOT saved:", name);
    return;
#else
    WCHAR dir[MAX_PATH], path[MAX_PATH + 64];
    const char *env = getenv("GT_PHOTO_DIR");
    if (env && *env)
        MultiByteToWideChar(CP_UTF8, 0, env, -1, dir, MAX_PATH);
    else {
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES | CSIDL_FLAG_CREATE, NULL, 0, dir)))
            GetCurrentDirectoryW(MAX_PATH, dir);
        wcscat(dir, L"\\GT Racing");
    }
    CreateDirectoryW(dir, NULL);
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf(path, sizeof path / sizeof *path, L"%ls\\GTRacing_%04u%02u%02u_%02u%02u%02u_%03u.png", dir, t.wYear,
             t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    int ok = save_png(path, out, w, h);
    free(out);
    /* only the file's own name in the log (privacy): the Pictures folder can be anywhere, also outside
       the user's folder that the log hides (a second drive, a folder named after the Windows user) */
    char u8path[MAX_PATH * 3 + 64];
    const WCHAR *file = wcsrchr(path, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, file ? file + 1 : path, -1, u8path, sizeof u8path, NULL, NULL);
    rt_log("[cg] screenshot %d x %d %s %s\\%s\n", w, h, ok ? "saved to" : "NOT saved to",
           env && *env ? "GT_PHOTO_DIR" : "Pictures\\GT Racing", u8path);
#endif
}

void cg_init(void)
{
    static const NsMethod image[] = {
        {"imageWithCGImage:", uiimage_withCGImage, 1}, {"CGImage", uiimage_CGImage, 0}, {"dealloc", uiimage_dealloc, 0},
        {NULL, NULL, 0}};
    ns_methods("UIImage", image);
}
