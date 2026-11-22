/* gles_android.c -- EAGLContext's OpenGL ES contexts on Android's EGL, and present()'s picture with ES 3 only.
   Empty on Windows (eagl.c uses SDL's WGL contexts and desktop OpenGL there).

   Contexts.  On Windows two threads may have contexts current on the same window at once; EGL lets a window
   surface be current on one thread only (EGL_BAD_ACCESS for the second).  The game draws on its render thread and
   loads textures on another with a shared context, so a context is made current on the window when no other
   thread has the window, and without a surface (EGL_KHR_surfaceless_context) otherwise.  present() then makes
   sure its thread has the window (gla_present_begin).  The first context is SDL's (SDL_GL_CreateContext sets up
   EGL: display, config, window surface); the rest are made here, sharing with it.  An SDL_GLContext on SDL's EGL
   is the EGLContext itself, so both kinds are the same handles to the rest of the host (gt_gl_current).

   The surface.  Android takes the window's surface away while the app is in the background and SDL makes a new
   one when it comes back: present() binds whatever surface the window has now. */
#ifndef _WIN32
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_egl.h>
#include <stdio.h>
#include <string.h>

#include "plat.h"

#include "gl.h"
#include "gles_internal.h"
#include "ui.h"

static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLConfig cfg;
static EGLContext first = EGL_NO_CONTEXT;
static volatile LONG owner;                /* the thread id with the window's surface current, or 0 */

void *gt_gl_current(void) { return (void *)eglGetCurrentContext(); }

SDL_GLContext gla_create(void)
{
    if (first == EGL_NO_CONTEXT) {
        SDL_GLContext c = SDL_GL_CreateContext(ui_window);      /* current on this thread, with the window */
        if (!c)
            return NULL;
        dpy = (EGLDisplay)SDL_EGL_GetCurrentDisplay();
        cfg = (EGLConfig)SDL_EGL_GetCurrentConfig();
        first = (EGLContext)c;
        /* SDL keeps no context as current from here: its own pause handling (SDL_androidevents.c) saves the
           calling thread's SDL-current context, unbinds it and binds it again on resume, which would put the window
           on the event thread.  With none known to SDL that is a no-op, and present() binds the window's new
           surface itself after a resume. */
        EGLSurface s = (EGLSurface)SDL_EGL_GetWindowSurface(ui_window);
        SDL_GL_MakeCurrent(ui_window, NULL);
        eglMakeCurrent(dpy, s, s, first);
        InterlockedExchange(&owner, (LONG)GetCurrentThreadId());
        const char *ext = eglQueryString(dpy, EGL_EXTENSIONS);
        rt_log("[egl] first context %p; surfaceless contexts: %s\n", (void *)first,
               ext && strstr(ext, "EGL_KHR_surfaceless_context") ? "yes" : "NO (a second thread cannot draw)");
        return c;
    }
    static const EGLint at[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext c = eglCreateContext(dpy, cfg, first, at);
    if (c == EGL_NO_CONTEXT) {
        SDL_SetError("eglCreateContext: 0x%x", eglGetError());
        return NULL;
    }
    return (SDL_GLContext)c;
}

void gla_destroy(SDL_GLContext c)
{
    if (!c)
        return;
    if (eglGetCurrentContext() == (EGLContext)c)
        gla_make_current(NULL);
    if ((EGLContext)c == first)
        return;                            /* SDL's: kept for the run, as eagl.c keeps first_gl */
    eglDestroyContext(dpy, (EGLContext)c);
}

static EGLSurface window_surface(void)
{
    return ui_window ? (EGLSurface)SDL_EGL_GetWindowSurface(ui_window) : EGL_NO_SURFACE;
}

/* 1 on success.  The window's surface stays with the thread that presents (gla_present_begin takes it); every
   other context is made current without a surface.  The game draws into its own framebuffer objects only (an
   EAGL app has no default framebuffer), so only present() needs the window. */
int gla_make_current(SDL_GLContext c)
{
    if (dpy == EGL_NO_DISPLAY)
        return c == NULL;
    LONG me = (LONG)GetCurrentThreadId();
    if (!c) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        InterlockedCompareExchange(&owner, 0, me);
        return 1;
    }
    if (owner == me) {
        EGLSurface s = window_surface();
        if (s != EGL_NO_SURFACE && eglMakeCurrent(dpy, s, s, (EGLContext)c))
            return 1;
        InterlockedCompareExchange(&owner, 0, me);
    }
    if (eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, (EGLContext)c))
        return 1;
    SDL_SetError("eglMakeCurrent: 0x%x", eglGetError());
    return 0;
}

/* present(): this thread draws to the window now; 0 if it cannot (the app is in the background, or another thread
   has the window) */
int gla_present_begin(void)
{
    EGLSurface s = window_surface();
    if (s == EGL_NO_SURFACE)
        return 0;
    if (eglGetCurrentSurface(EGL_DRAW) == s)
        return 1;
    LONG me = (LONG)GetCurrentThreadId();
    if (InterlockedCompareExchange(&owner, me, 0) != 0 && owner != me) {
        static int said;
        if (!said++)
            rt_log("[egl] present on thread %ld while thread %ld has the window: frame not shown\n", (long)me,
                   (long)owner);
        return 0;
    }
    if (!eglMakeCurrent(dpy, s, s, eglGetCurrentContext())) {
        InterlockedCompareExchange(&owner, 0, me);
        return 0;
    }
    rt_log("[egl] the window's surface %p is current on thread %ld\n", (void *)s, (long)me);
    return 1;
}

void gla_swap(void)
{
    EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
    if (s != EGL_NO_SURFACE && !eglSwapBuffers(dpy, s)) {
        static int said;
        if (said++ < 5)
            rt_log("[egl] eglSwapBuffers: 0x%x\n", eglGetError());
    }
}

void gla_swap_interval(int n)
{
    if (dpy != EGL_NO_DISPLAY)
        eglSwapInterval(dpy, n);
}

/* present()'s quad: the frame's texture drawn over the window with a program and vertex array of its own; the
   state it changes is saved first and put back after (desktop OpenGL's glPushAttrib, which ES has not) */
static u32 q_prog, q_vao, q_vbo;
static struct {
    int cap[5];
    int vp[4];
    u8 mask[4];
    float clear[4];
} q_saved;
static const u32 q_caps[5] = {GL_DEPTH_TEST, GL_STENCIL_TEST, GL_BLEND, GL_CULL_FACE, GL_SCISSOR_TEST};

static u32 q_shader(u32 kind, const char *src)
{
    u32 sh = p_glCreateShader(kind);
    const char *one = src;
    p_glShaderSource(sh, 1, GA(&one), 0);
    p_glCompileShader(sh);
    int ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, GA(&ok));
    if (!ok) {
        char log[1024];
        int n = 0;
        p_glGetShaderInfoLog(sh, sizeof log, GA(&n), GA(log));
        rt_log("[egl] present's shader did not compile: %s\n", log);
    }
    return sh;
}

static void q_init(void)
{
    static const char vs[] =
        "#version 100\n"
        "attribute vec2 p;\nattribute vec2 t;\nvarying vec2 v;\n"
        "void main() { v = t; gl_Position = vec4(p, 0.0, 1.0); }\n";
    static const char fs[] =
        "#version 100\nprecision mediump float;\n"
        "uniform sampler2D s;\nvarying vec2 v;\n"
        "void main() { gl_FragColor = vec4(texture2D(s, v).rgb, 1.0); }\n";
    q_prog = p_glCreateProgram();
    p_glAttachShader(q_prog, q_shader(GL_VERTEX_SHADER, vs));
    p_glAttachShader(q_prog, q_shader(GL_FRAGMENT_SHADER, fs));
    p_glBindAttribLocation(q_prog, 0, GA("p"));
    p_glBindAttribLocation(q_prog, 1, GA("t"));
    p_glLinkProgram(q_prog);
    p_glUseProgram(q_prog);
    p_glUniform1i((u32)p_glGetUniformLocation(q_prog, GA("s")), 0);
    p_glGenVertexArrays(1, GA(&q_vao));
    p_glGenBuffers(1, GA(&q_vbo));
    p_glBindVertexArray(q_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, q_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), 0, GL_STREAM_DRAW);
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 4 * sizeof(float), 0);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, 0, 4 * sizeof(float), 2 * sizeof(float));
    p_glBindVertexArray(0);
}

void gla_quad_save(void)
{
    for (int i = 0; i < 5; i++)
        q_saved.cap[i] = (int)(p_glIsEnabled(q_caps[i]) & 0xff);
    p_glGetIntegerv(GL_VIEWPORT, GA(q_saved.vp));
    p_glGetBooleanv(GL_COLOR_WRITEMASK, GA(q_saved.mask));
    p_glGetFloatv(GL_COLOR_CLEAR_VALUE, GA(q_saved.clear));
}

/* the texture's corners at vx / vy (window coordinates, -1..1), the window W x H pixels */
void gla_quad_draw(u32 tex, const float *vx, const float *vy, int W, int H)
{
    static const float st[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    if (!q_prog)
        q_init();
    gles_bind_fb(GL_FRAMEBUFFER, 0);
    p_glViewport(0, 0, (u32)W, (u32)H);
    for (int i = 0; i < 5; i++)
        p_glDisable(q_caps[i]);
    p_glColorMask(1, 1, 1, 1);
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glUseProgram(q_prog);
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, tex);
    /* a strip: corners 0 1 3 2 */
    static const int order[4] = {0, 1, 3, 2};
    float v[16];
    for (int k = 0; k < 4; k++) {
        int i = order[k];
        v[k * 4 + 0] = vx[i];
        v[k * 4 + 1] = vy[i];
        v[k * 4 + 2] = st[i][0];
        v[k * 4 + 3] = st[i][1];
    }
    p_glBindVertexArray(q_vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, q_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof v, GA(v), GL_STREAM_DRAW);
    p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    p_glBindVertexArray(0);                /* the game's attribute arrays are VAO 0's: untouched */
}

void gla_quad_restore(void)
{
    for (int i = 0; i < 5; i++)
        (q_saved.cap[i] ? p_glEnable : p_glDisable)(q_caps[i]);
    p_glViewport((u32)q_saved.vp[0], (u32)q_saved.vp[1], (u32)q_saved.vp[2], (u32)q_saved.vp[3]);
    p_glColorMask(q_saved.mask[0], q_saved.mask[1], q_saved.mask[2], q_saved.mask[3]);
    p_glClearColor(q_saved.clear[0], q_saved.clear[1], q_saved.clear[2], q_saved.clear[3]);
}

#endif
