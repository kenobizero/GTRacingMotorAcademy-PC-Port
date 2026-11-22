/* eagl.c -- EAGLContext on SDL's OpenGL contexts, and the game's picture on the PC window.

   On iOS the game renders into a framebuffer object whose colour renderbuffer gets its storage from
   the CAEAGLLayer (renderbufferStorage:fromDrawable:), and presentRenderbuffer: hands it to Core
   Animation, which composites the layer with its views' transforms onto the portrait screen.  Here
   that renderbuffer is copied into a texture and drawn to the window the same way: through the
   view's transform to the screen, then turned to the landscape the player sees, letterboxed.

   Testing: GT_SHOTS=120,600 saves the window at those presented frames (build\host\shots\);
   GT_QUIT_AT=n closes the application after n frames; GT_VSYNC=0 turns vsync off. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "gl.h"
#include "game_hooks.h"
#include "gles_internal.h"
#include "ui.h"

int eagl_frame_rate(void);
void eagl_set_frame_rate(int fps);

typedef struct { u32 isa, api, sharegroup, multi; SDL_GLContext gl; u32 src_fbo, dst_fbo, tex; int tw, th; } Ctx;
#define CX(o) ((Ctx *)GP(o))

static __thread u32 cur;                  /* this thread's current EAGLContext */
static SDL_GLContext first_gl;
static u32 drawable_rb, drawable_layer;
static int rb_w, rb_h;
static u32 frames;
static int shots[64], nshots, quit_at;
static char shot_dir[MAX_PATH];

void eagl_window_created(void) {}
void eagl_drawable(u32 *rb, int *w, int *h) { *rb = drawable_rb, *w = rb_w, *h = rb_h; }
u32 eagl_frames(void) { return frames; }

static void r_nop(CPU *c) { (void)c; }

static id init_api(id self, u32 api, u32 share)
{
    if (api != 1 && api != 2) {
        rt_log("[eagl] EAGLContext with API %u refused\n", api);
        objc_release(self);
        return 0;
    }
    Ctx *x = CX(self);
    x->api = api;
    gles_api = (int)api;
    SDL_GLContext prev = gt_gl_current();
#if GT_GL_DESKTOP
    if (first_gl && prev != first_gl)
        SDL_GL_MakeCurrent(ui_window, first_gl);
    x->gl = SDL_GL_CreateContext(ui_window);
#else
    /* EGL (gles_android.c): later contexts share with the first without being made current to be made */
    x->gl = gla_create();
    if (x->gl && first_gl)
        gla_make_current(x->gl);        /* gles_context_created below sets up the new one */
#endif
    if (!x->gl)
        rt_fatal("OpenGL context: %s", SDL_GetError());
    if (!first_gl) {
        first_gl = x->gl;
        gles_load();
        const char *vs = getenv("GT_VSYNC");
#if GT_GL_DESKTOP
        SDL_GL_SetSwapInterval(vs && *vs == '0' ? 0 : 1);
#else
        /* uncapped (GT_FPS=0, Options > Display > Frame Rate): vsync holds it to the screen's refresh */
        gla_swap_interval(vs && *vs == '0' && eagl_frame_rate() ? 0 : 1);
#endif
    }
    gles_context_created();
#if GT_GL_DESKTOP
    SDL_GL_MakeCurrent(ui_window, prev);
#else
    gla_make_current(prev);
#endif
    x->sharegroup = share ? objc_retain(share) : ns_alloc(ns_class("EAGLSharegroup"), 8);
    rt_log("[eagl] EAGLContext 0x%08x: OpenGL ES %u%s\n", self, api, share ? " (shared)" : "");
    return self;
}
static void c_initWithAPI(CPU *c) { ret_i(c, init_api(SELF, ARG(0), 0)); }
static void c_initWithAPIShare(CPU *c) { ret_i(c, init_api(SELF, ARG(0), ARG(1))); }
static void c_setCurrent(CPU *c)
{
    id ctx = ARG(0), old = cur;
    if (ctx != old) {
        cur = objc_retain(ctx);
        objc_release(old);
    }
#if GT_GL_DESKTOP
    int ok = SDL_GL_MakeCurrent(ui_window, ctx ? CX(ctx)->gl : NULL);
#else
    int ok = gla_make_current(ctx ? CX(ctx)->gl : NULL);
#endif
    if (ok)
        gles_context_current(ctx ? CX(ctx)->gl : NULL);    /* the vertex state shadow follows the context */
    if (!ok || rt_trace)
        rt_log("[eagl] thread %lu: current context 0x%08x%s%s\n", GetCurrentThreadId(), ctx, ok ? "" : " FAILED: ",
               ok ? "" : SDL_GetError());
    ret_i(c, ok ? 1 : 0);
}
static void c_current(CPU *c) { ret_i(c, cur); }
static void c_api(CPU *c) { ret_i(c, CX(SELF)->api); }
static void c_sharegroup(CPU *c) { ret_i(c, CX(SELF)->sharegroup); }
static void c_setMulti(CPU *c) { CX(SELF)->multi = ARG(0) & 0xff; }
static void c_multi(CPU *c) { ret_i(c, CX(SELF)->multi); }
static void c_dealloc(CPU *c)
{
    Ctx *x = CX(SELF);
    if (x->gl && x->gl != first_gl) {
        gles_context_destroyed(x->gl);
#if GT_GL_DESKTOP
        if (gt_gl_current() == x->gl)
            SDL_GL_MakeCurrent(ui_window, NULL);
        SDL_GL_DestroyContext(x->gl);
#else
        gla_destroy(x->gl);
#endif
    }
    objc_release(x->sharegroup);
    objc_free_instance(SELF);
}

static void c_storage(CPU *c)
{
    /* renderbufferStorage:(target)fromDrawable:(id<EAGLDrawable>) */
    id layer = ARG(1);
    float w = UI_SCREEN_W, h = UI_SCREEN_H, s = 1;
    if (layer)
        ui_layer_size(layer, &w, &h, &s);
    /* rounded down, as the game sizes its own viewports from the layer (points x scale, the fraction dropped).
       Rounded to nearest, a 1365-point-wide screen at scale 1.40625 (GT_WIDE=monitor, GT_RESOLUTION=monitor on a
       1920 x 1080 monitor: 1919.53) got 1920 rows of which the game drew 1919; the last one, the picture's left
       column, kept stale light pixels (port fault: a thin column of stale pixels at the picture's left edge). */
    int pw = (int)(w * s + 0.001f), ph = (int)(h * s + 0.001f);
    u32 rb = 0;
    p_glGetIntegerv(GL_RENDERBUFFER_BINDING, GA(&rb));
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (u32)pw, (u32)ph);
    drawable_rb = rb;
    drawable_layer = layer;
    rb_w = pw;
    rb_h = ph;
    glx_drawable_changed(rb);
    rt_log("[eagl] renderbuffer %u gets the layer's storage: %d x %d (%g x %g points, scale %g)\n", rb, pw, ph, w, h, s);
    ret_i(c, 1);
}

/* presenting */

static void save_shot(int W, int H)
{
    u8 *px = malloc((size_t)W * H * 4), *flip = malloc((size_t)W * H * 4);
    p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    p_glReadPixels(0, 0, (u32)W, (u32)H, GL_RGBA, GL_UNSIGNED_BYTE, GA(px));
    for (int y = 0; y < H; y++)
        memcpy(flip + (size_t)y * W * 4, px + (size_t)(H - 1 - y) * W * 4, (size_t)W * 4);
    SDL_Surface *s = SDL_CreateSurfaceFrom(W, H, SDL_PIXELFORMAT_ABGR8888, flip, W * 4);
    char path[MAX_PATH + 32];
    CreateDirectoryA(shot_dir, NULL);
    snprintf(path, sizeof path, "%s" GT_SEP_S "frame%05u.bmp", shot_dir, frames);
    if (s && SDL_SaveBMP(s, path))
        rt_log("[eagl] saved %s\n", path);
    SDL_DestroySurface(s);
    free(px);
    free(flip);
}

/* GT_BENCH=a-b,c-d (testing, for the optimisation passes' before / after reports): for each span of presented
   frames a..b, one log line with its frame rate, the process's CPU
   time and the main thread's, as a share of one core.  A test route reaches the same frames in the same places, so
   two builds are measured on the same menus and the same race (at the 60 cap: the CPU each frame costs).
   GT_BENCH_UNCAP=1 (with GT_VSYNC=0) lifts the cap inside the spans only: the frame rate the CPU allows there, while
   the route around them keeps the 60 frames a second its frame numbers were planned at. */
static int bench_open;
static double ft_sec(FILETIME k, FILETIME u)
{
    return ((double)((u64)k.dwHighDateTime << 32 | k.dwLowDateTime) +
            (double)((u64)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 1e7;
}
static void bench_frame(void)
{
    static int n = -1, cur_span;
    static u32 span[8][2];
    static double t0, proc0, main0;
    if (n < 0) {
        n = 0;
        for (const char *s = dev_getenv("GT_BENCH"); s && *s && n < 8;) {
            char *e;
            span[n][0] = (u32)strtoul(s, &e, 10);
            if (*e != '-')
                break;
            span[n][1] = (u32)strtoul(e + 1, &e, 10);
            if (span[n][1] > span[n][0])
                n++;
            s = *e == ',' ? e + 1 : "";
        }
    }
    if (cur_span >= n)
        return;
    FILETIME a, b, k, u;
    if (frames == span[cur_span][0]) {
        t0 = ns_now();
        GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
        proc0 = ft_sec(k, u);
        GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u);
        main0 = ft_sec(k, u);
        const char *e = dev_getenv("GT_BENCH_UNCAP");
        bench_open = e && *e == '1';
        dev_log("[bench] frames %u-%u: started\n", span[cur_span][0], span[cur_span][1]);   /* for an outside profiler */
    } else if (frames == span[cur_span][1] && t0 > 0) {
        double dt = ns_now() - t0;
        GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
        double proc = ft_sec(k, u) - proc0;
        GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u);
        double mainc = ft_sec(k, u) - main0;
        rt_log("[bench] frames %u-%u: %.1f fps, process CPU %.0f %%, main thread %.0f %% of one core\n",
               span[cur_span][0], span[cur_span][1], (span[cur_span][1] - span[cur_span][0]) / dt,
               100 * proc / dt, 100 * mainc / dt);
        cur_span++;
        t0 = 0;
        bench_open = 0;
    }
}

/* GT_GL_STATS=1 (testing, pass 4): the time present() spends in its state queries, its attribute push / pop and
   the swap, every 600 frames (the queries make a threaded driver finish the frame's commands first) */
static int pt_on = -1;
#if !GT_GL_DESKTOP
/* the [perf] line's parts of a frame (Android) */
static double pf_end, pf_wait0, pf_work, pf_wait, pf_swap, pf_max;
static unsigned pf_n;
#endif
static LARGE_INTEGER pt_freq;
static double pt_acc[4];
static u32 pt_frames;
static double pt_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)pt_freq.QuadPart;
}
static void pt_add(int k, double t0) { if (GT_DEV && pt_on > 0) pt_acc[k] += pt_now() - t0; }

/* the frame cap (GT_FPS; Options > Display > Frame Rate on Android) */
static double next_t, period = -1;
static volatile int vsync_want = -1;    /* a swap interval for the next present to set, -1 none */

/* fps: 30, 60 or 0 (uncapped: vsync on, as fast as the screen refreshes) */
void eagl_set_frame_rate(int fps)
{
    period = fps > 0 ? 1.0 / fps : 0;
    next_t = 0;
    const char *vs = getenv("GT_VSYNC");
    vsync_want = fps > 0 ? (vs && *vs == '1') : 1;
    rt_log("[eagl] frame rate: %s\n", fps > 0 ? (fps == 30 ? "30" : "60") : "uncapped (the screen's refresh)");
}

int eagl_frame_rate(void)
{
    if (period < 0) {
        int fps = rt_setting_int("GT_FPS", 60, 0, 1000);
        period = fps > 0 ? 1.0 / fps : 0;
    }
    return period > 0 ? (int)(1.0 / period + 0.5) : 0;
}

static void present(Ctx *x)
{
    if (!drawable_rb || rb_w <= 0 || rb_h <= 0)
        return;
#if !GT_GL_DESKTOP
    {
        void gt_android_pin_frame_thread(void);
        gt_android_pin_frame_thread();  /* main.c PRIME_CORE */
    }
    if (!gla_present_begin()) {         /* Android: in the background (no surface), or another thread has the window */
        p_glFlush();
        frames++;
        gt_frame_end();
        ns_wait(0.1);                   /* nothing is shown: 10 frames a second, as BACKGROUND_THROTTLE minimized */
        return;
    }
#endif
    int W, H;
    SDL_GetWindowSizeInPixels(ui_window, &W, &H);
    u32 prog, rfbo, dfbo, rbb, active, abuf, ebuf, tex0;
    if (pt_on < 0) {
        pt_on = dev_getenv("GT_GL_STATS") && *dev_getenv("GT_GL_STATS") == '1';
        QueryPerformanceFrequency(&pt_freq);
    }
    double t0 = pt_on > 0 ? pt_now() : 0;
    /* QUERY_FREE (gles.c, pass 4): the state kept, not asked of the driver (each query made it finish the frame's
       commands first: 0.04-0.17 ms a frame) */
    gles_state(GL_CURRENT_PROGRAM, &prog);
    gles_state(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    gles_state(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
    gles_state(GL_RENDERBUFFER_BINDING, &rbb);
    gles_state(GL_ACTIVE_TEXTURE, &active);
    gles_state(GL_ARRAY_BUFFER_BINDING, &abuf);
    gles_state(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebuf);
    gles_state(GL_TEXTURE_BINDING_2D, &tex0);    /* unit 0's */
    p_glActiveTexture(GL_TEXTURE0);
    pt_add(0, t0);
    t0 = pt_on > 0 ? pt_now() : 0;
#if GT_GL_DESKTOP
    p_glPushAttrib(GL_ALL_ATTRIB_BITS);
    p_glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
#else
    gla_quad_save();
#endif
    pt_add(1, t0);
    p_glUseProgram(0);
    p_glDisable(GL_SCISSOR_TEST);
    p_glColorMask(1, 1, 1, 1);

    /* the renderbuffer into a texture of this context */
    if (!x->src_fbo) {
        p_glGenFramebuffers(1, GA(&x->src_fbo));
        p_glGenFramebuffers(1, GA(&x->dst_fbo));
    }
    if (!x->tex || x->tw != rb_w || x->th != rb_h) {
        if (!x->tex)
            p_glGenTextures(1, GA(&x->tex));
        p_glBindTexture(GL_TEXTURE_2D, x->tex);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (u32)rb_w, (u32)rb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gles_bind_fb(GL_FRAMEBUFFER, x->dst_fbo);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, x->tex, 0);
        x->tw = rb_w;
        x->th = rb_h;
    }
    u32 ms = glx_present_fbo();           /* MSAA (glenhance.c): its samples resolved by the blit */
    if (ms)
        gles_bind_fb(GL_READ_FRAMEBUFFER, ms);
    else {
        gles_bind_fb(GL_READ_FRAMEBUFFER, x->src_fbo);
        p_glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, drawable_rb);
    }
    gles_bind_fb(GL_DRAW_FRAMEBUFFER, x->dst_fbo);
    p_glBlitFramebuffer(0, 0, (u32)rb_w, (u32)rb_h, 0, 0, (u32)rb_w, (u32)rb_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
#if !GT_GL_DESKTOP
    {
        /* DISCARD (GT_DISCARD=1, the default): the frame's framebuffer told it is done with.  A phone's GPU draws
           the frame tile by tile in its own small memory and writes every attachment back to the phone's memory
           at the end -- the 4 samples of colour and of depth a pixel with MSAA, ~35 MB a frame at 1584 x 720 -- unless
           told the contents are no longer wanted.  They are not: the colour is copied above, the depth starts
           cleared, and the iPad's own drawable keeps nothing after a present either (RetainedBacking = 0). */
        static int on = -1;
        static void (*inval)(u32, int, const u32 *);
        if (on < 0) {
            const char *e = getenv("GT_DISCARD");
            inval = (void (*)(u32, int, const u32 *))SDL_GL_GetProcAddress("glInvalidateFramebuffer");
            on = !(e && *e == '0') && inval;
            rt_log("[gl] DISCARD: %s\n", on ? "on (the frame's samples and depth not written back)"
                                             : inval ? "off (GT_DISCARD=0)" : "off (no glInvalidateFramebuffer)");
        }
        if (on) {
            static const u32 att[3] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
            u32 glx_main_fbo(void);
            u32 game = ms ? ms : glx_main_fbo();
            if (game) {
                gles_bind_fb(GL_READ_FRAMEBUFFER, game);
                inval(GL_READ_FRAMEBUFFER, 3, att);
            }
        }
    }
#endif

    /* where the texture's corners are in the window: layer -> screen (portrait) -> landscape -> pixels */
    float lw = UI_SCREEN_W, lh = UI_SCREEN_H, ls = 1;
    GtAffine m = {1, 0, 0, 1, 0, 0};
    if (drawable_layer) {
        ui_layer_size(drawable_layer, &lw, &lh, &ls);
        id v = ui_layer_view(drawable_layer);
        if (v)
            ui_view_to_screen(v, &m);
    }
    float sc = fminf(W / UI_SCREEN_H, H / 768.0f), ox = (W - UI_SCREEN_H * sc) / 2, oy = (H - 768.0f * sc) / 2;
    float vx[4], vy[4];
    static const float st[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; i++) {
        float px = st[i][0] * lw, py = (1 - st[i][1]) * lh;                    /* layer points, y down */
        float sx = m.a * px + m.c * py + m.tx, sy = m.b * px + m.d * py + m.ty;   /* screen, portrait */
        float lx, ly;
        if (UI_ORIENTATION == 3)
            lx = sy, ly = UI_SCREEN_W - sx;
        else
            lx = UI_SCREEN_H - sy, ly = sx;
        float wx = ox + lx * sc, wy = oy + ly * sc;
        vx[i] = 2 * wx / W - 1;
        vy[i] = 1 - 2 * wy / H;
    }

#if GT_GL_DESKTOP
    gles_bind_fb(GL_FRAMEBUFFER, 0);
    p_glViewport(0, 0, (u32)W, (u32)H);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_STENCIL_TEST);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_ALPHA_TEST);
    p_glDisable(GL_FOG);
    p_glDisable(GL_LIGHTING);
    for (u32 u = 1; u < 4; u++) {
        p_glActiveTexture(GL_TEXTURE0 + u);
        p_glDisable(GL_TEXTURE_2D);
    }
    p_glActiveTexture(GL_TEXTURE0);
    p_glEnable(GL_TEXTURE_2D);
    p_glBindTexture(GL_TEXTURE_2D, x->tex);
    p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    p_glMatrixMode(GL_TEXTURE);
    p_glPushMatrix();
    p_glLoadIdentity();
    p_glMatrixMode(GL_PROJECTION);
    p_glPushMatrix();
    p_glLoadIdentity();
    p_glMatrixMode(GL_MODELVIEW);
    p_glPushMatrix();
    p_glLoadIdentity();
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glBegin(GL_QUADS);
    for (int i = 0; i < 4; i++) {
        p_glTexCoord2f(st[i][0], st[i][1]);
        p_glVertex2f(vx[i], vy[i]);
    }
    p_glEnd();
#else
    gla_quad_draw(x->tex, x->tw, x->th, vx, vy, W, H);
#if !GT_GL_DESKTOP
    {
        /* the frame rate, every 600 frames: with the low reads the fault handler did meanwhile (each a signal,
           ~10 us: a hot one costs frames) */
        extern volatile u32 gt_low_reads;
        static LARGE_INTEGER t_last;
        static u32 f_last, low_last;
        if (frames - f_last >= 600) {
            LARGE_INTEGER now, fq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&fq);
            if (t_last.QuadPart && pf_n)
                rt_log("[perf] %.1f fps over %u frames; a frame: game %.1f ms, wait %.1f, swap %.1f (longest %.1f); "
                       "low reads %u\n",
                       (double)(frames - f_last) * (double)fq.QuadPart / (double)(now.QuadPart - t_last.QuadPart),
                       frames - f_last, pf_work * 1000 / pf_n, pf_wait * 1000 / pf_n, pf_swap * 1000 / pf_n,
                       pf_max * 1000, gt_low_reads - low_last);
            t_last = now, f_last = frames, low_last = gt_low_reads;
            pf_work = pf_wait = pf_swap = pf_max = 0, pf_n = 0;
        }
    }
#endif
    if (frames < 3) {                            /* the first frames' picture path, for the log */
        u32 e = p_glGetError();
        gles_bind_fb(GL_READ_FRAMEBUFFER, ms ? ms : x->src_fbo);
        u32 st = p_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
        gles_bind_fb(GL_READ_FRAMEBUFFER, 0);
        rt_log("[eagl] frame %u: renderbuffer %u (%d x %d) -> window %d x %d, read fbo status 0x%x, GL error 0x%x\n",
               frames, drawable_rb, rb_w, rb_h, W, H, st, e);
    }
#endif
    frames++;
    for (int i = 0; i < nshots; i++)
        if (shots[i] == (int)frames)
            save_shot(W, H);
    /* the iPad shows at most 60 frames a second; faster screens would run the game faster than that
       (GT_FPS=n changes the cap, 0 = none) */
    if (period < 0) {
        int fps = rt_setting_int("GT_FPS", 60, 0, 1000);   /* 0: no cap */
        period = fps > 0 ? 1.0 / fps : 0;
    }
#if !GT_GL_DESKTOP
    if (vsync_want >= 0) {                /* Options > Display > Frame Rate: the window's surface is current here */
        gla_swap_interval(vsync_want);
        vsync_want = -1;
    }
#endif
    /* BACKGROUND_THROTTLE (uikit.c): behind other windows or minimized, a lower cap (also with GT_FPS=0) */
    int bg = ui_background_fps();
    double per = bg > 0 && (period <= 0 || period < 1.0 / bg) ? 1.0 / bg : period;
    if (GT_DEV && per > 0 && bench_open && !bg)
        next_t = 0;                       /* GT_BENCH_UNCAP: no cap in the span, the next capped frame starts afresh */
    else if (per > 0) {
        double now = ns_now();
#if !GT_GL_DESKTOP
        pf_wait0 = now;
#endif
        if (next_t == 0 || next_t < now - per)
            next_t = now;
        next_t += per;
        if (ns_precise_wait_on()) {
            /* PRECISE_WAIT (ns_misc.c): the timer is asked for 50 us before the deadline and only what is left
               under 0.1 ms is spun (optimisation pass 4: waking 0.2 ms early and spinning up to 0.3 ms cost ~1.5 % of a
               core at 60 fps, a good part of the render thread's own CPU at the cap). */
            for (double d; (d = next_t - ns_now()) > 0.0001;)
                ns_wait(d - 0.00005);
        } else {
            for (double d; (d = next_t - ns_now()) > 0.0015;)
                Sleep((DWORD)((d - 0.001) * 1000));
        }
        while (ns_now() < next_t)
            SwitchToThread();
    }
#if !GT_GL_DESKTOP
    double pf_wait1 = ns_now();
    if (pf_wait0 == 0)
        pf_wait0 = pf_wait1;
#endif
    t0 = pt_on > 0 ? pt_now() : 0;
    if (ui_minimized())
        p_glFlush();                      /* BACKGROUND_THROTTLE: nothing to show; the frame's work still goes out */
    else
#if GT_GL_DESKTOP
        SDL_GL_SwapWindow(ui_window);
#else
        gla_swap();
    {
        /* the frame's time in three parts, for the [perf] line: the game's work (the game's own code and its GL
           calls, from the last frame's swap to this frame's cap), the cap's wait, the swap; and the work and the
           swap told to the system (PERF_HINT, android_perf.c) */
        extern void gt_android_perf_frame(double work_s, double period_s);
        double end = ns_now();
        if (pf_end) {
            double work = pf_wait0 - pf_end, swap = end - pf_wait1;
            pf_work += work, pf_wait += pf_wait1 - pf_wait0, pf_swap += swap, pf_n++;
            if (work + swap > pf_max)
                pf_max = work + swap;
            if (!ui_background_fps())
                gt_android_perf_frame(work + swap, per);
        }
        pf_end = end, pf_wait0 = 0;
    }
#endif
    pt_add(2, t0);
    t0 = pt_on > 0 ? pt_now() : 0;
#if GT_GL_DESKTOP
    p_glMatrixMode(GL_MODELVIEW);
    p_glPopMatrix();
    p_glMatrixMode(GL_PROJECTION);
    p_glPopMatrix();
    p_glMatrixMode(GL_TEXTURE);
    p_glPopMatrix();
#endif

    /* the game's framebuffers first: the attributes hold their draw/read buffers (COLOR_ATTACHMENT0),
       which the default framebuffer would refuse */
    gles_bind_fb(GL_READ_FRAMEBUFFER, rfbo);
    gles_bind_fb(GL_DRAW_FRAMEBUFFER, dfbo);
#if GT_GL_DESKTOP
    p_glPopClientAttrib();
    p_glPopAttrib();
#else
    gla_quad_restore();
#endif
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, tex0);
    p_glActiveTexture(active);
    gles_bind_rb(rbb);
    p_glBindBuffer(GL_ARRAY_BUFFER, abuf);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebuf);
    p_glUseProgram(prog);
    pt_add(3, t0);
    if (GT_DEV && pt_on > 0 && ++pt_frames == 600) {
        rt_log("[gl] present, ms a frame: state queries %.3f, attribute push %.3f, swap %.3f, pop and restore %.3f\n",
               pt_acc[0] * 1000 / 600, pt_acc[1] * 1000 / 600, pt_acc[2] * 1000 / 600, pt_acc[3] * 1000 / 600);
        pt_frames = 0;
        memset(pt_acc, 0, sizeof pt_acc);
    }
    glx_frame_end();                      /* DISPLAY_MENU: anti-aliasing / anisotropic changed in the menu */
    gt_frame_end();                       /* the game's own end of frame work (display.c: Motion Blur) */
    wide_frame_start();                   /* WIDE_UI: the side bands of a frame with only menus */
    gles_state_forget();                  /* STATE_CACHE (gles.c): the host's own GL work above, never stale */

    if (frames == 1)
        rt_log("[eagl] first frame presented\n");
    bench_frame();
    /* GT_SPIKE_MS=n (testing): one line for each frame that took longer than n ms from the last present to this
       one, with its frame number, so a hitch (a menu opening, a load) can be placed against the log's other lines */
    static double spike_ms = -1, spike_t;
    if (spike_ms < 0) {
        const char *e = dev_getenv("GT_SPIKE_MS");
        spike_ms = e && *e ? atof(e) : 0;
    }
    if (GT_DEV && spike_ms > 0) {
        double now = ns_now();
        if (spike_t > 0 && (now - spike_t) * 1000 > spike_ms)
            rt_log("[spike] frame %u: %.1f ms\n", frames, (now - spike_t) * 1000);
        spike_t = now;
    }
#define FPS_LOG_MS 10000                /* the frame rate is logged this often */
    static u64 fps_t0;
    static u32 fps_f0;
    u64 now_ms = SDL_GetTicks();
    if (!fps_t0) {
        fps_t0 = now_ms;
        fps_f0 = frames;
    } else if (now_ms - fps_t0 >= FPS_LOG_MS) {
        rt_log("[eagl] %.1f frames a second\n", (frames - fps_f0) * 1000.0 / (double)(now_ms - fps_t0));
        fps_t0 = now_ms;
        fps_f0 = frames;
    }
    /* GT_WATCH=addr,off1,off2...: log the pointer chain's end when it changes (debugging) */
    static const char *watch;
    static u32 last_seen = 0xdeadbeef;
    if (!GT_DEV || !watch)
        watch = dev_getenv("GT_WATCH") ? dev_getenv("GT_WATCH") : "";
    if (*watch) {
        const char *s = watch;
        u32 v = (u32)strtoul(s, (char **)&s, 0);
        v = rd32(v);
        while (*s == ',' && v) {
            u32 off = (u32)strtoul(s + 1, (char **)&s, 0);
            v = rd32(v + off);
        }
        if (v != last_seen)
            rt_log("[watch] frame %u: %s = 0x%08x\n", frames, watch, v);
        last_seen = v;
    }
    if (quit_at && (int)frames >= quit_at)
        ui_quit = 1;
}

static void c_present(CPU *c)
{
    Ctx *x = CX(SELF);
    present(x);
    ret_i(c, 1);
}

void eagl_init(void)
{
    objc_host_size("EAGLContext", sizeof(Ctx));
    const char *s = dev_getenv("GT_SHOTS");
    while (s && *s && nshots < 64) {
        shots[nshots++] = atoi(s);
        s = strchr(s, ',');
        if (s)
            s++;
    }
    if (dev_getenv("GT_QUIT_AT"))
        quit_at = atoi(dev_getenv("GT_QUIT_AT"));
    GetModuleFileNameA(NULL, shot_dir, sizeof shot_dir);
    char *e = strrchr(shot_dir, GT_SEP);
    if (e)
        strcpy(e, GT_SEP_S "shots");
    if (dev_getenv("GT_SHOT_DIR") && *dev_getenv("GT_SHOT_DIR"))      /* testing: two runs at once */
        snprintf(shot_dir, sizeof shot_dir, "%s", dev_getenv("GT_SHOT_DIR"));
    static const NsMethod ctx[] = {
        {"initWithAPI:", c_initWithAPI, 0}, {"initWithAPI:sharegroup:", c_initWithAPIShare, 0},
        {"setCurrentContext:", c_setCurrent, 1}, {"currentContext", c_current, 1}, {"API", c_api, 0},
        {"sharegroup", c_sharegroup, 0}, {"setMultiThreaded:", c_setMulti, 0}, {"isMultiThreaded", c_multi, 0},
        {"dealloc", c_dealloc, 0}, {"renderbufferStorage:fromDrawable:", c_storage, 0},
        {"presentRenderbuffer:", c_present, 0}, {"setDebugLabel:", r_nop, 0}, {NULL, NULL, 0}};
    ns_methods("EAGLContext", ctx);
}
