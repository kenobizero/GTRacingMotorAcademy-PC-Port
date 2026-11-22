/* uikit.c -- the host's UIKit (stage 4): UIApplicationMain and its run loop, the screen and device,
   views, windows, view controllers, layers, UITouch / UIEvent and UIAccelerometer (fed by pcinput.c, the PC's input).

   Nothing UIKit draws itself is shown: the game's picture is its OpenGL ES framebuffer (eagl.c).
   Views are still kept with their geometry (bounds, center, transform, superview), because the
   game reads them back, rotates them for landscape, and touches are found and converted by it.

   Host state is inside the objects (objc_host_size): game subclasses' ivars come after it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "ui.h"
#include "uikit_input.h"
#include "game_hooks.h"
#include "net.h"

SDL_Window *ui_window;
static float ui_scale = 1.0f;
float ui_screen_h = 1024.0f;
int ui_quit;
float ui_tilt;
volatile int ui_app_background;    /* Android: the app is in the background (pcinput.c) */
int ui_sensor_ok;                  /* Android: ui_sensor_g holds the device's gravity (pcinput.c) */
double ui_sensor_g[3];

static DWORD main_tid;
int ui_is_main_thread(void) { return GetCurrentThreadId() == main_tid; }

/* small helpers */

static u32 cls(const char *name) { return ns_class(name); }
static id singleton(id *slot, const char *name, u32 bytes)
{
    if (!*slot)
        *slot = ns_alloc(cls(name), bytes);
    return *slot;
}
static void call_block(u32 block, int n, u32 a)
{
    if (!block)
        return;
    u32 args[2] = {block, a};
    rt_guest_call(rt_cpu(), rd32(block + 12), n + 1, args);
}
static void put_rect(u32 at, GtRect r) { memcpy(GP(at), &r, sizeof r); }
static void put_point(u32 at, GtPoint p) { memcpy(GP(at), &p, sizeof p); }
static void put_size(u32 at, GtSize s) { memcpy(GP(at), &s, sizeof s); }
static GtRect arg_rect(CPU *c, int i) { return (GtRect){arg_f(c, i), arg_f(c, i + 1), arg_f(c, i + 2), arg_f(c, i + 3)}; }
static GtPoint arg_point(CPU *c, int i) { return (GtPoint){arg_f(c, i), arg_f(c, i + 1)}; }

static const GtAffine IDENT = {1, 0, 0, 1, 0, 0};
static GtAffine aff_mul(GtAffine a, GtAffine b)   /* a then b */
{
    return (GtAffine){a.a * b.a + a.b * b.c, a.a * b.b + a.b * b.d, a.c * b.a + a.d * b.c, a.c * b.b + a.d * b.d,
                      a.tx * b.a + a.ty * b.c + b.tx, a.tx * b.b + a.ty * b.d + b.ty};
}
static GtAffine aff_inv(GtAffine m)
{
    float det = m.a * m.d - m.b * m.c;
    if (det == 0)
        return IDENT;
    GtAffine r = {m.d / det, -m.b / det, -m.c / det, m.a / det, 0, 0};
    r.tx = -(m.tx * r.a + m.ty * r.c);
    r.ty = -(m.tx * r.b + m.ty * r.d);
    return r;
}
static GtPoint aff_apply(GtAffine m, GtPoint p) { return (GtPoint){m.a * p.x + m.c * p.y + m.tx, m.b * p.x + m.d * p.y + m.ty}; }
static GtRect aff_rect(GtAffine m, GtRect r)
{
    GtPoint q[4] = {aff_apply(m, (GtPoint){r.x, r.y}), aff_apply(m, (GtPoint){r.x + r.w, r.y}),
                    aff_apply(m, (GtPoint){r.x, r.y + r.h}), aff_apply(m, (GtPoint){r.x + r.w, r.y + r.h})};
    float x0 = q[0].x, x1 = q[0].x, y0 = q[0].y, y1 = q[0].y;
    for (int i = 1; i < 4; i++) {
        x0 = fminf(x0, q[i].x), x1 = fmaxf(x1, q[i].x);
        y0 = fminf(y0, q[i].y), y1 = fmaxf(y1, q[i].y);
    }
    return (GtRect){x0, y0, x1 - x0, y1 - y0};
}

/* objects */

typedef struct {
    u32 isa;
    u32 layer;                 /* CALayer, +1 */
    u32 superview, window;     /* not retained */
    u32 subviews;              /* NSMutableArray +1, or 0 */
    u32 controller;            /* the view controller whose view this is (next responder) */
    u32 bg;                    /* UIColor +1 */
    u32 tag, autoresize, content_mode, gestures;
    GtRect bounds;
    GtPoint center;
    GtAffine transform;
    float alpha, scale;
    u8 hidden, interaction, multitouch, clips, opaque, exclusive, is_window, pad;
} View;
typedef struct { View v; u32 root; float level; u32 key; } Window;
#define VW(o) ((View *)GP(o))
#define WN(o) ((Window *)GP(o))

typedef struct { u32 isa, view, presented, presenting, parent, title, orientation, nib; } Controller;
#define VC(o) ((Controller *)GP(o))

typedef struct { u32 isa, delegate, view; GtRect bounds; GtPoint position, anchor; float scale, opacity;
                 u32 props, sublayers; u8 opaque, hidden, pad[2]; } Layer;
#define LY(o) ((Layer *)GP(o))

typedef struct { u32 isa, delegate, windows, key_window, idle_off, bar_hidden, bar_orientation, badge, state; } App;
#define AP(o) ((App *)GP(o))

typedef struct { u32 isa, pad; double t; u32 phase, taps; GtPoint loc, prev; u32 view, window, finger; } Touch;
#define TC(o) ((Touch *)GP(o))
typedef struct { u32 isa, touches; double t; } Event;

typedef struct { u32 isa; float r, g, b, a; } Color;
typedef struct { u32 isa; float size; u32 name; } Font;

static id app_obj, screen_obj, device_obj, accel_obj;
static u32 cls_view, cls_window, cls_layer, cls_touch, cls_event, cls_color, cls_app;

/* geometry */

/* the view's coordinates -> its superview's (or the screen's for a window) */
static GtAffine to_super(id v)
{
    View *x = VW(v);
    GtAffine m = {1, 0, 0, 1, -(x->bounds.x + x->bounds.w / 2), -(x->bounds.y + x->bounds.h / 2)};
    m = aff_mul(m, x->transform);
    m.tx += x->center.x;
    m.ty += x->center.y;
    return m;
}
static GtAffine to_screen(id v)
{
    GtAffine m = IDENT;
    for (; v; v = VW(v)->superview)
        m = aff_mul(m, to_super(v));
    return m;
}
void ui_view_to_screen(id view, GtAffine *m) { *m = to_screen(view); }

static GtRect view_frame(id v)
{
    View *x = VW(v);
    GtRect b = {0, 0, x->bounds.w, x->bounds.h};
    GtAffine m = {1, 0, 0, 1, -b.w / 2, -b.h / 2};
    m = aff_mul(m, x->transform);
    m.tx += x->center.x;
    m.ty += x->center.y;
    return aff_rect(m, b);
}
static void view_set_frame(id v, GtRect f)
{
    View *x = VW(v);
    GtAffine t = x->transform;
    float w = f.w, h = f.h;
    if (fabsf(t.a) < 0.5f && fabsf(t.b) > 0.5f) {   /* turned by 90 degrees: the frame's sides swap */
        w = f.h;
        h = f.w;
    }
    x->bounds.w = w;
    x->bounds.h = h;
    x->center = (GtPoint){f.x + f.w / 2, f.y + f.h / 2};
}
static GtPoint convert(GtPoint p, id from, id to)
{
    GtAffine m = from ? to_screen(from) : IDENT;
    if (to)
        m = aff_mul(m, aff_inv(to_screen(to)));
    return aff_apply(m, p);
}
static GtRect convert_rect(GtRect r, id from, id to)
{
    GtAffine m = from ? to_screen(from) : IDENT;
    if (to)
        m = aff_mul(m, aff_inv(to_screen(to)));
    return aff_rect(m, r);
}

static id view_window(id v)
{
    for (; v; v = VW(v)->superview)
        if (VW(v)->is_window)
            return v;
    return 0;
}

/* UIResponder */

static void r_next(CPU *c) { ret_i(c, 0); }
static void r_yes(CPU *c) { ret_i(c, 1); }
static void r_no(CPU *c) { ret_i(c, 0); }
static void r_nop(CPU *c) { (void)c; }
static void r_forward(CPU *c)
{
    /* touches...:withEvent: the view did not handle: up the responder chain */
    u32 self = SELF, sel = c->r[1], a = ARG(0), b = ARG(1);
    id next = msg0(self, "nextResponder");
    if (next)
        msg2(next, sel_name(sel), a, b);
}

/* CALayer / CAEAGLLayer */

static void ly_init(CPU *c)
{
    Layer *l = LY(SELF);
    l->opacity = 1;
    l->scale = 1;
    l->anchor = (GtPoint){0.5f, 0.5f};
}
static void ly_layer(CPU *c) { ret_i(c, objc_autorelease(msg0(ns_alloc(SELF, sizeof(Layer)), "init"))); }
id ui_layer_view(id layer) { return layer ? LY(layer)->view : 0; }
void ui_layer_size(id layer, float *w, float *h, float *scale)
{
    Layer *l = LY(layer);
    if (l->view) {
        *w = VW(l->view)->bounds.w;
        *h = VW(l->view)->bounds.h;
        *scale = VW(l->view)->scale > 0 ? VW(l->view)->scale : 1;
    } else {
        *w = l->bounds.w;
        *h = l->bounds.h;
        *scale = l->scale > 0 ? l->scale : 1;
    }
}
static void ly_bounds(CPU *c)
{
    Layer *l = LY(ST_SELF);
    put_rect(ST_RET, l->view ? VW(l->view)->bounds : l->bounds);
}
static void ly_setBounds(CPU *c)
{
    Layer *l = LY(SELF);
    if (l->view)
        VW(l->view)->bounds = arg_rect(c, 2);
    else
        l->bounds = arg_rect(c, 2);
}
static void ly_frame(CPU *c)
{
    Layer *l = LY(ST_SELF);
    put_rect(ST_RET, l->view ? view_frame(l->view) : (GtRect){l->position.x - l->bounds.w * l->anchor.x,
                                                               l->position.y - l->bounds.h * l->anchor.y, l->bounds.w,
                                                               l->bounds.h});
}
static void ly_setFrame(CPU *c)
{
    Layer *l = LY(SELF);
    GtRect f = arg_rect(c, 2);
    if (l->view) {
        view_set_frame(l->view, f);
        return;
    }
    l->bounds = (GtRect){0, 0, f.w, f.h};
    l->position = (GtPoint){f.x + f.w * l->anchor.x, f.y + f.h * l->anchor.y};
}
static void ly_position(CPU *c)
{
    Layer *l = LY(ST_SELF);
    put_point(ST_RET, l->view ? VW(l->view)->center : l->position);
}
static void ly_setPosition(CPU *c)
{
    Layer *l = LY(SELF);
    if (l->view)
        VW(l->view)->center = arg_point(c, 2);
    else
        l->position = arg_point(c, 2);
}
static void ly_setOpaque(CPU *c) { LY(SELF)->opaque = (u8)ARG(0); }
static void ly_isOpaque(CPU *c) { ret_i(c, LY(SELF)->opaque); }
static void ly_setHidden(CPU *c) { LY(SELF)->hidden = (u8)ARG(0); }
static void ly_setOpacity(CPU *c) { LY(SELF)->opacity = ARGF(0); }
static void ly_opacity(CPU *c) { ret_f(c, LY(SELF)->opacity); }
static void ly_setScale(CPU *c) { LY(SELF)->scale = ARGF(0); if (LY(SELF)->view) VW(LY(SELF)->view)->scale = ARGF(0); }
static void ly_scale(CPU *c) { ret_f(c, LY(SELF)->view ? VW(LY(SELF)->view)->scale : LY(SELF)->scale); }
static void ly_setProps(CPU *c)
{
    Layer *l = LY(SELF);
    id old = l->props;
    l->props = objc_retain(msg0(ARG(0), "copy"));
    objc_release(old);
    rt_log("[ui] drawable properties %s\n", ns_describe(l->props));
}
static void ly_props(CPU *c) { ret_i(c, LY(SELF)->props); }
static void ly_delegate(CPU *c) { ret_i(c, LY(SELF)->delegate); }
static void ly_setDelegate(CPU *c) { LY(SELF)->delegate = ARG(0); }
static void ly_sublayers(CPU *c) { ret_i(c, LY(SELF)->sublayers); }
static void ly_addSublayer(CPU *c)
{
    Layer *l = LY(SELF);
    if (!l->sublayers)
        l->sublayers = ns_array_new(NULL, 0);
    ns_array_add(l->sublayers, ARG(0));
}
static void ly_dealloc(CPU *c)
{
    objc_release(LY(SELF)->props);
    objc_release(LY(SELF)->sublayers);
    objc_free_instance(SELF);
}
static void ly_anchor(CPU *c) { put_point(ST_RET, LY(ST_SELF)->anchor); }
static void ly_setAnchor(CPU *c) { LY(SELF)->anchor = arg_point(c, 2); }
static void ly_transform3d(CPU *c)
{
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(GP(ST_RET), m, sizeof m);
}

/* UIView */

static void view_setup(id v, GtRect frame)
{
    View *x = VW(v);
    x->transform = IDENT;
    x->alpha = 1;
    x->scale = 1;
    x->interaction = 1;
    x->opaque = 1;
    x->bounds = (GtRect){0, 0, frame.w, frame.h};
    x->center = (GtPoint){frame.x + frame.w / 2, frame.y + frame.h / 2};
    u32 lc = msg0(rd32(v), "layerClass");
    if (!lc)
        lc = cls_layer;
    id l = msg0(ns_alloc(lc, sizeof(Layer)), "init");
    LY(l)->delegate = v;
    LY(l)->view = v;
    x->layer = l;
}
static void v_layerClass(CPU *c) { ret_i(c, cls_layer); }
static void v_init(CPU *c)
{
    u32 w[4] = {0, 0, 0, 0};
    ret_i(c, objc_msg(SELF, "initWithFrame:", 4, w));
}
static void v_initWithFrame(CPU *c)
{
    id v = SELF;
    view_setup(v, arg_rect(c, 2));
    ret_i(c, v);
}
static void v_initWithCoder(CPU *c)
{
    view_setup(SELF, (GtRect){0, 0, UI_SCREEN_W, UI_SCREEN_H});
    ret_i(c, SELF);
}
static void remove_from_super(id v)
{
    View *x = VW(v);
    id s = x->superview;
    if (!s)
        return;
    id subs = VW(s)->subviews;
    objc_retain(v);
    x->superview = 0;
    x->window = 0;
    if (subs)
        msg1(subs, "removeObjectIdenticalTo:", v);
    objc_release(v);
}
static void v_dealloc(CPU *c)
{
    id v = SELF;
    View *x = VW(v);
    if (x->subviews) {
        for (u32 i = 0, n = ns_array_count(x->subviews); i < n; i++) {
            id s = ns_array_at(x->subviews, i);
            VW(s)->superview = 0;
            VW(s)->window = 0;
        }
        objc_release(x->subviews);
    }
    if (x->layer) {
        LY(x->layer)->view = 0;
        LY(x->layer)->delegate = 0;
        objc_release(x->layer);
    }
    objc_release(x->bg);
    objc_release(x->gestures);
    objc_free_instance(v);
}
static void v_frame(CPU *c) { put_rect(ST_RET, view_frame(ST_SELF)); }
static void v_setFrame(CPU *c) { view_set_frame(SELF, arg_rect(c, 2)); }
static void v_bounds(CPU *c) { put_rect(ST_RET, VW(ST_SELF)->bounds); }
static void v_setBounds(CPU *c) { VW(SELF)->bounds = arg_rect(c, 2); }
static void v_center(CPU *c) { put_point(ST_RET, VW(ST_SELF)->center); }
static void v_setCenter(CPU *c) { VW(SELF)->center = arg_point(c, 2); }
static void v_transform(CPU *c) { memcpy(GP(ST_RET), &VW(ST_SELF)->transform, sizeof(GtAffine)); }
static void v_setTransform(CPU *c)
{
    GtAffine t = {ARGF(0), ARGF(1), ARGF(2), ARGF(3), ARGF(4), ARGF(5)};
    VW(SELF)->transform = t;
    rt_log("[ui] -[%s setTransform:] %.3f %.3f %.3f %.3f %.1f %.1f\n", objc_class_name(rd32(SELF)), t.a, t.b, t.c, t.d,
           t.tx, t.ty);
}
static void v_layer(CPU *c) { ret_i(c, VW(SELF)->layer); }
static void v_superview(CPU *c) { ret_i(c, VW(SELF)->superview); }
static void v_window(CPU *c) { ret_i(c, view_window(SELF)); }
static void v_subviews(CPU *c)
{
    id s = VW(SELF)->subviews;
    ret_i(c, s ? objc_autorelease(msg0(s, "copy")) : objc_autorelease(ns_array_new(NULL, 0)));
}
static void v_nextResponder(CPU *c)
{
    View *x = VW(SELF);
    ret_i(c, x->controller ? x->controller : x->superview ? x->superview : x->is_window ? app_obj : 0);
}
static void insert_sub(id v, id sub, int at)
{
    if (!sub || sub == v)
        return;
    objc_retain(sub);
    if (VW(sub)->superview)
        remove_from_super(sub);
    msg1(sub, "willMoveToSuperview:", v);
    View *x = VW(v);
    if (!x->subviews)
        x->subviews = ns_array_new(NULL, 0);
    u32 n = ns_array_count(x->subviews);
    if (at < 0 || (u32)at > n)
        at = (int)n;
    msg2(x->subviews, "insertObject:atIndex:", sub, (u32)at);
    VW(sub)->superview = v;
    msg1(v, "didAddSubview:", sub);
    msg0(sub, "didMoveToSuperview");
    if (view_window(v))
        msg0(sub, "didMoveToWindow");
    objc_release(sub);
}
static void v_addSubview(CPU *c) { insert_sub(SELF, ARG(0), -1); }
static void v_insertAt(CPU *c) { insert_sub(SELF, ARG(0), (int)ARG(1)); }
static int index_of(id v, id sub)
{
    id s = VW(v)->subviews;
    for (u32 i = 0, n = s ? ns_array_count(s) : 0; i < n; i++)
        if (ns_array_at(s, i) == sub)
            return (int)i;
    return -1;
}
static void v_insertAbove(CPU *c)
{
    int i = index_of(SELF, ARG(1));
    insert_sub(SELF, ARG(0), i < 0 ? -1 : i + 1);
}
static void v_insertBelow(CPU *c)
{
    int i = index_of(SELF, ARG(1));
    insert_sub(SELF, ARG(0), i < 0 ? 0 : i);
}
static void v_removeFromSuperview(CPU *c) { remove_from_super(SELF); }
static void v_bringToFront(CPU *c)
{
    id sub = ARG(0);
    if (index_of(SELF, sub) >= 0) {
        objc_retain(sub);
        msg1(VW(SELF)->subviews, "removeObjectIdenticalTo:", sub);
        ns_array_add(VW(SELF)->subviews, sub);
        objc_release(sub);
    }
}
static void v_sendToBack(CPU *c)
{
    id sub = ARG(0);
    if (index_of(SELF, sub) >= 0) {
        objc_retain(sub);
        msg1(VW(SELF)->subviews, "removeObjectIdenticalTo:", sub);
        msg2(VW(SELF)->subviews, "insertObject:atIndex:", sub, 0);
        objc_release(sub);
    }
}
static void v_isDescendant(CPU *c)
{
    id v = SELF;
    for (; v && v != ARG(0); v = VW(v)->superview)
        ;
    ret_i(c, v != 0);
}
static id with_tag(id v, u32 tag)
{
    if (VW(v)->tag == tag)
        return v;
    id s = VW(v)->subviews;
    for (u32 i = 0, n = s ? ns_array_count(s) : 0; i < n; i++) {
        id r = with_tag(ns_array_at(s, i), tag);
        if (r)
            return r;
    }
    return 0;
}
static void v_viewWithTag(CPU *c) { ret_i(c, with_tag(SELF, ARG(0))); }
static void v_setHidden(CPU *c) { VW(SELF)->hidden = (u8)ARG(0); }
static void v_isHidden(CPU *c) { ret_i(c, VW(SELF)->hidden); }
static void v_setAlpha(CPU *c) { VW(SELF)->alpha = ARGF(0); }
static void v_alpha(CPU *c) { ret_f(c, VW(SELF)->alpha); }
static void v_setTag(CPU *c) { VW(SELF)->tag = ARG(0); }
static void v_tag(CPU *c) { ret_i(c, VW(SELF)->tag); }
static void v_setInteraction(CPU *c) { VW(SELF)->interaction = (u8)ARG(0); }
static void v_interaction(CPU *c) { ret_i(c, VW(SELF)->interaction); }
static void v_setMultitouch(CPU *c) { VW(SELF)->multitouch = (u8)ARG(0); }
static void v_multitouch(CPU *c) { ret_i(c, VW(SELF)->multitouch); }
static void v_setExclusive(CPU *c) { VW(SELF)->exclusive = (u8)ARG(0); }
static void v_setClips(CPU *c) { VW(SELF)->clips = (u8)ARG(0); }
static void v_clips(CPU *c) { ret_i(c, VW(SELF)->clips); }
static void v_setOpaque(CPU *c) { VW(SELF)->opaque = (u8)ARG(0); }
static void v_isOpaque(CPU *c) { ret_i(c, VW(SELF)->opaque); }
static void v_setBg(CPU *c)
{
    id old = VW(SELF)->bg;
    VW(SELF)->bg = objc_retain(ARG(0));
    objc_release(old);
}
static void v_bg(CPU *c) { ret_i(c, VW(SELF)->bg); }
static void v_setAutoresize(CPU *c) { VW(SELF)->autoresize = ARG(0); }
static void v_autoresize(CPU *c) { ret_i(c, VW(SELF)->autoresize); }
static void v_setContentMode(CPU *c) { VW(SELF)->content_mode = ARG(0); }
static void v_contentMode(CPU *c) { ret_i(c, VW(SELF)->content_mode); }
static void v_setScale(CPU *c)
{
    VW(SELF)->scale = ARGF(0);
    rt_log("[ui] -[%s setContentScaleFactor:%.2f]\n", objc_class_name(rd32(SELF)), ARGF(0));
}
static void v_scale(CPU *c) { ret_f(c, VW(SELF)->scale); }
static void v_sizeThatFits(CPU *c) { put_size(ST_RET, (GtSize){VW(ST_SELF)->bounds.w, VW(ST_SELF)->bounds.h}); }
static void v_convertPointTo(CPU *c)
{
    GtPoint p = {bits2f(ST_ARG(0)), bits2f(ST_ARG(1))};
    put_point(ST_RET, convert(p, ST_SELF, ST_ARG(2)));
}
static void v_convertPointFrom(CPU *c)
{
    GtPoint p = {bits2f(ST_ARG(0)), bits2f(ST_ARG(1))};
    put_point(ST_RET, convert(p, ST_ARG(2), ST_SELF));
}
static void v_convertRectTo(CPU *c)
{
    GtRect r = {bits2f(ST_ARG(0)), bits2f(ST_ARG(1)), bits2f(ST_ARG(2)), bits2f(ST_ARG(3))};
    put_rect(ST_RET, convert_rect(r, ST_SELF, ST_ARG(4)));
}
static void v_convertRectFrom(CPU *c)
{
    GtRect r = {bits2f(ST_ARG(0)), bits2f(ST_ARG(1)), bits2f(ST_ARG(2)), bits2f(ST_ARG(3))};
    put_rect(ST_RET, convert_rect(r, ST_ARG(4), ST_SELF));
}
static int point_inside(id v, GtPoint p)
{
    GtRect b = VW(v)->bounds;
    return p.x >= b.x && p.y >= b.y && p.x < b.x + b.w && p.y < b.y + b.h;
}
static void v_pointInside(CPU *c) { ret_i(c, point_inside(SELF, arg_point(c, 2))); }
/* hitTest: in the view's coordinates; the deepest visible, interactive view containing p */
static id hit_test(id v, GtPoint p)
{
    View *x = VW(v);
    if (x->hidden || !x->interaction || x->alpha < 0.01f)
        return 0;
    int inside = point_inside(v, p);
    if (!inside && x->clips)
        return 0;
    id s = x->subviews;
    for (u32 n = s ? ns_array_count(s) : 0; n-- > 0;) {
        id sub = ns_array_at(s, n);
        GtPoint q = aff_apply(aff_inv(to_super(sub)), p);
        id r = hit_test(sub, q);
        if (r)
            return r;
    }
    return inside ? v : 0;
}
static void v_hitTest(CPU *c) { ret_i(c, hit_test(SELF, arg_point(c, 2))); }
static void v_addGesture(CPU *c)
{
    if (!VW(SELF)->gestures)
        VW(SELF)->gestures = ns_array_new(NULL, 0);
    ns_array_add(VW(SELF)->gestures, ARG(0));
}
static void v_gestures(CPU *c) { ret_i(c, VW(SELF)->gestures); }

/* animations happen at once: the block, then the completion */
static u32 anim_delegate, anim_stop;
static void va_begin(CPU *c) { anim_delegate = anim_stop = 0; (void)c; }
static void va_setDelegate(CPU *c) { anim_delegate = ARG(0); }
static void va_setStop(CPU *c) { anim_stop = ARG(0); }
static void va_commit(CPU *c)
{
    (void)c;
    u32 d = anim_delegate, s = anim_stop;
    anim_delegate = anim_stop = 0;
    if (d && s) {
        u32 a[3] = {0, ns_num_bool(1), 0};
        objc_msg(d, sel_name(s), 3, a);
    }
}
static void va_animate(CPU *c) { call_block(arg_word(c, 4), 0, 0); }
static void va_animateCompletion(CPU *c)
{
    call_block(arg_word(c, 4), 0, 0);
    call_block(arg_word(c, 5), 1, 1);
}
static void va_animateDelay(CPU *c)
{
    /* animateWithDuration:(double)delay:(double)options:animations:completion: */
    call_block(arg_word(c, 7), 0, 0);
    call_block(arg_word(c, 8), 1, 1);
}

/* UIWindow */

static void w_initWithFrame(CPU *c)
{
    id w = SELF;
    view_setup(w, arg_rect(c, 2));
    VW(w)->is_window = 1;
    ret_i(c, w);
}
static void app_add_window(id w)
{
    App *a = AP(app_obj);
    if (!a->windows)
        a->windows = ns_array_new(NULL, 0);
    for (u32 i = 0, n = ns_array_count(a->windows); i < n; i++)
        if (ns_array_at(a->windows, i) == w)
            return;
    ns_array_add(a->windows, w);
}
static void w_makeKeyAndVisible(CPU *c)
{
    id w = SELF;
    app_add_window(w);
    AP(app_obj)->key_window = w;
    VW(w)->hidden = 0;
    rt_log("[ui] window %s %gx%g made key and visible\n", objc_class_name(rd32(w)), VW(w)->bounds.w, VW(w)->bounds.h);
}
static void w_makeKey(CPU *c)
{
    app_add_window(SELF);
    AP(app_obj)->key_window = SELF;
}
static void w_isKey(CPU *c) { ret_i(c, AP(app_obj)->key_window == SELF); }
static void w_root(CPU *c) { ret_i(c, WN(SELF)->root); }
static void w_setRoot(CPU *c)
{
    id w = SELF, vc = ARG(0), old = WN(w)->root;
    WN(w)->root = objc_retain(vc);
    if (old) {
        remove_from_super(msg0(old, "view"));
        objc_release(old);
    }
    if (vc) {
        id v = msg0(vc, "view");
        VW(v)->center = (GtPoint){UI_SCREEN_W / 2, UI_SCREEN_H / 2};
        VW(v)->bounds = (GtRect){0, 0, UI_SCREEN_W, UI_SCREEN_H};
        insert_sub(w, v, -1);
        rt_log("[ui] root view controller %s, view %s\n", objc_class_name(rd32(vc)), objc_class_name(rd32(v)));
    }
}
static void w_setLevel(CPU *c) { WN(SELF)->level = ARGF(0); }
static void w_level(CPU *c) { ret_f(c, WN(SELF)->level); }
static void w_screen(CPU *c) { ret_i(c, screen_obj); }

/* UIViewController */

static void vc_init(CPU *c) { VC(SELF)->orientation = UI_ORIENTATION; ret_i(c, SELF); }
static void vc_initWithNib(CPU *c)
{
    VC(SELF)->orientation = UI_ORIENTATION;
    VC(SELF)->nib = objc_retain(ARG(0));
    ret_i(c, SELF);
}
static void vc_loadView(CPU *c)
{
    u32 w[4] = {0, 0, f2bits(UI_SCREEN_W), f2bits(UI_SCREEN_H)};
    id v = objc_msg(objc_alloc_instance(cls_view, 0), "initWithFrame:", 4, w);
    msg1(SELF, "setView:", v);
    objc_release(v);
}
static void vc_view(CPU *c)
{
    id self = SELF;
    if (!VC(self)->view) {
        msg0(self, "loadView");
        if (VC(self)->view)
            msg0(self, "viewDidLoad");
    }
    ret_i(c, VC(self)->view);
}
static void vc_setView(CPU *c)
{
    id self = SELF, v = ARG(0), old = VC(self)->view;
    VC(self)->view = objc_retain(v);
    if (v)
        VW(v)->controller = self;
    if (old) {
        if (VW(old)->controller == self)
            VW(old)->controller = 0;
        objc_release(old);
    }
}
static void vc_isViewLoaded(CPU *c) { ret_i(c, VC(SELF)->view != 0); }
static void vc_dealloc(CPU *c)
{
    Controller *x = VC(SELF);
    if (x->view && VW(x->view)->controller == SELF)
        VW(x->view)->controller = 0;
    objc_release(x->view);
    objc_release(x->title);
    objc_release(x->nib);
    objc_free_instance(SELF);
}
static void vc_nextResponder(CPU *c)
{
    id v = VC(SELF)->view;
    ret_i(c, v ? VW(v)->superview : 0);
}
static void vc_present(CPU *c)
{
    id self = SELF, vc = ARG(0);
    if (!vc)
        return;
    VC(self)->presented = objc_retain(vc);
    VC(vc)->presenting = self;
    id v = msg0(vc, "view");
    id w = AP(app_obj)->key_window;
    msg1(vc, "viewWillAppear:", 0);
    if (w)
        insert_sub(w, v, -1);
    msg1(vc, "viewDidAppear:", 0);
    rt_log("[ui] %s presents %s\n", objc_class_name(rd32(self)), objc_class_name(rd32(vc)));
}
static void vc_presentCompletion(CPU *c)
{
    vc_present(c);
    call_block(ARG(2), 0, 0);
}
static void vc_dismiss(CPU *c)
{
    id self = SELF, vc = VC(self)->presented;
    if (!vc && VC(self)->presenting) {   /* sent to the presented controller */
        vc = self;
        self = VC(vc)->presenting;
    }
    if (!vc)
        return;
    msg1(vc, "viewWillDisappear:", 0);
    if (VC(vc)->view)
        remove_from_super(VC(vc)->view);
    msg1(vc, "viewDidDisappear:", 0);
    VC(vc)->presenting = 0;
    VC(self)->presented = 0;
    objc_release(vc);
}
static void vc_dismissCompletion(CPU *c)
{
    u32 block = ARG(1);
    vc_dismiss(c);
    call_block(block, 0, 0);
}
static void vc_presented(CPU *c) { ret_i(c, VC(SELF)->presented); }
static void vc_presenting(CPU *c) { ret_i(c, VC(SELF)->presenting); }
static void vc_parent(CPU *c) { ret_i(c, VC(SELF)->parent ? VC(SELF)->parent : VC(SELF)->presenting); }
static void vc_orientation(CPU *c) { ret_i(c, UI_ORIENTATION); }
static void vc_shouldAutorotate(CPU *c) { ret_i(c, ARG(0) == 1); }
static void vc_title(CPU *c) { ret_i(c, VC(SELF)->title); }
static void vc_setTitle(CPU *c)
{
    id old = VC(SELF)->title;
    VC(SELF)->title = objc_retain(msg0(ARG(0), "copy"));
    objc_release(old);
}

/* UIApplication */

static void a_shared(CPU *c) { ret_i(c, app_obj); }
static void a_delegate(CPU *c) { ret_i(c, AP(SELF)->delegate); }
static void a_setDelegate(CPU *c) { AP(SELF)->delegate = ARG(0); }
static void a_keyWindow(CPU *c) { ret_i(c, AP(SELF)->key_window); }
static void a_windows(CPU *c)
{
    id w = AP(SELF)->windows;
    ret_i(c, w ? w : objc_autorelease(ns_array_new(NULL, 0)));
}
static void a_setIdle(CPU *c) { AP(SELF)->idle_off = ARG(0) & 0xff; }
static void a_idle(CPU *c) { ret_i(c, AP(SELF)->idle_off); }
static void a_setBarHidden(CPU *c) { AP(SELF)->bar_hidden = ARG(0) & 0xff; }
static void a_barHidden(CPU *c) { ret_i(c, AP(SELF)->bar_hidden); }
static void a_barOrientation(CPU *c) { ret_i(c, AP(SELF)->bar_orientation); }
static void a_setBarOrientation(CPU *c)
{
    /* logged when it changes (optimisation pass 4): the game sets it every few frames, the same value each time
       (2,352 of a session's 2,849 log lines, each flushed to disk) */
    static u32 logged = 0xffffffffu;
    AP(SELF)->bar_orientation = ARG(0);
    if (ARG(0) != logged) {
        logged = ARG(0);
        rt_log("[ui] status bar orientation %u\n", ARG(0));
    }
}
static void a_barFrame(CPU *c) { put_rect(ST_RET, (GtRect){0, 0, 0, 0}); }
static void a_openURL(CPU *c)
{
    rt_log("[ui] openURL: %s (not opened)\n", ns_describe(ARG(0)));
    ret_i(c, 0);
}
static void a_state(CPU *c) { ret_i(c, AP(SELF)->state); }
static void a_badge(CPU *c) { ret_i(c, AP(SELF)->badge); }
static void a_setBadge(CPU *c) { AP(SELF)->badge = ARG(0); }
static void a_bgTask(CPU *c) { ret_i(c, 0); }
static void a_emptyArray(CPU *c) { ret_i(c, objc_autorelease(ns_array_new(NULL, 0))); }
static void a_sendAction(CPU *c)
{
    /* sendAction:to:from:forEvent: */
    if (ARG(1))
        msg1(ARG(1), sel_name(ARG(0)), ARG(2));
    ret_i(c, ARG(1) != 0);
}

/* UIScreen, UIScreenMode, UIDevice */

static void s_main(CPU *c) { ret_i(c, screen_obj); }
static void s_screens(CPU *c)
{
    u32 s = screen_obj;
    ret_i(c, objc_autorelease(ns_array_new(&s, 1)));
}
static void s_bounds(CPU *c) { put_rect(ST_RET, (GtRect){0, 0, UI_SCREEN_W, UI_SCREEN_H}); }
static void s_appFrame(CPU *c) { put_rect(ST_RET, (GtRect){0, 0, UI_SCREEN_W, UI_SCREEN_H}); }
static void s_scale(CPU *c) { ret_f(c, ui_scale); }
static id mode_obj;
static void s_mode(CPU *c) { ret_i(c, singleton(&mode_obj, "UIScreenMode", 8)); }
static void s_modes(CPU *c)
{
    u32 m = singleton(&mode_obj, "UIScreenMode", 8);
    ret_i(c, objc_autorelease(ns_array_new(&m, 1)));
}
static void sm_size(CPU *c) { put_size(ST_RET, (GtSize){UI_SCREEN_W * ui_scale, UI_SCREEN_H * ui_scale}); }
static void s_brightness(CPU *c) { ret_f(c, 1.0f); }

static const char *dev_model = "iPad2,1";
static void d_current(CPU *c) { ret_i(c, device_obj); }
static void d_model(CPU *c) { ret_i(c, ns_string("iPad")); }
static void d_systemName(CPU *c) { ret_i(c, ns_string("iPhone OS")); }
static void d_systemVersion(CPU *c) { ret_i(c, ns_string("5.1")); }
static void d_name(CPU *c) { ret_i(c, ns_string("iPad")); }
static void d_udid(CPU *c) { ret_i(c, ns_string("0123456789abcdef0123456789abcdef01234567")); }
static void d_orientation(CPU *c) { ret_i(c, UI_ORIENTATION); }
static void d_idiom(CPU *c) { ret_i(c, 1); }
static void d_battery(CPU *c) { ret_f(c, 1.0f); }
static void d_batteryState(CPU *c) { ret_i(c, 3); }   /* UIDeviceBatteryStateFull */

/* UIColor, UIFont */

static id color(u32 k, float r, float g, float b, float a)
{
    id o = ns_alloc(k, sizeof(Color));
    Color *x = GP(o);
    x->r = r, x->g = g, x->b = b, x->a = a;
    return objc_autorelease(o);
}
static void col_rgba(CPU *c) { ret_i(c, color(SELF, ARGF(0), ARGF(1), ARGF(2), ARGF(3))); }
static void col_white(CPU *c) { ret_i(c, color(SELF, ARGF(0), ARGF(0), ARGF(0), ARGF(1))); }
static void col_init_rgba(CPU *c)
{
    Color *x = GP(SELF);
    x->r = ARGF(0), x->g = ARGF(1), x->b = ARGF(2), x->a = ARGF(3);
    ret_i(c, SELF);
}
#define NAMED_COLOR(fn, r, g, b, a) static void fn(CPU *c) { ret_i(c, color(SELF, r, g, b, a)); }
NAMED_COLOR(col_black, 0, 0, 0, 1)
NAMED_COLOR(col_whiteC, 1, 1, 1, 1)
NAMED_COLOR(col_clear, 0, 0, 0, 0)
NAMED_COLOR(col_red, 1, 0, 0, 1)
NAMED_COLOR(col_green, 0, 1, 0, 1)
NAMED_COLOR(col_blue, 0, 0, 1, 1)
NAMED_COLOR(col_gray, 0.5f, 0.5f, 0.5f, 1)
NAMED_COLOR(col_dgray, 1 / 3.0f, 1 / 3.0f, 1 / 3.0f, 1)
NAMED_COLOR(col_lgray, 2 / 3.0f, 2 / 3.0f, 2 / 3.0f, 1)
NAMED_COLOR(col_yellow, 1, 1, 0, 1)
NAMED_COLOR(col_orange, 1, 0.5f, 0, 1)
static void col_withAlpha(CPU *c)
{
    Color *x = GP(SELF);
    ret_i(c, color(rd32(SELF), x->r, x->g, x->b, ARGF(0)));
}
static void col_cg(CPU *c) { ret_i(c, SELF); }
static void col_set(CPU *c) { (void)c; }

static void font_sized(CPU *c)
{
    /* systemFontOfSize: / boldSystemFontOfSize: / fontWithName:size: */
    int named = c->r[1] == sel_get("fontWithName:size:");
    id o = ns_alloc(SELF, sizeof(Font));
    Font *f = GP(o);
    f->size = named ? ARGF(1) : ARGF(0);
    f->name = objc_retain(named ? ARG(0) : ns_string("Helvetica"));
    ret_i(c, objc_autorelease(o));
}
static void font_size(CPU *c) { ret_f(c, ((Font *)GP(SELF))->size); }
static void font_name(CPU *c) { ret_i(c, ((Font *)GP(SELF))->name); }
static void font_lineHeight(CPU *c) { ret_f(c, ((Font *)GP(SELF))->size * 1.2f); }
static void font_withSize(CPU *c)
{
    id o = ns_alloc(rd32(SELF), sizeof(Font));
    ((Font *)GP(o))->size = ARGF(0);
    ((Font *)GP(o))->name = objc_retain(((Font *)GP(SELF))->name);
    ret_i(c, objc_autorelease(o));
}

/* touches */

static void t_locationInView(CPU *c)
{
    Touch *t = TC(ST_SELF);
    put_point(ST_RET, convert(t->loc, t->window, ST_ARG(0)));
}
static void t_prevInView(CPU *c)
{
    Touch *t = TC(ST_SELF);
    put_point(ST_RET, convert(t->prev, t->window, ST_ARG(0)));
}
static void t_phase(CPU *c) { ret_i(c, TC(SELF)->phase); }
static void t_taps(CPU *c) { ret_i(c, TC(SELF)->taps); }
static void t_time(CPU *c) { ret_d(c, TC(SELF)->t); }
static void t_view(CPU *c) { ret_i(c, TC(SELF)->view); }
static void t_window(CPU *c) { ret_i(c, TC(SELF)->window); }
static void t_dealloc(CPU *c)
{
    objc_release(TC(SELF)->view);
    objc_release(TC(SELF)->window);
    objc_free_instance(SELF);
}
static void e_all(CPU *c) { ret_i(c, ((Event *)GP(SELF))->touches); }
static void e_forView(CPU *c)
{
    id all = ((Event *)GP(SELF))->touches, out = ns_set_new(NULL, 0);
    id arr = msg0(all, "allObjects");
    for (u32 i = 0, n = ns_array_count(arr); i < n; i++) {
        id t = ns_array_at(arr, i);
        if (TC(t)->view == ARG(0) || TC(t)->window == ARG(0))
            msg1(out, "addObject:", t);
    }
    ret_i(c, out);
}
static void e_time(CPU *c) { ret_d(c, ((Event *)GP(SELF))->t); }
static void e_type(CPU *c) { ret_i(c, 0); }
static void e_dealloc(CPU *c)
{
    objc_release(((Event *)GP(SELF))->touches);
    objc_free_instance(SELF);
}

static id fingers[MAX_FINGERS];          /* active UITouch per finger, +1 */
static double last_tap_time;
static GtPoint last_tap_at;
static int last_taps;

/* WIDE_UI (widescreen.c): on a wider screen (GT_WIDE) gameswf draws the stage (1024 x 768) at one scale in
   the middle, ui_stage_dx landscape points from the left edge.  The host's own points (its taps, the pad's
   buttons, GT_TAPS, the pointer) are the stage's; the game turns a touch into a stage point by x * 1024 /
   screen width (logged in gameswf's hit test: a touch at 300 of 1366 came as 224.9), so a
   touch is handed over at stage x * width / 1024.  On a 4:3 screen all of it is the identity. */
float ui_stage_dx;
int ui_wide_ui;

/* landscape points (what the game takes: 0 .. screen width x 768) -> portrait screen points */
static GtPoint screen_from_landscape(float lx, float ly)
{
    if (UI_ORIENTATION == 3)
        return (GtPoint){UI_SCREEN_W - ly, lx};
    return (GtPoint){ly, UI_SCREEN_H - lx};
}

static id top_window(void)
{
    App *a = AP(app_obj);
    if (a->key_window && !VW(a->key_window)->hidden)
        return a->key_window;
    if (a->windows)
        for (u32 n = ns_array_count(a->windows); n-- > 0;) {
            id w = ns_array_at(a->windows, n);
            if (!VW(w)->hidden)
                return w;
        }
    return 0;
}

static const char *const phase_sel[] = {"touchesBegan:withEvent:", "touchesMoved:withEvent:", "",
                                        "touchesEnded:withEvent:", "touchesCancelled:withEvent:"};

/* phase 0 began, 1 moved, 3 ended; lx, ly in stage points (see ui_stage_dx) */
void ui_touch_event(int finger, int phase, float lx, float ly)
{
    if (finger < 0 || finger >= MAX_FINGERS || !app_obj)
        return;
    /* WIDE_UI: a race HUD part drawn at a wide screen's edge takes the touch at its own place
       (widescreen.c wide_hud_touch), the finger keeping that offset until it is lifted */
    static float finger_dx[MAX_FINGERS];
    if (phase == 0 && !fingers[finger]) {
        float ox = lx;
        wide_hud_touch(&lx, ly);
        finger_dx[finger] = lx - ox;
    } else {
        lx += finger_dx[finger];
    }
    GtPoint sp = screen_from_landscape(lx * UI_SCREEN_H / 1024.0f, ly);
    double now = ns_now();
    u32 mark = objc_pool_push();
    id t = fingers[finger];
    if (phase == 0) {
        if (t)
            goto out;
        id w = top_window();
        if (!w)
            goto out;
        GtPoint wp = convert(sp, 0, w);
        id v = hit_test(w, wp);
        if (!v)
            goto out;
        t = ns_alloc(cls_touch, sizeof(Touch));
        Touch *x = TC(t);
        x->view = objc_retain(v);
        x->window = objc_retain(w);
        x->loc = x->prev = wp;
        x->finger = (u32)finger;
        float dx = sp.x - last_tap_at.x, dy = sp.y - last_tap_at.y;
        last_taps = now - last_tap_time < 0.35 && dx * dx + dy * dy < 400 ? last_taps + 1 : 1;
        x->taps = (u32)last_taps;
        last_tap_time = now;
        last_tap_at = sp;
        fingers[finger] = t;
    } else {
        if (!t)
            goto out;
        Touch *x = TC(t);
        GtPoint wp = convert(sp, 0, x->window);
        if (phase == 1 && wp.x == x->loc.x && wp.y == x->loc.y)
            goto out;
        x->prev = x->loc;
        x->loc = wp;
    }
    Touch *x = TC(t);
    x->phase = (u32)phase;
    x->t = now;
    id ev = ns_alloc(cls_event, sizeof(Event));
    u32 all[MAX_FINGERS], n = 0;
    for (int i = 0; i < MAX_FINGERS; i++)
        if (fingers[i])
            all[n++] = fingers[i];
    ((Event *)GP(ev))->touches = objc_retain(ns_set_new(all, n));
    ((Event *)GP(ev))->t = now;
    objc_autorelease(ev);
    if (rt_trace)
        rt_log("[touch] %d %s (%.0f, %.0f) -> %s\n", finger, phase_sel[phase], x->loc.x, x->loc.y,
               objc_class_name(rd32(x->view)));
    msg2(x->view, phase_sel[phase], ns_set_new(&t, 1), ev);
    if (phase == 3 || phase == 4) {
        fingers[finger] = 0;
        objc_release(t);
    }
out:
    objc_pool_pop(mark);
}

/* UIAccelerometer: tilt from the keyboard or the pad */

typedef struct { u32 isa, delegate; double interval; } Accel;
typedef struct { u32 isa, pad; double t, x, y, z; } Accel_;   /* UIAcceleration: timestamp, x, y, z (doubles) */
static double accel_next;
static void ac_shared(CPU *c) { ret_i(c, singleton(&accel_obj, "UIAccelerometer", sizeof(Accel))); }
static void ac_setDelegate(CPU *c)
{
    ((Accel *)GP(SELF))->delegate = ARG(0);
    rt_log("[ui] accelerometer delegate %s\n", ARG(0) ? objc_class_name(rd32(ARG(0))) : "nil");
}
static void ac_delegate(CPU *c) { ret_i(c, ((Accel *)GP(SELF))->delegate); }
static void ac_setInterval(CPU *c) { ((Accel *)GP(SELF))->interval = ARGD(0); }
static void ac_interval(CPU *c) { ret_d(c, ((Accel *)GP(SELF))->interval); }
/* No CoreMotion: [CMMotionManager alloc] is nil.  GT Racing still takes its CoreMotion path (its
   choice comes from the device, not from the alloc), so its updates never start; steer.c fills the
   game's Accelerometer object itself.  The UIAccelerometer below serves any other user. */
static void cm_alloc(CPU *c) { ret_i(c, 0); }
static void acc_t(CPU *c) { ret_d(c, ((Accel_ *)GP(SELF))->t); }
static void acc_x(CPU *c) { ret_d(c, ((Accel_ *)GP(SELF))->x); }
static void acc_y(CPU *c) { ret_d(c, ((Accel_ *)GP(SELF))->y); }
static void acc_z(CPU *c) { ret_d(c, ((Accel_ *)GP(SELF))->z); }

/* gravity in the device's axes (x right, y up the portrait screen, z out of it) for the iPad held in
   landscape right, leaning back by ~40 degrees, turned like a wheel by ui_tilt (sign checked in a
   race: +y turns the car left, so right = -y) */
void ui_gravity(double g[3])
{
    /* Android: the phone's own accelerometer, unless the keys or a pad steer now */
    if (ui_sensor_ok && ui_tilt > -0.02f && ui_tilt < 0.02f) {
        g[0] = ui_sensor_g[0], g[1] = ui_sensor_g[1], g[2] = ui_sensor_g[2];
        return;
    }
    double ang = -ui_tilt * 0.6, lean = 0.7;
    g[0] = -cos(ang) * cos(lean);
    g[1] = sin(ang) * cos(lean);
    g[2] = -sin(lean);
}

void ui_accel_tick(void)
{
    Accel *a = accel_obj ? GP(accel_obj) : NULL;
    if (!a || !a->delegate)
        return;
    double now = ns_now(), iv = a->interval > 0.005 ? a->interval : 0.016;
    if (now < accel_next)
        return;
    accel_next = now + iv;
    double g[3];
    ui_gravity(g);
    id e = ns_alloc(cls("UIAcceleration"), sizeof(Accel_));
    Accel_ *x = GP(e);
    x->t = now;
    x->x = g[0];
    x->y = g[1];
    x->z = g[2];
    u32 mark = objc_pool_push();
    objc_autorelease(e);
    msg2(a->delegate, "accelerometer:didAccelerate:", accel_obj, e);
    objc_pool_pop(mark);
}

/* UIApplicationMain */

/* The window's pixel format: the one SDL would choose, but with PFD_SWAP_COPY when the driver has it.  With
   SDL's choice (PFD_SWAP_EXCHANGE) the NVIDIA OpenGL driver takes over a window covering the screen: full
   screen no longer goes through the desktop compositor, so Windows' own pop-ups (the volume and brightness
   bar) are hidden under the game, and screen captures freeze on the last windowed frame.  The Gangstar
   MV port had the same issue.  A port fault (an iPad has no such pop-ups).  The fix is
   the GMV port's (gl_context.c pf_window, found there on the same driver): SDL has no attribute for the swap
   method but copies the format of a window it is given, so a hidden one gets the format.  GT_SWAP_COPY=0 lets
   SDL choose.  (A layered window style also kept full screen composed, but not every time.) */
#ifdef _WIN32
static LRESULT CALLBACK pf_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static HWND pf_window(void)
{
    const char *sc = getenv("GT_SWAP_COPY");
    if (sc && *sc == '0')
        return NULL;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = pf_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "GTRacingFormat";
    wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP, 0, 0, 16, 16, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
        return NULL;
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {0}, want = {0};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (pf && DescribePixelFormat(dc, pf, sizeof want, &want) && !(want.dwFlags & PFD_SWAP_COPY)) {
        const DWORD need = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER | PFD_SWAP_COPY;
        int n = DescribePixelFormat(dc, 1, sizeof want, NULL);
        for (int i = 1; i <= n; i++) {
            PIXELFORMATDESCRIPTOR c = {0};
            if (!DescribePixelFormat(dc, i, sizeof c, &c))
                continue;
            if ((c.dwFlags & need) == need && !(c.dwFlags & PFD_GENERIC_FORMAT) && c.iPixelType == want.iPixelType &&
                c.cColorBits == want.cColorBits && c.cAlphaBits == want.cAlphaBits &&
                c.cDepthBits == want.cDepthBits && c.cStencilBits == want.cStencilBits &&
                c.cAccumBits == want.cAccumBits) {
                pf = i;
                break;
            }
        }
    }
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) {
        rt_log("[ui] no pixel format of our own (%lu); SDL chooses\n", GetLastError());
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return NULL;
    }
    PIXELFORMATDESCRIPTOR got = {0};
    DescribePixelFormat(dc, pf, sizeof got, &got);
    rt_log("[ui] pixel format %d: swap %s, colour %d, alpha %d, depth %d, stencil %d\n", pf,
           (got.dwFlags & PFD_SWAP_COPY) ? "copy" : (got.dwFlags & PFD_SWAP_EXCHANGE) ? "exchange" : "undefined",
           got.cColorBits, got.cAlphaBits, got.cDepthBits, got.cStencilBits);
    ReleaseDC(hwnd, dc);
    return hwnd;
}
#endif

/* the primary display's size in pixels (landscape: the longer side is the width); 0 if unknown */
static int monitor_size(int *w, int *h)
{
#ifdef _WIN32
    DEVMODEA dm = {0};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth && dm.dmPelsHeight) {
        *w = (int)dm.dmPelsWidth;
        *h = (int)dm.dmPelsHeight;
        return 1;
    }
    return 0;
#else
    /* Android: SDL's video, started here for good (ui_init runs before the window is made; SDL_Init counts its
       users, so make_window's own start is the second) */
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        return 0;
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    int ok = dm && dm->w > 0 && dm->h > 0;
    if (ok) {
        *w = dm->w > dm->h ? dm->w : dm->h;
        *h = dm->w > dm->h ? dm->h : dm->w;
    }
    return ok;
#endif
}

static void make_window(void)
{
#if GT_ANDROID
    /* the main loop keeps running in the background (the game is told, BACKGROUND_THROTTLE); the Back button is a
       key (pcinput.c: Esc), not the end of the activity */
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS | SDL_INIT_SENSOR))
        rt_fatal("SDL_Init: %s", SDL_GetError());
    /* OpenGL ES 3.0: the game's ES 2 calls as they are, and what the host's own drawing needs (blits, vertex
       arrays, mapped ranges) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
#else
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS))
        rt_fatal("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    int w = (int)UI_SCREEN_H, h = 768;
    if (dm && dm->h >= 1200) {
        w = w * 3 / 2;
        h = 1152;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "GT Racing: Motor Academy");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    /* FULL_SCREEN (display.c): GT_FULLSCREEN=1, written by Options > Display and Alt+Enter, starts full
       screen (the desktop's mode); w x h stay the window's size for Alt+Enter back */
    const char *fs = getenv("GT_FULLSCREEN");
    if (GT_ANDROID || (fs && *fs == '1')) {
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);
        rt_log("[ui] full screen (GT_FULLSCREEN=1)\n");
    }
#ifdef _WIN32
    HWND format_of = pf_window();
    if (format_of)
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_PIXEL_FORMAT_HWND_POINTER, format_of);
#endif
    ui_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
#ifdef _WIN32
    if (format_of)
        DestroyWindow(format_of);
#endif
    if (!ui_window)
        rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    ui_make_cursor();
    eagl_window_created();
}

static void post(const char *note) { ns_post_notification(note, app_obj); }

static void deliver(id delegate, const char *sel, const char *note)
{
    u32 mark = objc_pool_push();
    if (delegate && objc_responds(delegate, sel))
        msg1(delegate, sel, app_obj);
    if (note)
        post(note);
    objc_pool_pop(mark);
}

/* BACKGROUND_THROTTLE (optimisation pass 5)
   Why.  An iPad app in the background is suspended: it draws nothing.  Here the window went on drawing 60 frames a
   second (more with GT_FPS) behind other windows and while minimized, a whole core's worth of CPU and GPU work on a
   laptop for a picture nobody sees.  A port gap (iOS has no windows), not a fault of the game.
   What (polled once a pass of the main loop, from the window's flags):
   - the window not in front: frames capped at GT_BACKGROUND_FPS (30).  The game counts time in frames (eagl.c's
     60 cap), so it runs at half speed there, as in any 30 fps frame;
   - minimized: GT_MINIMIZED_FPS (10), and the game is told what iOS tells it when the Home button is pressed
     (applicationWillResignActive: / applicationDidEnterBackground:): it pauses a race itself (SetGamePause:1, its
     pause menu waits when the window comes back) and suspends its movie; back up, applicationWillEnterForeground: /
     applicationDidBecomeActive:.  GT_MINIMIZED_PAUSE=0: the frame cap only.  While minimized the 1 ms timer
     period (timeBeginPeriod, below) is given back to Windows: nothing then waits on whole milliseconds that
     anybody sees, and the frame cap's waits are on the precise timer (PRECISE_WAIT) anyway;
   - none of this while a multiplayer game may be open (a socket of the game's, the ONLINE_P2P tunnel, a GameKit
     session: Local, Bluetooth and Online): half speed or a pause on one machine would make the others wait or lose
     it.
   GT_BACKGROUND_THROTTLE=0: none of it. */
static int bg_on = -1, bg_fps_unfocused = 30, bg_fps_minimized = 10, bg_pause_on = 1;
static volatile int bg_fps;        /* the cap now (0 none), read by eagl.c's present on the render thread */
static volatile int bg_min;        /* minimized now (the present skips its picture) */
static int bg_paused;              /* resign / background told to the game */
int ui_background_fps(void) { return bg_fps; }
int ui_minimized(void) { return bg_min; }

static int env_int(const char *k, int dflt, int lo, int hi)
{
    const char *e = getenv(k);
    if (!e || !*e)
        return dflt;
    int v = atoi(e);
    return v < lo ? lo : v > hi ? hi : v;
}

static void bg_poll(id delegate)
{
    if (bg_on < 0) {
        const char *e = getenv("GT_BACKGROUND_THROTTLE");
        bg_on = !(e && *e == '0');
        bg_fps_unfocused = env_int("GT_BACKGROUND_FPS", 30, 1, 240);
        bg_fps_minimized = env_int("GT_MINIMIZED_FPS", 10, 1, 240);
        e = getenv("GT_MINIMIZED_PAUSE");
        bg_pause_on = !(e && *e == '0');
        if (bg_on)
            rt_log("[bg] background throttle: %d fps behind other windows, %d minimized%s\n", bg_fps_unfocused,
                   bg_fps_minimized, bg_pause_on ? ", the game paused while minimized" : "");
        else
            rt_log("[bg] background throttle off (GT_BACKGROUND_THROTTLE=0)\n");
    }
    if (!bg_on || !ui_window)
        return;
    SDL_WindowFlags f = SDL_GetWindowFlags(ui_window);
    /* Android: the app in the background (pcinput.c, SDL's DID_ENTER_BACKGROUND) is a minimized window */
    int min = (f & SDL_WINDOW_MINIMIZED) != 0 || ui_app_background, front = (f & SDL_WINDOW_INPUT_FOCUS) != 0;
    int multi = net_sockets_open() || gk_sessions_open();
    int fps = multi ? 0 : min ? bg_fps_minimized : !front ? bg_fps_unfocused : 0;
    static int last_fps = -1, last_multi = -1;
    if (multi != last_multi && (min || !front || last_multi >= 0)) {
        if (multi && (min || !front))
            rt_log("[bg] a multiplayer game may be open: no throttle, no pause\n");
        last_multi = multi;
    }
    bg_min = min && !multi;
    if (fps != last_fps) {
        const char *where = multi ? "multiplayer" : min ? "minimized" : !front ? "behind other windows" : "in front";
        if (fps)
            rt_log("[bg] %s: %d frames a second (frame %u)\n", where, fps, eagl_frames());
        else
            rt_log("[bg] %s: full frame rate (frame %u)\n", where, eagl_frames());
        last_fps = fps;
        bg_fps = fps;
    }
    int pause = bg_pause_on && min && !multi;
    if (pause && !bg_paused) {
        bg_paused = 1;
        rt_log("[bg] minimized: the game goes to the background as on iOS (frame %u)\n", eagl_frames());
        deliver(delegate, "applicationWillResignActive:", "UIApplicationWillResignActiveNotification");
        deliver(delegate, "applicationDidEnterBackground:", "UIApplicationDidEnterBackgroundNotification");
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    } else if (!min && bg_paused) {
        bg_paused = 0;
#ifdef _WIN32
        timeBeginPeriod(1);
#endif
        rt_log("[bg] restored: the game comes back to the foreground (frame %u)\n", eagl_frames());
        deliver(delegate, "applicationWillEnterForeground:", "UIApplicationWillEnterForegroundNotification");
        deliver(delegate, "applicationDidBecomeActive:", "UIApplicationDidBecomeActiveNotification");
    }
}

/* GT_TEST_MINIMIZE=frame:seconds (testing BACKGROUND_THROTTLE): the window is minimized at that presented frame and
   restored that many seconds later (a frame count would stall if the game stopped drawing) */
static void bg_test(void)
{
    static int at = -2;
    static double secs, back_at;
    if (at == -2) {
        const char *e = dev_getenv("GT_TEST_MINIMIZE");
        at = -1;
        if (e && sscanf(e, "%d:%lf", &at, &secs) != 2)
            at = -1;
    }
    if (at > 0 && (int)eagl_frames() >= at) {
        at = 0;
        back_at = ns_now() + secs;
        dev_log("[bg] test: minimizing the window (frame %u)\n", eagl_frames());
        SDL_MinimizeWindow(ui_window);
    } else if (at == 0 && back_at > 0 && ns_now() >= back_at) {
        back_at = 0;
        dev_log("[bg] test: restoring the window\n");
        SDL_RestoreWindow(ui_window);
        SDL_RaiseWindow(ui_window);
    }
}

u32 h_UIApplicationMain(u32 argc, u32 argv, u32 principal, u32 delegate_name)
{
    (void)argc;
    (void)argv;
    /* 1 ms timer ticks, kept (pass 5 looked at dropping it): the main loop's waits below (SDL_WaitEventTimeout) and
       the game's own threads' usleep / Sleep wait in whole milliseconds and would wake on Windows' 15.6 ms tick
       without it, which would break the 60 fps frame pacing.  Given back while minimized (BACKGROUND_THROTTLE). */
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    make_window();
    steer_init();
    u32 pc = principal ? objc_getClass_addr(ns_utf8(principal)) : 0;
    id app = msg0(objc_alloc_instance(pc ? pc : cls_app, 0), "init");
    if (!app_obj)
        app_obj = app;
    AP(app_obj)->bar_orientation = UI_ORIENTATION;
    id delegate = 0;
    if (delegate_name) {
        u32 dc = objc_getClass_addr(ns_utf8(delegate_name));
        if (!dc)
            rt_fatal("UIApplicationMain: no delegate class %s", ns_utf8(delegate_name));
        delegate = msg0(msg0(dc, "alloc"), "init");
        msg1(app_obj, "setDelegate:", delegate);
    }
    rt_log("[ui] UIApplicationMain: application %s, delegate %s\n", objc_class_name(rd32(app_obj)),
           delegate ? objc_class_name(rd32(delegate)) : "none");
    u32 mark = objc_pool_push();
    if (delegate && objc_responds(delegate, "application:didFinishLaunchingWithOptions:"))
        msg2(delegate, "application:didFinishLaunchingWithOptions:", app_obj, 0);
    else if (delegate && objc_responds(delegate, "applicationDidFinishLaunching:"))
        msg1(delegate, "applicationDidFinishLaunching:", app_obj);
    post("UIApplicationDidFinishLaunchingNotification");
    objc_pool_pop(mark);
    deliver(delegate, "applicationDidBecomeActive:", "UIApplicationDidBecomeActiveNotification");
    rt_log("[ui] launched; running the main loop\n");
    while (!ui_quit) {
        ui_pump();
        bg_test();
        bg_poll(delegate);
        double next = ns_run_loop_once();
        if (ui_quit)
            break;
        if (ns_precise_wait_on()) {
            /* PRECISE_WAIT (ns_misc.c): the window's events wake a wait of whole milliseconds (ended 1 ms
               early); the rest sleeps on the precise timer instead of spinning */
            if (next > 0.002) {
                int ms = (int)(next * 1000.0) - 1;
                SDL_WaitEventTimeout(NULL, ms > 50 ? 50 : ms);
            } else if (next > 0) {
                ns_wait(next);
            }
        } else if (next > 0.0005) {
            int ms = (int)(next * 1000.0);
            if (ms > 50)
                ms = 50;
            SDL_WaitEventTimeout(NULL, ms);
        }
    }
    rt_log("[ui] window closed: the application terminates\n");
    if (!bg_paused) {                  /* closed while minimized: the game is in the background already */
        deliver(delegate, "applicationWillResignActive:", "UIApplicationWillResignActiveNotification");
        deliver(delegate, "applicationDidEnterBackground:", "UIApplicationDidEnterBackgroundNotification");
    }
    deliver(delegate, "applicationWillTerminate:", "UIApplicationWillTerminateNotification");
    exit(0);
}

/* the device model as Darwin tells it: sysctl, uname */

static u32 sysctl_value(const void *v, u32 n, u32 oldp, u32 oldlenp)
{
    u32 room = oldlenp ? rd32(oldlenp) : 0;
    if (oldlenp)
        wr32(oldlenp, n);
    if (!oldp)
        return 0;
    if (room < n)
        return (u32)-1;
    memcpy(GP(oldp), v, n);
    return 0;
}
static u32 sysctl_named(const char *name, u32 oldp, u32 oldlenp)
{
    u32 i4;
    u64 i8;
    if (!strcmp(name, "hw.machine"))
        return sysctl_value(dev_model, (u32)strlen(dev_model) + 1, oldp, oldlenp);
    if (!strcmp(name, "hw.model")) {
        const char *m = !strcmp(dev_model, "iPad3,1") ? "J1AP" : "K93AP";
        return sysctl_value(m, (u32)strlen(m) + 1, oldp, oldlenp);
    }
    if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.activecpu") || !strcmp(name, "hw.physicalcpu"))
        return i4 = 2, sysctl_value(&i4, 4, oldp, oldlenp);
    if (!strcmp(name, "hw.physmem") || !strcmp(name, "hw.usermem"))
        return i4 = 512u << 20, sysctl_value(&i4, 4, oldp, oldlenp);
    if (!strcmp(name, "hw.memsize"))
        return i8 = 512ull << 20, sysctl_value(&i8, 8, oldp, oldlenp);
    if (!strcmp(name, "hw.cpufrequency") || !strcmp(name, "hw.cpufrequency_max"))
        return i4 = 1000000000u, sysctl_value(&i4, 4, oldp, oldlenp);
    if (!strcmp(name, "kern.osrelease"))
        return sysctl_value("11.0.0", 7, oldp, oldlenp);
    if (!strcmp(name, "kern.osversion"))
        return sysctl_value("9B176", 6, oldp, oldlenp);
    rt_log("[ui] sysctl %s: not known\n", name);
    return (u32)-1;
}
u32 h_sysctlbyname(u32 name, u32 oldp, u32 oldlenp, u32 newp, u32 newlen)
{
    (void)newp;
    (void)newlen;
    return sysctl_named((const char *)GP(name), oldp, oldlenp);
}
u32 h_sysctl(u32 mib, u32 n, u32 oldp, u32 oldlenp, u32 newp, u32 newlen)
{
    (void)newp;
    (void)newlen;
    u32 a = n > 0 ? rd32(mib) : 0, b = n > 1 ? rd32(mib + 4) : 0;
    static const char *const hw[] = {NULL, "hw.machine", "hw.model", "hw.ncpu", NULL, "hw.physmem", "hw.usermem"};
    if (a == 6 && b < 7 && hw[b])                     /* CTL_HW */
        return sysctl_named(hw[b], oldp, oldlenp);
    if (a == 6 && b == 24)
        return sysctl_named("hw.memsize", oldp, oldlenp);
    if (a == 6 && b == 15)
        return sysctl_named("hw.cpufrequency", oldp, oldlenp);
    if (a == 1 && b == 2)                             /* CTL_KERN, KERN_OSRELEASE */
        return sysctl_named("kern.osrelease", oldp, oldlenp);
    if (a == 4 && b == 17 && n == 6 && rd32(mib + 16) == 3) {   /* CTL_NET, PF_ROUTE, .., NET_RT_IFLIST, index */
        if (rd32(mib + 20) == 1)
            return net_iflist(1, oldp, oldlenp);
    }
    rt_log("[ui] sysctl {%u, %u, ...} (%u names): not known\n", a, b, n);
    return (u32)-1;
}
u32 h_uname(u32 buf)
{
    char *u = GP(buf);
    memset(u, 0, 5 * 256);
    strcpy(u, "Darwin");
    strcpy(u + 256, "iPad");
    strcpy(u + 512, "11.0.0");
    strcpy(u + 768, "Darwin Kernel Version 11.0.0");
    strcpy(u + 1024, dev_model);
    return 0;
}

/* registration */

void ui_init(void)
{
    main_tid = GetCurrentThreadId();
    const char *dev = getenv("GT_DEVICE");
    if (dev && !strcmp(dev, "ipad3")) {
        dev_model = "iPad3,1";
        ui_scale = 2.0f;
    }
    /* GT_WIDE=<landscape width in points, 768 high> or "monitor" (the primary monitor's shape; DISPLAY_MENU's
       Aspect Ratio row writes 1024 for 4:3, 1229 for 16:10, 1366 for 16:9, 1792 for 21:9) */
    const char *wide = getenv("GT_WIDE");
    int wide_w = rt_parse_int(wide, 0, 0, 100000);
    if (wide && !_stricmp(wide, "monitor")) {
        int mw, mh;
        if (monitor_size(&mw, &mh))
            wide_w = (int)(768.0 * mw / mh + 0.5);
        rt_log("[ui] GT_WIDE=monitor: %d x 768\n", wide_w);
    }
    if (wide_w > 1024 && wide_w <= 2048) {
        ui_screen_h = (float)wide_w;
        /* WIDE_UI (widescreen.c): the menus and the HUD at one scale, in the middle; GT_WIDE_UI=0 stretches
           them over the width as the first widescreen probe did */
        const char *wu = getenv("GT_WIDE_UI");
        ui_wide_ui = !(wu && *wu == '0');
        ui_stage_dx = ui_wide_ui ? (ui_screen_h - 1024.0f) / 2 : 0;
        rt_log("[ui] GT_WIDE=%s: a %d x 768 screen, the menus %s\n", wide, wide_w,
               ui_wide_ui ? "in the middle at one scale (WIDE_UI)" : "stretched over it");
    }
    /* GT_RESOLUTION: the game draws
       its picture at this height in pixels instead of the device's own (768 on an iPad 2, 1536 on an iPad 3);
       "monitor" = the primary monitor's height, or less if its width would not hold the picture (1440 x 1080 on
       a 1920 x 1080 screen: the window's 4:3 picture in full screen, drawn 1:1).  It is the UIScreen scale (and
       so the layer's contentsScale and the renderbuffer's size) that changes; the device (GT_DEVICE) stays,
       hw.model follows the device, not the scale.  Tested at 1.40625 (1080 x 1440): menus, race
       settings and a Quick Race start drawn whole, taps in place, 60 fps.  0: the device's own.  Not set: "monitor",
       as GT_DEVICE ipad3, GT_WIDE monitor and GT_FULLSCREEN 1 (main.c default_settings). */
    const char *res = getenv("GT_RESOLUTION");
    if (res && *res && strcmp(res, "0")) {
        float s = 0;
        if (!_stricmp(res, "monitor")) {
            int mw, mh;
            if (monitor_size(&mw, &mh))
                s = fminf(mw / UI_SCREEN_H, mh / UI_SCREEN_W);
        } else if (rt_parse_int(res, 0, 0, 100000) > 0)
            s = rt_parse_int(res, 0, 0, 100000) / UI_SCREEN_W;
        if (s >= 0.5f && s <= 4.0f) {
            ui_scale = s;
            rt_log("[ui] GT_RESOLUTION=%s: the picture drawn at %d x %d\n", res, (int)(UI_SCREEN_H * s + 0.5f),
                   (int)(UI_SCREEN_W * s + 0.5f));
        } else
            rt_log("[ui] GT_RESOLUTION=%s not used (scale %g; 384 .. 3072 pixels high)\n", res, s);
    }
    objc_host_size("UIApplication", sizeof(App));
    objc_host_size("UIView", sizeof(View));
    objc_host_size("UIWindow", sizeof(Window));
    objc_host_size("UIViewController", sizeof(Controller));
    objc_host_size("CALayer", sizeof(Layer));
    cls_view = cls("UIView");
    cls_window = cls("UIWindow");
    cls_layer = cls("CALayer");
    cls_touch = cls("UITouch");
    cls_event = cls("UIEvent");
    cls_color = cls("UIColor");
    cls_app = cls("UIApplication");
    screen_obj = ns_alloc(cls("UIScreen"), 8);
    device_obj = ns_alloc(cls("UIDevice"), 8);

    static const NsMethod responder[] = {
        {"nextResponder", r_next, 0}, {"isFirstResponder", r_no, 0}, {"becomeFirstResponder", r_yes, 0},
        {"resignFirstResponder", r_yes, 0}, {"canBecomeFirstResponder", r_no, 0},
        {"touchesBegan:withEvent:", r_forward, 0}, {"touchesMoved:withEvent:", r_forward, 0},
        {"touchesEnded:withEvent:", r_forward, 0}, {"touchesCancelled:withEvent:", r_forward, 0},
        {"motionBegan:withEvent:", r_nop, 0}, {"motionEnded:withEvent:", r_nop, 0},
        {"motionCancelled:withEvent:", r_nop, 0}, {NULL, NULL, 0}};
    ns_methods("UIResponder", responder);

    static const NsMethod layer[] = {
        {"init", ly_init, 0}, {"layer", ly_layer, 1}, {"bounds", ly_bounds, 0}, {"setBounds:", ly_setBounds, 0},
        {"frame", ly_frame, 0}, {"setFrame:", ly_setFrame, 0}, {"position", ly_position, 0},
        {"setPosition:", ly_setPosition, 0}, {"setOpaque:", ly_setOpaque, 0}, {"isOpaque", ly_isOpaque, 0},
        {"opaque", ly_isOpaque, 0}, {"setHidden:", ly_setHidden, 0}, {"setOpacity:", ly_setOpacity, 0},
        {"opacity", ly_opacity, 0}, {"setContentsScale:", ly_setScale, 0}, {"contentsScale", ly_scale, 0},
        {"setDrawableProperties:", ly_setProps, 0}, {"drawableProperties", ly_props, 0},
        {"delegate", ly_delegate, 0}, {"setDelegate:", ly_setDelegate, 0}, {"sublayers", ly_sublayers, 0},
        {"addSublayer:", ly_addSublayer, 0}, {"dealloc", ly_dealloc, 0}, {"anchorPoint", ly_anchor, 0},
        {"setAnchorPoint:", ly_setAnchor, 0}, {"transform", ly_transform3d, 0}, {"setTransform:", r_nop, 0},
        {"setNeedsDisplay", r_nop, 0}, {"setMasksToBounds:", r_nop, 0}, {"setCornerRadius:", r_nop, 0},
        {"setBorderWidth:", r_nop, 0}, {"setBorderColor:", r_nop, 0}, {"setShadowOpacity:", r_nop, 0},
        {"setShadowColor:", r_nop, 0}, {"setShadowOffset:", r_nop, 0}, {"setShadowRadius:", r_nop, 0},
        {"addAnimation:forKey:", r_nop, 0}, {"removeAllAnimations", r_nop, 0},
        {"removeAnimationForKey:", r_nop, 0}, {"setContents:", r_nop, 0}, {NULL, NULL, 0}};
    ns_methods("CALayer", layer);

    static const NsMethod view[] = {
        {"layerClass", v_layerClass, 1}, {"init", v_init, 0}, {"initWithFrame:", v_initWithFrame, 0},
        {"initWithCoder:", v_initWithCoder, 0}, {"dealloc", v_dealloc, 0}, {"frame", v_frame, 0},
        {"setFrame:", v_setFrame, 0}, {"bounds", v_bounds, 0}, {"setBounds:", v_setBounds, 0},
        {"center", v_center, 0}, {"setCenter:", v_setCenter, 0}, {"transform", v_transform, 0},
        {"setTransform:", v_setTransform, 0}, {"layer", v_layer, 0}, {"superview", v_superview, 0},
        {"window", v_window, 0}, {"subviews", v_subviews, 0}, {"nextResponder", v_nextResponder, 0},
        {"addSubview:", v_addSubview, 0}, {"insertSubview:atIndex:", v_insertAt, 0},
        {"insertSubview:aboveSubview:", v_insertAbove, 0}, {"insertSubview:belowSubview:", v_insertBelow, 0},
        {"removeFromSuperview", v_removeFromSuperview, 0}, {"bringSubviewToFront:", v_bringToFront, 0},
        {"sendSubviewToBack:", v_sendToBack, 0}, {"isDescendantOfView:", v_isDescendant, 0},
        {"viewWithTag:", v_viewWithTag, 0}, {"setHidden:", v_setHidden, 0}, {"isHidden", v_isHidden, 0},
        {"hidden", v_isHidden, 0}, {"setAlpha:", v_setAlpha, 0}, {"alpha", v_alpha, 0}, {"setTag:", v_setTag, 0},
        {"tag", v_tag, 0}, {"setUserInteractionEnabled:", v_setInteraction, 0},
        {"isUserInteractionEnabled", v_interaction, 0}, {"setMultipleTouchEnabled:", v_setMultitouch, 0},
        {"isMultipleTouchEnabled", v_multitouch, 0}, {"setExclusiveTouch:", v_setExclusive, 0},
        {"setClipsToBounds:", v_setClips, 0}, {"clipsToBounds", v_clips, 0}, {"setOpaque:", v_setOpaque, 0},
        {"isOpaque", v_isOpaque, 0}, {"setBackgroundColor:", v_setBg, 0}, {"backgroundColor", v_bg, 0},
        {"setAutoresizingMask:", v_setAutoresize, 0}, {"autoresizingMask", v_autoresize, 0},
        {"setAutoresizesSubviews:", r_nop, 0}, {"setContentMode:", v_setContentMode, 0},
        {"contentMode", v_contentMode, 0}, {"setContentScaleFactor:", v_setScale, 0},
        {"contentScaleFactor", v_scale, 0}, {"setNeedsDisplay", r_nop, 0}, {"setNeedsLayout", r_nop, 0},
        {"layoutIfNeeded", r_nop, 0}, {"layoutSubviews", r_nop, 0}, {"sizeToFit", r_nop, 0},
        {"sizeThatFits:", v_sizeThatFits, 0}, {"drawRect:", r_nop, 0}, {"convertPoint:toView:", v_convertPointTo, 0},
        {"convertPoint:fromView:", v_convertPointFrom, 0}, {"convertRect:toView:", v_convertRectTo, 0},
        {"convertRect:fromView:", v_convertRectFrom, 0}, {"hitTest:withEvent:", v_hitTest, 0},
        {"pointInside:withEvent:", v_pointInside, 0}, {"addGestureRecognizer:", v_addGesture, 0},
        {"gestureRecognizers", v_gestures, 0}, {"removeGestureRecognizer:", r_nop, 0}, {"endEditing:", r_yes, 0},
        {"willMoveToSuperview:", r_nop, 0}, {"didMoveToSuperview", r_nop, 0}, {"didMoveToWindow", r_nop, 0},
        {"didAddSubview:", r_nop, 0}, {"willRemoveSubview:", r_nop, 0}, {"setNeedsDisplayInRect:", r_nop, 0},
        {"beginAnimations:context:", va_begin, 1}, {"commitAnimations", va_commit, 1},
        {"setAnimationDuration:", r_nop, 1}, {"setAnimationDelegate:", va_setDelegate, 1},
        {"setAnimationDidStopSelector:", va_setStop, 1}, {"setAnimationCurve:", r_nop, 1},
        {"setAnimationsEnabled:", r_nop, 1}, {"setAnimationDelay:", r_nop, 1},
        {"setAnimationBeginsFromCurrentState:", r_nop, 1}, {"setAnimationTransition:forView:cache:", r_nop, 1},
        {"setAnimationRepeatCount:", r_nop, 1}, {"setAnimationWillStartSelector:", r_nop, 1},
        {"animateWithDuration:animations:", va_animate, 1},
        {"animateWithDuration:animations:completion:", va_animateCompletion, 1},
        {"animateWithDuration:delay:options:animations:completion:", va_animateDelay, 1}, {NULL, NULL, 0}};
    ns_methods("UIView", view);

    static const NsMethod window[] = {
        {"initWithFrame:", w_initWithFrame, 0}, {"makeKeyAndVisible", w_makeKeyAndVisible, 0},
        {"makeKeyWindow", w_makeKey, 0}, {"isKeyWindow", w_isKey, 0}, {"rootViewController", w_root, 0},
        {"setRootViewController:", w_setRoot, 0}, {"setWindowLevel:", w_setLevel, 0}, {"windowLevel", w_level, 0},
        {"screen", w_screen, 0}, {"setScreen:", r_nop, 0}, {"resignKeyWindow", r_nop, 0},
        {"becomeKeyWindow", r_nop, 0}, {NULL, NULL, 0}};
    ns_methods("UIWindow", window);

    static const NsMethod controller[] = {
        {"init", vc_init, 0}, {"initWithNibName:bundle:", vc_initWithNib, 0}, {"loadView", vc_loadView, 0},
        {"view", vc_view, 0}, {"setView:", vc_setView, 0}, {"isViewLoaded", vc_isViewLoaded, 0},
        {"dealloc", vc_dealloc, 0}, {"nextResponder", vc_nextResponder, 0}, {"viewDidLoad", r_nop, 0},
        {"viewDidUnload", r_nop, 0}, {"viewWillAppear:", r_nop, 0}, {"viewDidAppear:", r_nop, 0},
        {"viewWillDisappear:", r_nop, 0}, {"viewDidDisappear:", r_nop, 0}, {"didReceiveMemoryWarning", r_nop, 0},
        {"presentModalViewController:animated:", vc_present, 0},
        {"presentViewController:animated:completion:", vc_presentCompletion, 0},
        {"dismissModalViewControllerAnimated:", vc_dismiss, 0},
        {"dismissViewControllerAnimated:completion:", vc_dismissCompletion, 0},
        {"modalViewController", vc_presented, 0}, {"presentedViewController", vc_presented, 0},
        {"presentingViewController", vc_presenting, 0}, {"parentViewController", vc_parent, 0},
        {"interfaceOrientation", vc_orientation, 0},
        {"shouldAutorotateToInterfaceOrientation:", vc_shouldAutorotate, 0}, {"title", vc_title, 0},
        {"setTitle:", vc_setTitle, 0}, {"setWantsFullScreenLayout:", r_nop, 0},
        {"setModalTransitionStyle:", r_nop, 0}, {"setModalPresentationStyle:", r_nop, 0},
        {"navigationItem", r_next, 0}, {"navigationController", r_next, 0}, {"tabBarItem", r_next, 0},
        {"willRotateToInterfaceOrientation:duration:", r_nop, 0},
        {"didRotateFromInterfaceOrientation:", r_nop, 0}, {"setHidesBottomBarWhenPushed:", r_nop, 0},
        {"setContentSizeForViewInPopover:", r_nop, 0}, {NULL, NULL, 0}};
    ns_methods("UIViewController", controller);

    static const NsMethod app[] = {
        {"sharedApplication", a_shared, 1}, {"delegate", a_delegate, 0}, {"setDelegate:", a_setDelegate, 0},
        {"keyWindow", a_keyWindow, 0}, {"windows", a_windows, 0}, {"setIdleTimerDisabled:", a_setIdle, 0},
        {"isIdleTimerDisabled", a_idle, 0}, {"setStatusBarHidden:", a_setBarHidden, 0},
        {"setStatusBarHidden:animated:", a_setBarHidden, 0}, {"setStatusBarHidden:withAnimation:", a_setBarHidden, 0},
        {"isStatusBarHidden", a_barHidden, 0}, {"statusBarOrientation", a_barOrientation, 0},
        {"setStatusBarOrientation:", a_setBarOrientation, 0},
        {"setStatusBarOrientation:animated:", a_setBarOrientation, 0}, {"statusBarFrame", a_barFrame, 0},
        {"setStatusBarStyle:", r_nop, 0}, {"setStatusBarStyle:animated:", r_nop, 0}, {"openURL:", a_openURL, 0},
        {"canOpenURL:", r_no, 0}, {"applicationState", a_state, 0},
        {"registerForRemoteNotificationTypes:", r_nop, 0}, {"unregisterForRemoteNotifications", r_nop, 0},
        {"enabledRemoteNotificationTypes", r_no, 0}, {"applicationIconBadgeNumber", a_badge, 0},
        {"setApplicationIconBadgeNumber:", a_setBadge, 0}, {"cancelAllLocalNotifications", r_nop, 0},
        {"scheduleLocalNotification:", r_nop, 0}, {"cancelLocalNotification:", r_nop, 0},
        {"presentLocalNotificationNow:", r_nop, 0}, {"scheduledLocalNotifications", a_emptyArray, 0},
        {"beginBackgroundTaskWithExpirationHandler:", a_bgTask, 0}, {"endBackgroundTask:", r_nop, 0},
        {"setNetworkActivityIndicatorVisible:", r_nop, 0}, {"isNetworkActivityIndicatorVisible", r_no, 0},
        {"beginIgnoringInteractionEvents", r_nop, 0}, {"endIgnoringInteractionEvents", r_nop, 0},
        {"isIgnoringInteractionEvents", r_no, 0}, {"setApplicationSupportsShakeToEdit:", r_nop, 0},
        {"sendAction:to:from:forEvent:", a_sendAction, 0}, {"backgroundTimeRemaining", r_no, 0},
        {NULL, NULL, 0}};
    ns_methods("UIApplication", app);

    static const NsMethod screen[] = {
        {"mainScreen", s_main, 1}, {"screens", s_screens, 1}, {"bounds", s_bounds, 0},
        {"applicationFrame", s_appFrame, 0}, {"scale", s_scale, 0}, {"currentMode", s_mode, 0},
        {"preferredMode", s_mode, 0}, {"availableModes", s_modes, 0}, {"brightness", s_brightness, 0},
        {"setBrightness:", r_nop, 0}, {NULL, NULL, 0}};
    ns_methods("UIScreen", screen);
    cg_init();
    static const NsMethod mode[] = {{"size", sm_size, 0}, {NULL, NULL, 0}};
    ns_methods("UIScreenMode", mode);

    static const NsMethod device[] = {
        {"currentDevice", d_current, 1}, {"model", d_model, 0}, {"localizedModel", d_model, 0},
        {"systemName", d_systemName, 0}, {"systemVersion", d_systemVersion, 0}, {"name", d_name, 0},
        {"uniqueIdentifier", d_udid, 0}, {"orientation", d_orientation, 0}, {"userInterfaceIdiom", d_idiom, 0},
        {"isMultitaskingSupported", r_yes, 0}, {"beginGeneratingDeviceOrientationNotifications", r_nop, 0},
        {"endGeneratingDeviceOrientationNotifications", r_nop, 0},
        {"isGeneratingDeviceOrientationNotifications", r_yes, 0}, {"batteryLevel", d_battery, 0},
        {"batteryState", d_batteryState, 0}, {"setBatteryMonitoringEnabled:", r_nop, 0},
        {"isBatteryMonitoringEnabled", r_no, 0}, {"proximityState", r_no, 0},
        {"setProximityMonitoringEnabled:", r_nop, 0}, {"isProximityMonitoringEnabled", r_no, 0},
        {NULL, NULL, 0}};
    ns_methods("UIDevice", device);

    static const NsMethod colors[] = {
        {"colorWithRed:green:blue:alpha:", col_rgba, 1}, {"colorWithWhite:alpha:", col_white, 1},
        {"initWithRed:green:blue:alpha:", col_init_rgba, 0}, {"blackColor", col_black, 1},
        {"whiteColor", col_whiteC, 1}, {"clearColor", col_clear, 1}, {"redColor", col_red, 1},
        {"greenColor", col_green, 1}, {"blueColor", col_blue, 1}, {"grayColor", col_gray, 1},
        {"darkGrayColor", col_dgray, 1}, {"lightGrayColor", col_lgray, 1}, {"yellowColor", col_yellow, 1},
        {"orangeColor", col_orange, 1}, {"groupTableViewBackgroundColor", col_lgray, 1},
        {"scrollViewTexturedBackgroundColor", col_dgray, 1}, {"colorWithPatternImage:", col_gray, 1},
        {"colorWithAlphaComponent:", col_withAlpha, 0}, {"CGColor", col_cg, 0}, {"set", col_set, 0},
        {"setFill", col_set, 0}, {"setStroke", col_set, 0}, {NULL, NULL, 0}};
    ns_methods("UIColor", colors);

    static const NsMethod fonts[] = {
        {"systemFontOfSize:", font_sized, 1}, {"boldSystemFontOfSize:", font_sized, 1},
        {"italicSystemFontOfSize:", font_sized, 1}, {"fontWithName:size:", font_sized, 1},
        {"pointSize", font_size, 0}, {"fontName", font_name, 0}, {"familyName", font_name, 0},
        {"lineHeight", font_lineHeight, 0}, {"fontWithSize:", font_withSize, 0}, {NULL, NULL, 0}};
    ns_methods("UIFont", fonts);

    static const NsMethod touch[] = {
        {"locationInView:", t_locationInView, 0}, {"previousLocationInView:", t_prevInView, 0},
        {"phase", t_phase, 0}, {"tapCount", t_taps, 0}, {"timestamp", t_time, 0}, {"view", t_view, 0},
        {"window", t_window, 0}, {"dealloc", t_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("UITouch", touch);
    static const NsMethod event[] = {
        {"allTouches", e_all, 0}, {"touchesForView:", e_forView, 0}, {"touchesForWindow:", e_forView, 0},
        {"timestamp", e_time, 0}, {"type", e_type, 0}, {"subtype", e_type, 0}, {"dealloc", e_dealloc, 0},
        {NULL, NULL, 0}};
    ns_methods("UIEvent", event);

    static const NsMethod accel[] = {
        {"sharedAccelerometer", ac_shared, 1}, {"setDelegate:", ac_setDelegate, 0}, {"delegate", ac_delegate, 0},
        {"setUpdateInterval:", ac_setInterval, 0}, {"updateInterval", ac_interval, 0}, {NULL, NULL, 0}};
    ns_methods("UIAccelerometer", accel);
    static const NsMethod acceleration[] = {
        {"timestamp", acc_t, 0}, {"x", acc_x, 0}, {"y", acc_y, 0}, {"z", acc_z, 0}, {NULL, NULL, 0}};
    ns_methods("UIAcceleration", acceleration);
    static const NsMethod motion[] = {{"alloc", cm_alloc, 1}, {"allocWithZone:", cm_alloc, 1}, {NULL, NULL, 0}};
    ns_methods("CMMotionManager", motion);

    gt_blocks_init();
    gt_net_init();
    gt_gk_init();                       /* LAN_MULTIPLAYER: GKSession over the network (gk.c) */
    eagl_init();
    al_init();
}
