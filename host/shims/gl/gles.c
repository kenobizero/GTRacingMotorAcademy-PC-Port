/* gles.c -- the game's OpenGL ES 1.1 / 2.0 calls on desktop OpenGL 2.1 (compatibility profile).

   Most calls are the same function there (gl_list.h, passed straight through).  What differs:
   - glGetString says what an iPad's PowerVR SGX says (the game picks its paths by it);
   - GLSL ES shaders get a GLSL 1.20 header (precision qualifiers become nothing);
   - the OES framebuffer names are the core ones, glClearDepthf is glClearDepth;
   - PVRTC textures (glCompressedTexImage2D) are unpacked here to RGBA8;
   - a few ES-only glGet values and texture/renderbuffer formats are answered or mapped. */
#include <ctype.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "gl.h"
#include "game_hooks.h"
#include "gles_internal.h"
#include "liveblocks.h"
#include "ui.h"

#define GLP(name, params, args) T_##name p_##name;
#define GLR(ret, name, params, args) T_##name p_##name;
#define GLX(ret, name, params) T_##name p_##name;
#include "gl_list.h"

/* pass-throughs: h_<name> as the import bridges call them */
#define GLP(name, params, args) void h_##name params { p_##name args; }
#define GLR(ret, name, params, args) ret h_##name params { return p_##name args; }
#include "gl_list.h"

static int loaded;
static char gpu_vendor[128], gpu_renderer[128];

/* GT_GLDEBUG=1: the driver's error messages (KHR_debug), the first 200 */
#define GL_DEBUG_LOG_MAX 200            /* driver errors logged, then the rest are counted only */
static void GT_GLAPI gl_debug_cb(u32 source, u32 type, u32 id, u32 severity, int len, const char *msg, const void *user)
{
    (void)source, (void)id, (void)len, (void)user;
    static volatile LONG n;
    if (type != GL_DEBUG_TYPE_ERROR && severity != GL_DEBUG_SEVERITY_HIGH)
        return;
    if (InterlockedIncrement(&n) <= GL_DEBUG_LOG_MAX) {
        u32 fbo = 0, prog = 0, status = p_glCheckFramebufferStatus(GL_FRAMEBUFFER);
        p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, GA(&fbo));
        p_glGetIntegerv(GL_CURRENT_PROGRAM, GA(&prog));
        rt_log("[gl debug] %s (thread %lu, frame %u, fbo %u status 0x%x, program %u)\n", msg, GetCurrentThreadId(),
               eagl_frames(), fbo, status, prog);
    }
}
static void gl_debug_on(void)
{
    typedef void(GT_GLAPI * T_cb)(u32, u32, u32, u32, int, const char *, const void *);
    typedef void(GT_GLAPI * T_set)(T_cb, const void *);
    T_set set = (T_set)SDL_GL_GetProcAddress("glDebugMessageCallback");
    if (!set) {
        rt_log("[gl] no glDebugMessageCallback\n");
        return;
    }
    p_glEnable(GL_DEBUG_OUTPUT);
    p_glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    set(gl_debug_cb, NULL);
    rt_log("[gl] debug output on\n");
}

#if !GT_GL_DESKTOP
/* OpenGL ES 3 (Android): the desktop calls of gl_list.h the host makes, on ES 3 */
static void GT_GLAPI es_clear_depth(double d) { p_glClearDepthf((float)d); }
static u32 es_buffer_size(u32 target)
{
    int n = 0;
    p_glGetBufferParameteriv(target, GL_BUFFER_SIZE, GA(&n));
    return n > 0 ? (u32)n : 0;
}
/* glMapBuffer(OES): the whole buffer, its old contents kept unless only written (the game writes the buffer it
   maps; GL_WRITE_ONLY_OES is the only access OES_mapbuffer has) */
static u32 GT_GLAPI es_map_buffer(u32 target, u32 access)
{
    u32 n = es_buffer_size(target);
    if (!n)
        return 0;
    u32 bits = access == GL_READ_ONLY ? GL_MAP_READ_BIT :
               access == GL_READ_WRITE ? GL_MAP_READ_BIT | GL_MAP_WRITE_BIT : GL_MAP_WRITE_BIT;
    return p_glMapBufferRange(target, 0, n, bits);
}
static void GT_GLAPI es_get_buffer_sub_data(u32 target, u32 off, u32 n, u32 out)
{
    void *p = GP(p_glMapBufferRange(target, off, n, GL_MAP_READ_BIT));
    if (p) {
        memcpy(GP(out), p, n);
        p_glUnmapBuffer(target);
    } else
        memset(GP(out), 0, n);
}
/* ES 1.1's fixed-function calls (the game makes an OpenGL ES 2 context; they are imported, not drawn with) and
   the desktop state calls present() and PostFX used: nothing */
static void GT_GLAPI es_nothing(void) {}
static u32 nothing_calls;
static void GT_GLAPI es_nothing_logged(void)
{
    if (nothing_calls++ < 5)
        rt_log("[gl] an OpenGL ES 1.1 / desktop call this OpenGL ES 3 does not have was skipped\n");
}
static int es_standins(int missing)
{
    p_glClearDepth = es_clear_depth;
    p_glMapBuffer = es_map_buffer;
    p_glGetBufferSubData = es_get_buffer_sub_data;
    int filled = 0;
    /* every entry point still missing: a call that does nothing (the ARM calling convention lets a function of no
       arguments stand for any: the caller cleans up, the result in r0 is not used by these callers) */
#define FILL(name) if (!p_##name) { p_##name = (T_##name)(void *)es_nothing_logged; filled++; }
#define GLP(name, params, args) FILL(name)
#define GLR(ret, name, params, args) FILL(name)
#define GLX(ret, name, params) FILL(name)
#include "gl_list.h"
#undef FILL
    (void)es_nothing;
    rt_log("[gl] OpenGL ES: %d entry points of the desktop list have no ES counterpart (stand-ins)\n", filled);
    return missing - filled;
}
#endif

void gles_load(void)
{
    if (loaded)
        return;
    int missing = 0;
#define LOAD(name)                                                                    \
    p_##name = (T_##name)SDL_GL_GetProcAddress(#name);                                \
    if (!p_##name) {                                                                  \
        rt_log("[gl] %s is missing from this OpenGL\n", #name);                       \
        missing++;                                                                    \
    }
#define GLP(name, params, args) LOAD(name)
#define GLR(ret, name, params, args) LOAD(name)
#define GLX(ret, name, params) LOAD(name)
#include "gl_list.h"
#undef LOAD
#if !GT_GL_DESKTOP
    missing = es_standins(missing);
#endif
    loaded = 1;
    rt_log("[gl] %s / %s / OpenGL %s%s\n", (const char *)p_glGetString(GL_VENDOR),
           (const char *)p_glGetString(GL_RENDERER), (const char *)p_glGetString(GL_VERSION),
           missing ? " (entry points missing, see above)" : "");
    const char *v = (const char *)p_glGetString(GL_VENDOR), *r = (const char *)p_glGetString(GL_RENDERER);
    snprintf(gpu_vendor, sizeof gpu_vendor, "%s", v ? v : "");
    snprintf(gpu_renderer, sizeof gpu_renderer, "%s", r ? r : "");
    gpu_profile_start();               /* GPU_PROFILE (gpuprofile.c): the kind of GPU, on a thread of its own */
}

/* the driver's own names (GPU_PROFILE, gpuprofile.c); the game is told an iPad's (h_glGetString) */
const char *gles_gpu_vendor(void) { return gpu_vendor; }
const char *gles_gpu_renderer(void) { return gpu_renderer; }

/* each new context (current on this thread now) */
void gles_context_created(void)
{
    if (dev_getenv("GT_GLDEBUG"))
        gl_debug_on();
}

static void once(const char *what)
{
    static const char *seen[64];
    for (int i = 0; i < 64; i++) {
        if (seen[i] == what)
            return;
        if (!seen[i]) {
            seen[i] = what;
            rt_log("[gl] %s\n", what);
            return;
        }
    }
}

/* strings and state */

int gles_api = 2;                  /* the API of the last EAGLContext made (eagl.c) */

u32 h_glGetString(u32 name)
{
    static const char ext[] =
        "GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 GL_OES_mapbuffer GL_OES_element_index_uint "
        "GL_OES_framebuffer_object GL_OES_blend_func_separate GL_OES_texture_mirrored_repeat "
        "GL_IMG_texture_compression_pvrtc GL_EXT_texture_filter_anisotropic GL_EXT_read_format_bgra "
        "GL_APPLE_texture_2D_limited_npot GL_IMG_read_format GL_EXT_blend_minmax ";
    switch (name) {
    case GL_VENDOR:
        return GA("Imagination Technologies");
    case GL_RENDERER:
        return GA("PowerVR SGX 543");
    case GL_VERSION:
        return GA(gles_api == 1 ? "OpenGL ES-CM 1.1 IMGSGX543-63.14.2" : "OpenGL ES 2.0 IMGSGX543-63.14.2");
    case GL_SHADING_LANGUAGE_VERSION:
        return GA("OpenGL ES GLSL ES 1.00");
    case GL_EXTENSIONS:
        return GA(ext);
    }
    return p_glGetString(name);
}

void h_glGetIntegerv(u32 pname, u32 out)
{
    int *v = GP(out);
    switch (pname) {
#if GT_GL_DESKTOP
    case GL_MAX_VERTEX_UNIFORM_VECTORS:
        p_glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, out);
        *v /= 4;
        return;
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS:
        p_glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, out);
        *v /= 4;
        return;
    case GL_MAX_VARYING_VECTORS:
        p_glGetIntegerv(GL_MAX_VARYING_FLOATS, out);
        *v /= 4;
        return;
#endif
    case GL_IMPLEMENTATION_COLOR_READ_TYPE:
        *v = GL_UNSIGNED_BYTE;
        return;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT:
        *v = GL_RGBA;
        return;
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
        *v = 4;
        return;
    case GL_COMPRESSED_TEXTURE_FORMATS:
        v[0] = GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG, v[1] = GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG, v[2] = GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG, v[3] = GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG;
        return;
    case GL_SHADER_COMPILER:
        *v = 1;
        return;
    case GL_NUM_SHADER_BINARY_FORMATS:
        *v = 0;
        return;
    case GL_MAX_TEXTURE_SIZE:
        p_glGetIntegerv(pname, out);
        if (*v > 4096)
            *v = 4096;
        return;
    case GL_FRAMEBUFFER_BINDING: { /* GL_FRAMEBUFFER_BINDING: the game's main framebuffer, not glenhance.c's stand-in */
        p_glGetIntegerv(pname, out);
        *v = (int)glx_logical_fbo((u32)*v);
        return;
    }
    }
    p_glGetIntegerv(pname, out);
}

#if GT_GL_DESKTOP
void h_glClearDepthf(float d) { p_glClearDepth(d); }
#else
void h_glClearDepthf(float d) { p_glClearDepthf(d); }
#endif

u32 h_glMapBufferOES(u32 target, u32 access) { return p_glMapBuffer(target, access); }
u32 h_glUnmapBufferOES(u32 target) { return p_glUnmapBuffer(target) & 0xff; }

void h_glCurrentPaletteMatrixOES(u32 m) { (void)m; once("OES_matrix_palette (glCurrentPaletteMatrixOES) is not supported"); }
void h_glMatrixIndexPointerOES(u32 a, u32 b, u32 c_, u32 d) { (void)a, (void)b, (void)c_, (void)d; once("OES_matrix_palette (glMatrixIndexPointerOES) is not supported"); }
void h_glWeightPointerOES(u32 a, u32 b, u32 c_, u32 d) { (void)a, (void)b, (void)c_, (void)d; once("OES_matrix_palette (glWeightPointerOES) is not supported"); }

/* vertex arrays
   The generic attribute arrays are shadowed here (per thread = per context, each context stays on its thread) so
   that a draw can:
   - turn GL_FIXED (16.16) arrays in the game's memory into floats (desktop drivers take GL_FIXED only from
     buffers; from client memory it is a driver path nobody tests);
   - skip an enabled client array whose memory is gone (PowerVR never reads an array the program does not use;
     a desktop driver may copy every enabled one, and a stale pointer then faults inside the driver);
   - say what was bound when a draw faults (gles_fault_dump, from the fault handler). */
#define MAX_ATTR 16
typedef struct {
    u32 on, size, type, norm, stride, ptr, vbo;
    float *conv;       /* GL_FIXED from client memory: floats for this attribute */
    u32 conv_n;
} Attr;
typedef struct {
    Attr a[MAX_ATTR];
    u32 array_buf, elem_buf;
    u32 draw_mode, draw_count, draw_type, draw_ptr, draws;
    struct { u32 lo, hi; } ok[16];   /* memory known readable this frame (16, pass 3: a race's client arrays
                                        lie in more than 4 regions, VirtualQuery was 1.3 % of its render thread) */
    u32 ok_next, ok_frame;
    u32 del_seen;                    /* del_gen when this thread last checked its buffer bindings */
    u32 client;                      /* DRAW_FAST: bit i = attribute i enabled and read from client memory */
    u32 bufarr;                      /* MIXED_KEEP: bit i = attribute i enabled and read from a buffer object */
    /* STATE_CACHE (below): what this context has bound, SC_UNKNOWN when not known */
    u32 sc_unit, sc_tex[32], sc_prog, sc_bsrc, sc_bdst;
    u32 sc_tex_gen, sc_prog_gen;     /* tex_del_gen / prog_del_gen when this context last looked */
    u32 sc_caps_known, sc_caps_on;   /* glEnable / glDisable of the caps in cap_bit(), one bit each */
    u32 sc_dmask, sc_cmask;          /* glDepthMask, glColorMask (4 bits) */
    u32 sc_abuf_gen, sc_ebuf_gen;    /* buf_gen when array_buf / elem_buf were bound here, SC_UNKNOWN: not sure */
    u32 sc_arr_known, sc_arr_on;     /* ARRAY_CACHE (pass 6): glEnable/DisableVertexAttribArray, one bit an array */
    /* QUERY_FREE (below): this context's state as last set, kept for the host's own GL work; tk_known: bits TK_* */
    u32 tk_known, tk_prog, tk_unit, tk_tex0, tk_rfb, tk_dfb, tk_rb, tk_scissor_on, tk_box[4];
    float tk_clear[4];
    /* STREAM_VBO (below): this context's rings (vertices, indices), where the next copy goes in each, the context
       they were made in, and the arrays now pointing into the ring instead of the game's memory */
    u32 sv_ring, sv_iring, sv_off, sv_ioff, sv_moved;
    void *sv_ctx;
    /* STREAM_VBO's persistent rings (mapped once): their bytes, the quarter now written in each, a fence a quarter */
    unsigned char *sv_map[2];
    u32 sv_seg[2];
    void *sv_fence[2][4];
} VState;
enum { TK_PROG = 1, TK_UNIT = 2, TK_TEX0 = 4, TK_RFB = 8, TK_DFB = 16, TK_RB = 32, TK_SCISSOR_ON = 64, TK_BOX = 128,
       TK_CLEAR = 256 };
#define SC_UNKNOWN 0xffffffffu
static void sc_forget(VState *s)
{
    s->sc_unit = s->sc_prog = s->sc_bsrc = s->sc_bdst = SC_UNKNOWN;
    memset(s->sc_tex, 0xff, sizeof s->sc_tex);
    s->sc_caps_known = 0;
    s->sc_dmask = s->sc_cmask = SC_UNKNOWN;
    s->sc_abuf_gen = s->sc_ebuf_gen = SC_UNKNOWN;
    s->sc_arr_known = 0;
}
static VState *vstate_new(void)
{
    VState *s = calloc(1, sizeof *s);
    if (s)
        sc_forget(s);
    return s;
}
/* bumped by every glDeleteBuffers on any thread: the loader thread's shared context deletes buffers that the
   main thread's attribute arrays still name (a crash back to the menu after a race: attribute 3 at
   "buffer offset 0x18" read as memory 0x18 inside nvoglv32.dll).  PowerVR keeps the binding or ignores the
   unused array; the NVIDIA driver drops it, so each thread rechecks its bindings with glIsBuffer. */
static volatile LONG del_gen;
/* a delete (with its del_gen bump) and a draw (from check_deleted to the driver's draw) never overlap: taken
   shared by draws, exclusive by glDeleteBuffers (not by eagl.c's present: it waits for the frame and vsync,
   and would hold the loader's deletes to one a frame).  Without it the loader thread could
   delete a buffer after the main thread had checked its bindings and before it drew (or before del_gen moved),
   and the draw read the dropped array at its offset as memory: a crash starting a race with the
   Ferrari 288 GTO (attribute 3, stride 36, "buffer offset 0x18", read of 0x18 in nvoglv32.dll, on the loading
   screen's quad).  A port fault (PowerVR keeps the binding); not reproduced here, the window is short. */
static SRWLOCK del_lock = SRWLOCK_INIT;
static void gles_draw_lock(void) { AcquireSRWLockShared(&del_lock); }
static void gles_draw_unlock(void) { ReleaseSRWLockShared(&del_lock); }
/* The shadow belongs to a GL context, as the state it mirrors does, not to a thread.  It was once per thread.
   A thread that binds a buffer under one EAGLContext and then sets an attribute pointer under the
   other (the game's main and shared loader contexts) recorded "buffer offset 0x18".  The context really had no
   buffer bound, so that was a pointer to memory 0x18, and nothing was deleted, so no recheck ran.
   eagl.c's setCurrentContext: switches this thread to the context's shadow (gles_context_current). */
static __thread VState *vs_;
static struct { void *gl; VState *s; } ctx_vs[16];
static SRWLOCK ctx_lock = SRWLOCK_INIT;
void gles_context_current(void *gl)
{
    if (!gl)
        return;                  /* no context: GL calls do nothing, the shadow waits for the next one */
    VState *s = NULL;
    AcquireSRWLockExclusive(&ctx_lock);
    int i;
    for (i = 0; i < 16 && ctx_vs[i].gl; i++)
        if (ctx_vs[i].gl == gl) {
            s = ctx_vs[i].s;
            break;
        }
    if (!s && i < 16 && (s = vstate_new()) != NULL)
        ctx_vs[i].gl = gl, ctx_vs[i].s = s;
    ReleaseSRWLockExclusive(&ctx_lock);
    if (s && vs_ && vs_ != s) {
        static volatile LONG n;
        if (InterlockedIncrement(&n) <= 8)
            rt_log("[gl] thread %lu moves to another context's vertex state (context %p)\n", GetCurrentThreadId(),
                   gl);
    }
    if (s)
        vs_ = s;
}
void gles_context_destroyed(void *gl)
{
    AcquireSRWLockExclusive(&ctx_lock);
    for (int i = 0; i < 16; i++)
        if (ctx_vs[i].gl == gl)
            ctx_vs[i].gl = (void *)1;   /* the shadow is left (a thread may still point at it), the slot unused */
    ReleaseSRWLockExclusive(&ctx_lock);
}
static VState *vs(void)
{
    if (!vs_)
        gles_context_current(gt_gl_current());
    if (!vs_)
        vs_ = vstate_new();
    return vs_;
}

/* DRAW_FAST: s->client (and MIXED_KEEP's s->bufarr) follow attribute k's on / vbo */
static inline void client_set(VState *s, u32 k)
{
    u32 b = 1u << k;
    if (s->a[k].on && !s->a[k].vbo)
        s->client |= b;
    else
        s->client &= ~b;
    if (s->a[k].on && s->a[k].vbo)
        s->bufarr |= b;
    else
        s->bufarr &= ~b;
}

/* draws so far on this thread's context (widescreen.c: was anything drawn before the menus this frame) */
u32 gles_draws(void) { return vs()->draws; }

static u32 type_bytes(u32 t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES: return 2;   /* shorts, half floats */
    default: return 4;
    }
}

/* READABLE_STABLE (optimisation pass 4): memory that stays readable for the whole run is told
   without asking Windows: the exe's own image (the game's segments and data are in it; never unmapped) and the
   calling thread's guest stack (rt_core.c; freed only when the thread ends).  The rest (the heap, which can give
   pages back) is asked and kept for the frame only.  A profile of a race had VirtualQuery here at
   ~6 % of the render thread's own work.  GT_GL_STATS=1 counts the queries. */
static u32 img_lo, img_hi;
static unsigned rd_query, rd_image, rd_stack, rd_live;
static void image_range(void)
{
#ifndef _WIN32
    img_lo = GT_RESERVE_START;           /* Android: the game's range, mapped once by main (never unmapped) */
    img_hi = GT_MACHO_END;
#else
    const u8 *base = (const u8 *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    img_lo = (u32)(uintptr_t)base;
    img_hi = img_lo + nt->OptionalHeader.SizeOfImage;
#endif
}
static int stable(u32 a, u32 n)
{
    if (!img_hi)
        image_range();
    if (a >= img_lo && a < img_hi && n <= img_hi - a) {
        rd_image++;
        return 1;
    }
    RtThread *t = rt_thread_if();
    if (t && a >= t->stack_lo && a < t->stack_hi && n <= t->stack_hi - a) {
        rd_stack++;
        return 1;
    }
    return 0;
}

/* [a, a+n) committed and readable? Regions found are kept for the rest of the frame.
   LIVE_BLOCKS (pass 6, liveblocks.c): a range inside a heap block that is allocated now (malloc'd and not freed
   yet) is readable without asking Windows; a block freed since is no longer in the table, so its range is asked
   of Windows (and found gone if the heap gave its pages back). */
static int readable(VState *s, u32 a, u32 n)
{
    if (stable(a, n))
        return 1;
    if (gt_live_has(a, n)) {
        rd_live++;
        return 1;
    }
    u32 f = eagl_frames();
    if (s->ok_frame != f) {
        memset(s->ok, 0, sizeof s->ok);
        s->ok_frame = f;
    }
    u32 end = a + n;
    while (a < end) {
        int hit = 0;
        for (int i = 0; i < 16; i++)
            if (a >= s->ok[i].lo && a < s->ok[i].hi) {
                a = s->ok[i].hi;
                hit = 1;
                break;
            }
        if (hit)
            continue;
        MEMORY_BASIC_INFORMATION m;
        rd_query++;
        if (!VirtualQuery((void *)(uintptr_t)a, &m, sizeof m) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !m.Protect)
            return 0;
        u32 lo = (u32)(uintptr_t)m.BaseAddress, hi = lo + (u32)m.RegionSize;
        u32 slot = s->ok_next++ & 15;
        s->ok[slot].lo = lo;
        s->ok[slot].hi = hi;
        a = hi;
    }
    return 1;
}

/* STATE_CACHE (below) */
enum { SC_TEX, SC_PROG, SC_BLEND, SC_CAP, SC_MASK, SC_BUF, SC_UNI, SC_ARR, SC_KINDS };
static int sc_enabled(void);
static u32 sc_calls[SC_KINDS], sc_skipped[SC_KINDS];
/* bumped by every glDeleteBuffers and glGenBuffers on any thread: GL may hand a deleted buffer's name out again,
   and a context that still has the old one bound must bind the new one for real (STATE_CACHE) */
static volatile LONG buf_gen;

/* STATE_CACHE (optimisation pass 3): a bind of the buffer this context already has bound is not passed on, while
   no buffer was deleted or made anywhere since that bind */
void h_glBindBuffer(u32 target, u32 id)
{
    VState *s = vs();
    u32 g = (u32)buf_gen;
    if (target == GL_ARRAY_BUFFER) {
        sc_calls[SC_BUF]++;
        if (s->array_buf == id && s->sc_abuf_gen == g && sc_enabled()) {
            sc_skipped[SC_BUF]++;
            return;
        }
        s->array_buf = id, s->sc_abuf_gen = g;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        sc_calls[SC_BUF]++;
        if (s->elem_buf == id && s->sc_ebuf_gen == g && sc_enabled()) {
            sc_skipped[SC_BUF]++;
            return;
        }
        s->elem_buf = id, s->sc_ebuf_gen = g;
    }
    p_glBindBuffer(target, id);
}
void h_glGenBuffers(u32 n, u32 ids)
{
    p_glGenBuffers(n, ids);
    InterlockedIncrement(&buf_gen);
}
/* DEFER_DELETE: a buffer deleted on one context while another still uses it
   Why.  The game's loader thread (the shared EAGLContext) deletes buffers that the main context still has bound:
   an enabled attribute array or the element buffer.  GL says such a buffer lives on until no context has it bound
   any more (only the deleting context's own bindings are dropped), and on the iPad it does.  The desktop
   driver here does not keep it (the main context's array became "buffer offset 0x18" read as memory 0x18 inside
   nvoglv32.dll, see del_gen), so check_deleted had to leave such arrays out of the draws: geometry that used them
   lost an attribute for as long as the game did not set it again.  A port fault (the driver's behaviour across
   shared contexts), not the game's.
   What.  glDeleteBuffers drops the deleting context's own bindings at once, as GL does; a buffer that another
   context still has bound (an enabled attribute array, the array or the element buffer) is not passed to the
   driver yet but kept on a list, and deleted at a later glDeleteBuffers or present once no context names it
   (each context's shadow says what it has bound).  Its name stays taken until then, so glGenBuffers cannot hand
   it out again meanwhile.  At most DD_MAX are kept; past that a delete goes straight through.
   GT_GL_DEFER_DELETE=0: every delete straight to the driver (check_deleted still guards the draws). */
#define DD_MAX 256
static u32 dd_names[DD_MAX];
static int dd_n, dd_on = -1;
static unsigned dd_kept, dd_freed;      /* GT_GL_STATS: deletes held back, held ones deleted since */
static int dd_enabled(void)
{
    if (dd_on < 0) {
        const char *e = getenv("GT_GL_DEFER_DELETE");
        dd_on = !(e && *e == '0');
        rt_log("[gl] DEFER_DELETE: %s\n", dd_on ? "on" : "off (GT_GL_DEFER_DELETE=0)");
    }
    return dd_on;
}
/* does a context other than skip (NULL: any) have buffer id bound? */
static int dd_in_use(const VState *skip, u32 id)
{
    int used = 0;
    AcquireSRWLockShared(&ctx_lock);
    for (int i = 0; i < 16 && ctx_vs[i].gl && !used; i++) {
        const VState *o = ctx_vs[i].s;
        if (ctx_vs[i].gl == (void *)1 || !o || o == skip)   /* (void *)1: a destroyed context's slot */
            continue;
        if (o->array_buf == id || o->elem_buf == id)
            used = 1;
        for (u32 k = 0; k < MAX_ATTR && !used; k++)
            if (o->a[k].on && o->a[k].vbo == id)
                used = 1;
    }
    ReleaseSRWLockShared(&ctx_lock);
    return used;
}
/* the kept buffers no context names any more: to the driver (caller holds del_lock exclusive, a context current) */
static void dd_sweep_locked(void)
{
    int w = 0, freed = 0;
    for (int r = 0; r < dd_n; r++) {
        u32 id = dd_names[r];
        if (dd_in_use(NULL, id))
            dd_names[w++] = id;
        else {
            p_glDeleteBuffers(1, GA(&id));
            freed++;
        }
    }
    dd_n = w;
    if (freed) {
        dd_freed += (unsigned)freed;
        InterlockedIncrement(&del_gen);
        InterlockedIncrement(&buf_gen);
    }
}
/* at each present (gles_state_forget) */
static void dd_sweep(void)
{
    if (!dd_n || !gt_gl_current())
        return;
    AcquireSRWLockExclusive(&del_lock);
    dd_sweep_locked();
    ReleaseSRWLockExclusive(&del_lock);
}

void h_glDeleteBuffers(u32 n, u32 ids)
{
    VState *s = vs();
    const u32 *v = GP(ids);
    int defer = v && dd_enabled();
    u32 now[64], keep[64];
    u32 nn = 0, nk = 0;
    for (u32 i = 0; v && i < n; i++) {
        if (v[i] && v[i] == s->array_buf) {
            s->array_buf = 0;
            if (defer)
                p_glBindBuffer(GL_ARRAY_BUFFER, 0);            /* what the driver's delete would do to this context */
        }
        if (v[i] && v[i] == s->elem_buf) {
            s->elem_buf = 0;
            if (defer)
                p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
        /* an attribute array in this buffer: GL resets its binding to 0, so its offset becomes a pointer into
           client memory (0x18 ...) that the driver reads if the array is still enabled, even when the program
           does not use it (PowerVR does not).  Shadow that, so draw_prepare leaves it out of the draws. */
        for (u32 k = 0; v[i] && k < MAX_ATTR; k++)
            if (s->a[k].vbo == v[i]) {
                s->a[k].vbo = 0;
                client_set(s, k);
                if (s->a[k].on)
                    once("glDeleteBuffers on the buffer of an enabled attribute array: left out of draws until set again");
            }
    }
    AcquireSRWLockExclusive(&del_lock);
    if (!defer || n > 64) {
        p_glDeleteBuffers(n, ids);
        InterlockedIncrement(&del_gen);
        InterlockedIncrement(&buf_gen);
    } else {
        for (u32 i = 0; i < n; i++) {
            if (!v[i])
                continue;
            if (dd_n < DD_MAX && dd_in_use(s, v[i]))
                keep[nk++] = v[i];
            else
                now[nn++] = v[i];
        }
        for (u32 i = 0; i < nk; i++)
            dd_names[dd_n++] = keep[i];
        if (nk) {
            dd_kept += nk;
            static volatile LONG logged;
            if (InterlockedIncrement(&logged) <= 4)
                rt_log("[gl] DEFER_DELETE: buffer %u deleted on thread %lu while another context still has it bound: "
                       "kept until none does (%d kept now)\n", keep[0], GetCurrentThreadId(), dd_n);
        }
        if (nn) {
            p_glDeleteBuffers(nn, GA(now));
            InterlockedIncrement(&del_gen);
            InterlockedIncrement(&buf_gen);
        }
        dd_sweep_locked();
    }
    ReleaseSRWLockExclusive(&del_lock);
}

/* a buffer deleted on another thread since this thread last looked: drop the bindings that name it.
   The driver is asked what this context still has bound (not glIsBuffer alone): the loader thread deletes a
   buffer and makes a new one, GL hands out the same name again, glIsBuffer says yes, but this context's
   binding was already dropped.  That hole let the crash above back (menu after a race, iPad 3
   mode with POSTFX: attribute 3 "buffer offset 0x18" read as memory 0x18 inside nvoglv32.dll).  The queries
   run only after a delete somewhere, i.e. while loading, so draws pay nothing. */
static u32 gl_int(u32 pname)
{
    u32 v = 0;
    p_glGetIntegerv(pname, GA(&v));
    return v;
}
static void check_deleted(VState *s)
{
    u32 g = (u32)del_gen;
    if (s->del_seen == g)
        return;
    s->del_seen = g;
    for (u32 k = 0; k < MAX_ATTR; k++) {
        if (!s->a[k].vbo)
            continue;
        u32 b = 0;
        p_glGetVertexAttribiv(k, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, GA(&b));
        if (!b || b != s->a[k].vbo || !(p_glIsBuffer(b) & 0xff)) {
            if (s->a[k].on)
                once("a buffer deleted on another thread was still an enabled attribute array here: left out of draws until set again");
            s->a[k].vbo = 0;
            client_set(s, k);
        }
    }
    s->array_buf = gl_int(GL_ARRAY_BUFFER_BINDING);
    s->elem_buf = gl_int(GL_ELEMENT_ARRAY_BUFFER_BINDING);
    s->sc_abuf_gen = s->sc_ebuf_gen = SC_UNKNOWN;      /* the next binds go to the driver (STATE_CACHE) */
}
/* ARRAY_CACHE (optimisation pass 6: STATE_CACHE extended to
   glEnableVertexAttribArray): the game enables its attribute arrays again before every draw, 76-228 calls a frame
   in a race, ~99 % of them for an array already on (measured on a test save, France Raceway, AMD).  Not
   a fault of the game (on PowerVR the call is cheap) or of the port; each one is still a trip into the driver.
   Like STATE_CACHE: a call that changes nothing is not passed on.  What keeps the shadow (sc_arr_known /
   sc_arr_on, per GL context like the state it mirrors) true:
   - the only other places that change these switches are draw_prepare / draw_done here (an unreadable array is
     switched off for one draw and on again after it: the same state at the end) and eagl.c's present, which
     saves and restores them (glPush/PopClientAttrib) and is followed by gles_state_forget;
   - the game's ES 1 glEnableClientState / glDisableClientState (attribute 0 is the vertex array on some
     drivers) forget the shadow;
   - an array not known yet is passed on.
   GT_GL_ARRAY_CACHE=0 (or GT_GL_STATE_CACHE=0): every call to the driver. */
static int ac_on = -1;
static int ac_enabled(void)
{
    if (ac_on < 0) {
        const char *e = getenv("GT_GL_ARRAY_CACHE");
        int want = !(e && *e == '0');
        ac_on = want && sc_enabled();
        rt_log("[gl] ARRAY_CACHE: %s\n", ac_on ? "on" : want ? "off (GT_GL_STATE_CACHE=0)" : "off (GT_GL_ARRAY_CACHE=0)");
    }
    return ac_on;
}
static void array_set(u32 i, int on)
{
    if (i < MAX_ATTR) {
        VState *s = vs();
        s->a[i].on = on != 0;
        client_set(s, i);
        u32 b = 1u << i;
        sc_calls[SC_ARR]++;
        if ((s->sc_arr_known & b) && !(s->sc_arr_on & b) == !on && ac_enabled()) {
            sc_skipped[SC_ARR]++;
            return;
        }
        s->sc_arr_known |= b;
        s->sc_arr_on = on ? s->sc_arr_on | b : s->sc_arr_on & ~b;
    }
    if (on)
        p_glEnableVertexAttribArray(i);
    else
        p_glDisableVertexAttribArray(i);
}
void h_glEnableVertexAttribArray(u32 i) { array_set(i, 1); }
void h_glDisableVertexAttribArray(u32 i) { array_set(i, 0); }
void h_glEnableClientState(u32 a)
{
    vs()->sc_arr_known = 0;
    p_glEnableClientState(a);
}
void h_glDisableClientState(u32 a)
{
    vs()->sc_arr_known = 0;
    p_glDisableClientState(a);
}
void h_glVertexAttribPointer(u32 index, u32 size, u32 type, u32 norm, u32 stride, u32 ptr)
{
    VState *s = vs();
    norm &= 0xff;
    check_deleted(s);
    if (index < MAX_ATTR) {
        Attr *a = &s->a[index];
        a->size = size, a->type = type, a->norm = norm, a->stride = stride, a->ptr = ptr, a->vbo = s->array_buf;
        client_set(s, index);
        s->sv_moved &= ~(1u << index);   /* STREAM_VBO: the game's own pointer from now on */
        if (type == GL_FIXED && !a->vbo) {
            once("GL_FIXED vertex attributes from client memory: turned into floats at each draw");
            return;                /* set at the draw */
        }
    }
    p_glVertexAttribPointer(index, size, type, norm, stride, ptr);
}

/* an attribute array left out of a draw: logged once per kind (attribute, pointer, layout), then at its 10th, 100th
   ... time (optimisation pass 4): a fixed first 20 of the run would all be alike after a replay (a buffer the
   loader deleted) and hide the later kinds. */
static void unreadable_note(u32 i, const Attr *a, u32 first, u32 last, u32 bufs)
{
    enum { KINDS = 64 };
    static struct { u32 attr, ptr, size, type, stride, n; } kind[KINDS];
    static int nkinds;
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    int k = 0;
    while (k < nkinds && !(kind[k].attr == i && kind[k].ptr == a->ptr && kind[k].size == a->size &&
                           kind[k].type == a->type && kind[k].stride == a->stride))
        k++;
    u32 n = 0;
    if (k < nkinds)
        n = ++kind[k].n;
    else if (nkinds < KINDS) {
        kind[k].attr = i, kind[k].ptr = a->ptr, kind[k].size = a->size, kind[k].type = a->type;
        kind[k].stride = a->stride, kind[k].n = n = 1;
        nkinds++;
    }
    ReleaseSRWLockExclusive(&lock);
    if (n == 1)
        rt_log("[gl] draw: attribute %u's array at 0x%08x (%u x %u, type 0x%x, stride %u) is not readable for vertices "
               "%u..%u: left out of this draw (frame %u; repeats counted; arrays from buffers in it: 0x%x)\n", i,
               a->ptr, a->size, type_bytes(a->type), a->type, a->stride, first, last, eagl_frames(), bufs);
    else if (n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        rt_log("[gl] draw: attribute %u's array at 0x%08x left out %u times so far (frame %u)\n", i, a->ptr, n,
               eagl_frames());
    else if (!n)
        once("more kinds of unreadable attribute arrays than are counted: left out of draws, not logged");
}

/* FIXED_FAST and INDEX_FAST (optimisation pass 6)
   Why.  Two loops of the draw path ran a value at a time: the GL_FIXED (16.16) arrays turned into floats at each draw
   (7-25 thousand vertices a frame in a race, 50-250 us of the render thread at the 60 fps cap) and the scan of a
   draw's indices for the vertex range it reads (33-95 thousand indices a frame).  Measured on a test save
   (France Raceway, AMD built-in GPU).  Neither is a fault of the game or of the port, only slow.
   What.  FIXED_FAST: four values at a time with SSE2 (cvtdq2ps then a multiply by 2^-16, the same two roundings as
   the plain loop, so the same floats bit for bit; --selftest compares them), and with STREAM_VBO's mapped ring the
   floats are written straight into the ring instead of into the array's float copy and then copied there.
   INDEX_FAST: the smallest and largest index 8 (16-bit) or 16 (8-bit) at a time with SSE2 (--selftest compares
   them with the plain loop).  Reads stay inside the arrays: a 3-value vertex is read as 8 + 4 bytes, the tail of the
   indices one at a time.
   GT_FIXED_FAST=0 / GT_INDEX_FAST=0: the plain loops (FIXED_FAST=0 also copies through the float copy). */
static int fx_on = -1, ix_on = -1;
static int fx_enabled(void)
{
    if (fx_on < 0) {
        const char *e = getenv("GT_FIXED_FAST");
        fx_on = !(e && *e == '0');
        rt_log("[gl] FIXED_FAST: %s\n", fx_on ? "on" : "off (GT_FIXED_FAST=0)");
    }
    return fx_on;
}
static int ix_enabled(void)
{
    if (ix_on < 0) {
        const char *e = getenv("GT_INDEX_FAST");
        ix_on = !(e && *e == '0');
        rt_log("[gl] INDEX_FAST: %s\n", ix_on ? "on" : "off (GT_INDEX_FAST=0)");
    }
    return ix_on;
}
static u32 fx_verts, fx_direct;          /* GT_GL_STATS: GL_FIXED vertices turned into floats, those into the ring */

/* n vertices of `size` 16.16 values, `stride` bytes apart from src, into d (size floats a vertex) */
static void fixed_plain(float *d, const u8 *src, u32 n, u32 size, u32 stride)
{
    for (u32 v = 0; v < n; v++, src += stride, d += size)
        for (u32 k = 0; k < size; k++)
            d[k] = ((const int *)src)[k] * (1.0f / 65536.0f);
}
#if defined(__SSE2__)
static void fixed_sse2(float *d, const u8 *src, u32 n, u32 size, u32 stride)
{
    const __m128 k = _mm_set1_ps(1.0f / 65536.0f);
    u32 v = 0;
    switch (size) {
    case 4:
        for (; v < n; v++, src += stride, d += 4)
            _mm_storeu_ps(d, _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i *)src)), k));
        return;
    case 3:
        for (; v < n; v++, src += stride, d += 3) {
            __m128i x = _mm_loadl_epi64((const __m128i *)src);
            x = _mm_unpacklo_epi64(x, _mm_cvtsi32_si128(((const int *)src)[2]));
            __m128 f = _mm_mul_ps(_mm_cvtepi32_ps(x), k);
            _mm_storel_pi((__m64 *)d, f);
            _mm_store_ss(d + 2, _mm_movehl_ps(f, f));
        }
        return;
    case 2:
        for (; v < n; v++, src += stride, d += 2)
            _mm_storel_pi((__m64 *)d, _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadl_epi64((const __m128i *)src)), k));
        return;
    }
    fixed_plain(d, src, n, size, stride);
}
#elif defined(__ARM_NEON)
/* Android (armeabi-v7a, NEON): vcvt's 16 fraction bits are the same single rounding as int -> float * 2^-16 */
static void fixed_sse2(float *d, const u8 *src, u32 n, u32 size, u32 stride)
{
    u32 v = 0;
    switch (size) {
    case 4:
        for (; v < n; v++, src += stride, d += 4)
            vst1q_f32(d, vcvtq_n_f32_s32(vld1q_s32((const int32_t *)(const void *)src), 16));
        return;
    case 2:
        for (; v < n; v++, src += stride, d += 2)
            vst1_f32(d, vcvt_n_f32_s32(vld1_s32((const int32_t *)(const void *)src), 16));
        return;
    }
    fixed_plain(d, src, n, size, stride);
}
#else
static void fixed_sse2(float *d, const u8 *src, u32 n, u32 size, u32 stride) { fixed_plain(d, src, n, size, stride); }
#endif
static void fixed_convert(float *d, const u8 *src, u32 n, u32 size, u32 stride)
{
    fx_verts += n;
    if (fx_enabled())
        fixed_sse2(d, src, n, size, stride);
    else
        fixed_plain(d, src, n, size, stride);
}

/* the smallest and largest of count indices of n bytes each */
static void index_plain(const u8 *p, u32 count, u32 n, u32 *mn_, u32 *mx_)
{
    u32 mn = 0xffffffff, mx = 0;
    for (u32 i = 0; i < count; i++) {
        u32 v = n == 1 ? p[i] : n == 2 ? ((const unsigned short *)p)[i] : ((const u32 *)p)[i];
        if (v < mn)
            mn = v;
        if (v > mx)
            mx = v;
    }
    *mn_ = mn, *mx_ = mx;
}
#if defined(__SSE2__)
static void index_sse2(const u8 *p, u32 count, u32 n, u32 *mn_, u32 *mx_)
{
    u32 mn = 0xffffffff, mx = 0, i = 0;
    if (n == 2 && count >= 16) {         /* signed 16-bit min / max on the values with their top bit flipped */
        const __m128i flip = _mm_set1_epi16((short)0x8000);
        __m128i lo = _mm_set1_epi16(0x7fff), hi = flip;
        for (; i + 8 <= count; i += 8) {
            __m128i x = _mm_xor_si128(_mm_loadu_si128((const __m128i *)(p + i * 2)), flip);
            lo = _mm_min_epi16(lo, x);
            hi = _mm_max_epi16(hi, x);
        }
        unsigned short a[8], b[8];
        _mm_storeu_si128((__m128i *)a, _mm_xor_si128(lo, flip));
        _mm_storeu_si128((__m128i *)b, _mm_xor_si128(hi, flip));
        for (int k = 0; k < 8; k++) {
            if (a[k] < mn)
                mn = a[k];
            if (b[k] > mx)
                mx = b[k];
        }
    } else if (n == 1 && count >= 32) {
        __m128i lo = _mm_set1_epi8((char)0xff), hi = _mm_setzero_si128();
        for (; i + 16 <= count; i += 16) {
            __m128i x = _mm_loadu_si128((const __m128i *)(p + i));
            lo = _mm_min_epu8(lo, x);
            hi = _mm_max_epu8(hi, x);
        }
        u8 a[16], b[16];
        _mm_storeu_si128((__m128i *)a, lo);
        _mm_storeu_si128((__m128i *)b, hi);
        for (int k = 0; k < 16; k++) {
            if (a[k] < mn)
                mn = a[k];
            if (b[k] > mx)
                mx = b[k];
        }
    }
    u32 tmn, tmx;                        /* the rest (and 32-bit indices) one at a time */
    index_plain(p + i * n, count - i, n, &tmn, &tmx);
    *mn_ = tmn < mn ? tmn : mn, *mx_ = tmx > mx ? tmx : mx;
}
#elif defined(__ARM_NEON)
static void index_sse2(const u8 *p, u32 count, u32 n, u32 *mn_, u32 *mx_)
{
    u32 mn = 0xffffffff, mx = 0, i = 0;
    if (n == 2 && count >= 16) {
        uint16x8_t lo = vdupq_n_u16(0xffff), hi = vdupq_n_u16(0);
        for (; i + 8 <= count; i += 8) {
            uint16x8_t x = vld1q_u16((const uint16_t *)(const void *)(p + i * 2));
            lo = vminq_u16(lo, x);
            hi = vmaxq_u16(hi, x);
        }
        uint16_t a[8], b[8];
        vst1q_u16(a, lo);
        vst1q_u16(b, hi);
        for (int k = 0; k < 8; k++) {
            if (a[k] < mn)
                mn = a[k];
            if (b[k] > mx)
                mx = b[k];
        }
    } else if (n == 1 && count >= 32) {
        uint8x16_t lo = vdupq_n_u8(0xff), hi = vdupq_n_u8(0);
        for (; i + 16 <= count; i += 16) {
            uint8x16_t x = vld1q_u8(p + i);
            lo = vminq_u8(lo, x);
            hi = vmaxq_u8(hi, x);
        }
        u8 a[16], b[16];
        vst1q_u8(a, lo);
        vst1q_u8(b, hi);
        for (int k = 0; k < 16; k++) {
            if (a[k] < mn)
                mn = a[k];
            if (b[k] > mx)
                mx = b[k];
        }
    }
    u32 tmn, tmx;
    index_plain(p + i * n, count - i, n, &tmn, &tmx);
    *mn_ = tmn < mn ? tmn : mn, *mx_ = tmx > mx ? tmx : mx;
}
#else
static void index_sse2(const u8 *p, u32 count, u32 n, u32 *mn_, u32 *mx_) { index_plain(p, count, n, mn_, mx_); }
#endif

/* --selftest (selftest.c): FIXED_FAST and INDEX_FAST against the plain loops, on random values (the ends of the
   ranges among them), every size, stride and length kind; the number of differences */
int gles_selftest_fixed(void)
{
    enum { NV = 67, MAXS = 24 };
    static int src[NV * MAXS / 4 + 8];
    static float a[NV * 4 + 4], b[NV * 4 + 4];
    u32 x = 0x12345678u;
    int bad = 0;
    for (u32 i = 0; i < sizeof src / sizeof src[0]; i++) {
        u32 r = st_rand(&x);
        src[i] = i % 11 == 0 ? (int)0x80000000u : i % 11 == 1 ? 0x7fffffff : i % 11 == 2 ? (int)(r & 0xff) - 128 : (int)r;
    }
    for (u32 size = 1; size <= 4; size++)
        for (u32 stride = size * 4; stride <= MAXS; stride += 4)
            for (u32 n = 0; n <= NV; n += n < 5 ? 1 : 31) {
                memset(a, 0x55, sizeof a);
                memset(b, 0x55, sizeof b);
                fixed_plain(a, (const u8 *)src, n, size, stride);
                fixed_sse2(b, (const u8 *)src, n, size, stride);
                bad += memcmp(a, b, sizeof a) != 0;   /* the floats and the bytes after them */
            }
    return bad;
}
int gles_selftest_index(void)
{
    static u8 buf[4 * 5000 + 16];
    u32 x = 0x9e3779b9u;
    int bad = 0;
    static const u32 counts[] = {0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 100, 4999};
    for (int round = 0; round < 6; round++)
        for (u32 n = 1; n <= 4; n *= 2)
            for (unsigned c = 0; c < sizeof counts / sizeof counts[0]; c++) {
                for (u32 i = 0; i < sizeof buf; i++)
                    buf[i] = (u8)(round < 2 ? st_rand(&x) : round == 2 ? 0xff : round == 3 ? 0 : 0x80 + (st_rand(&x) & 1));
                if (round == 5 && counts[c])     /* one extreme placed at the very end */
                    memset(buf + (counts[c] - 1) * n, 0xff, n);
                for (u32 off = 0; off < 3; off++) {
                    u32 a0, a1, b0, b1;
                    index_plain(buf + off * n, counts[c], n, &a0, &a1);
                    index_sse2(buf + off * n, counts[c], n, &b0, &b1);
                    bad += a0 != b0 || a1 != b1;
                }
            }
    return bad;
}

/* STREAM_VBO (optimisation pass 5; GT_STREAM_VBO=1 by default, 0 goes back to client arrays)
   Why.  The game draws most of what it draws from vertex arrays in its own memory (an iPad has one memory for the
   CPU and the GPU, so a "client array" costs it nothing).  A desktop driver has to copy such an array into memory
   the GPU can read at every draw, inside the draw call, by its own paths: on NVIDIA's threaded driver that made each
   such draw ~8x dearer in the FastFive port (REFastFive streamvbo.c), and built-in GPUs' drivers are
   known for slow client-array paths.  Not a fault of the game or of the port.
   What.  Before a draw with client arrays the host copies the vertices the draw uses (the range draw_prepare
   already works out, from the indices) into a buffer object of its own (a 4 MB ring per GL context, renewed when
   full) and points those arrays at it; client indices go into a 1 MB ring of their own the same way.  The bytes
   drawn are the same.  The game's pointers stay shadowed (Attr): an array the host moved is set back before a draw
   that is not streamed, and a glVertexAttribPointer of the game's replaces it.  GL_FIXED arrays are copied from
   their float copy (draw_prepare).  The buffer bindings the game made are put back before the draw returns.
   GT_STREAM_VBO=0: every draw (client arrays); subdata / map: how the bytes are copied (sv_mode below).
   GT_GL_STATS=1 counts what it did every 600 frames. */
#define SV_RING  (4u << 20)
#define SV_IRING (1u << 20)
typedef void(GT_GLAPI *T_sv_subdata)(u32 target, u32 off, u32 n, const void *p);
typedef void(GT_GLAPI *T_sv_basevertex)(u32 mode, u32 count, u32 type, u32 ptr, int base);
typedef void(GT_GLAPI *T_sv_storage)(u32 target, u32 size, const void *p, u32 flags);
typedef void *(GT_GLAPI *T_sv_map)(u32 target, u32 off, u32 n, u32 access);
typedef void *(GT_GLAPI *T_sv_fence)(u32 cond, u32 flags);
typedef u32(GT_GLAPI *T_sv_wait)(void *sync, u32 flags, unsigned long long ns);
typedef void(GT_GLAPI *T_sv_delsync)(void *sync);
static T_sv_subdata sv_subdata;
static T_sv_basevertex sv_basevertex;
static T_sv_storage sv_storage;
static T_sv_map sv_mapr;
static T_sv_fence sv_fencesync;
static T_sv_wait sv_clientwait;
static T_sv_delsync sv_delsync;
static int sv_on = -1;
/* how the copies get there: 1 glBufferSubData into a ring renewed (orphaned) when full; 2 (default) a ring 4x the
   size mapped once (glBufferStorage MAP_PERSISTENT | MAP_COHERENT, GL 4.4), the copy a memcpy, a fence a quarter so a
   quarter is written again only after the GPU has drawn from it.  GT_STREAM_VBO=subdata / map picks one.
   Why 2: mode 1 makes an AMD built-in GPU slower than no streaming at all, measured with a laptop in
   hybrid mode (race at speed, uncapped, 2 runs each, a test save):
                  off          subdata          map
     AMD Radeon(TM)  328/348 fps  324/342 (CPU 120 %)  346/341 (CPU 86-99 %); capped 60: CPU 21 / 25 / 19 %
     NVIDIA (hybrid) 244/237      282/281             305/296
   AMD's driver does each glBufferSubData with work on its own threads; a mapped ring is a plain memcpy. */
static int sv_mode;
#define SV_SEGS 4
static u32 sv_size(int w) { return (w ? SV_IRING : SV_RING) * (sv_mode == 2 ? SV_SEGS : 1); }
/* GT_GL_STATS: draws, those with client arrays, those streamed, bytes copied, element buffers read back for an
   index range, draws whose arrays did not fit */
static struct { u32 draws, client, streamed, bytes, readback, nofit, mixed, kept, hit; } sv_st;

/* MIXED_KEEP.  Why: a streamed draw was rebased (the ring's copy counted from vertex `first` / the lowest index:
   glDrawArrays from 0, glDrawElementsBaseVertex with -lo), and a base vertex moves EVERY array of the draw,
   also those read from the game's own buffer objects, which were not copied, so such an array would be read
   lo vertices too early.  The game draws such mixes all the time in a race (nearly every streamed draw has
   attribute 3 enabled from a buffer, France Raceway), but GT_GL_STATS shows that none of the programs of those
   draws reads it (a left-over enabled array), so nothing drawn has looked wrong because of it so far.  A
   latent port fault (STREAM_VBO, optimisation pass 5), closed before a draw that does read one shows it.
   What: a draw with any enabled array in a buffer keeps its vertex numbers: the copies go into the ring where
   the arrays' vertex 0 would be (sv_move's "not rebased", room for the vertices before them) and the draw is made
   with the game's own first / indices.  A draw too far into its arrays for that is not streamed (client arrays,
   as with GT_STREAM_VBO=0).
   GT_STREAM_MIXED_KEEP=0 (testing only): rebased. */
static int mk_on = -1;
static int mk_enabled(void)
{
    if (mk_on < 0) {
        const char *e = getenv("GT_STREAM_MIXED_KEEP");
        mk_on = !(e && *e == '0');
        rt_log("[gl] MIXED_KEEP: %s\n", mk_on ? "on" : "off (GT_STREAM_MIXED_KEEP=0)");
    }
    return mk_on;
}
/* GT_GL_STATS: does the draw's program read one of the arrays from buffers (a draw MIXED_KEEP matters for)?
   The attribute locations each program reads are asked once and kept (statistics only). */
static int st_on = -1;
static u32 prog_reads(u32 prog)
{
    static struct { u32 prog, mask; } c[256];
    static int n;
    for (int i = 0; i < n; i++)
        if (c[i].prog == prog)
            return c[i].mask;
    u32 cnt = 0, mask = 0;
    p_glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, GA(&cnt));
    for (u32 i = 0; i < cnt && i < 32; i++) {
        char name[64] = {0};
        u32 len = 0, size = 0, type = 0;
        p_glGetActiveAttrib(prog, i, sizeof name, GA(&len), GA(&size), GA(&type), GA(name));
        int loc = (int)p_glGetAttribLocation(prog, GA(name));
        if (loc >= 0 && loc < 32)
            mask |= 1u << loc;
    }
    if (n < 256)
        c[n].prog = prog, c[n].mask = mask, n++;
    return mask;
}
static void mixed_note(VState *s, u32 bufs, u32 lo, int keep)
{
    if (st_on < 0)
        st_on = dev_getenv("GT_GL_STATS") && *dev_getenv("GT_GL_STATS") == '1';
    if (!GT_DEV || !st_on || !lo || !bufs)
        return;
    u32 prog = 0;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, GA(&prog));
    u32 r = prog_reads(prog);
    if (!(r & bufs))
        return;
    sv_st.hit++;
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 6)
        rt_log("[gl] MIXED_KEEP: program %u reads attributes 0x%x, from buffers 0x%x, client 0x%x; lowest vertex %u: %s\n",
               prog, r, bufs, s->client, lo, keep ? "vertex numbers kept" : "REBASED (buffer arrays read from the wrong vertices)");
}

static int sv_enabled(void)
{
    if (sv_on < 0) {
        const char *e = getenv("GT_STREAM_VBO");
        int want = !(e && *e == '0');
        sv_subdata = (T_sv_subdata)SDL_GL_GetProcAddress("glBufferSubData");
        sv_basevertex = (T_sv_basevertex)SDL_GL_GetProcAddress("glDrawElementsBaseVertex");
        sv_storage = (T_sv_storage)SDL_GL_GetProcAddress("glBufferStorage");
        sv_mapr = (T_sv_map)SDL_GL_GetProcAddress("glMapBufferRange");
        sv_fencesync = (T_sv_fence)SDL_GL_GetProcAddress("glFenceSync");
        sv_clientwait = (T_sv_wait)SDL_GL_GetProcAddress("glClientWaitSync");
        sv_delsync = (T_sv_delsync)SDL_GL_GetProcAddress("glDeleteSync");
        int can_map = sv_storage && sv_mapr && sv_fencesync && sv_clientwait && sv_delsync;
        sv_mode = e && !_stricmp(e, "subdata") ? 1 : 2;
        if (sv_mode == 2 && !can_map)
            sv_mode = 1;
        sv_on = want && sv_subdata;
        rt_log("[gl] STREAM_VBO: %s%s\n",
               sv_on ? (sv_mode == 2 ? "on (persistent mapped ring)" : "on (glBufferSubData ring)")
                     : want ? "off (no glBufferSubData)" : "off (GT_STREAM_VBO=0)",
               sv_on && !sv_basevertex ? " (no glDrawElementsBaseVertex: indexed draws keep their offsets)" : "");
    }
    return sv_on;
}

/* this context's rings, made on first use (and again if the thread's context is not the one they were made in) */
static int sv_ready(VState *s)
{
    void *ctx = gt_gl_current();
    if (s->sv_ring && s->sv_ctx == ctx)
        return 1;
    if (!ctx)
        return 0;
    u32 b[2] = {0, 0};
    p_glGenBuffers(2, GA(b));          /* p_: not the game's (buf_gen): the names are never deleted */
    if (!b[0] || !b[1])
        return 0;
    unsigned char *mp[2] = {NULL, NULL};
    for (int w = 0; w < 2; w++) {
        u32 target = w ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER, size = sv_size(w);
        p_glBindBuffer(target, b[w]);
        if (sv_mode == 2) {
            sv_storage(target, size, NULL, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
            mp[w] = sv_mapr(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        } else
            p_glBufferData(target, size, 0, GL_STREAM_DRAW);
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, s->array_buf);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->elem_buf);
    if (sv_mode == 2 && (!mp[0] || !mp[1])) {   /* the driver would not map them: no streaming in this context */
        rt_log("[gl] STREAM_VBO: the persistent rings could not be mapped; this context draws as before\n");
        return 0;
    }
    memset(s->sv_fence, 0, sizeof s->sv_fence);
    s->sv_map[0] = mp[0], s->sv_map[1] = mp[1], s->sv_seg[0] = s->sv_seg[1] = 0;
    s->sv_ring = b[0], s->sv_iring = b[1], s->sv_off = s->sv_ioff = 0, s->sv_moved = 0, s->sv_ctx = ctx;
    rt_log("[gl] STREAM_VBO: rings %u / %u for context %p (thread %lu)\n", b[0], b[1], ctx, GetCurrentThreadId());
    return 1;
}

/* where the next total bytes of ring w (0 vertices, 1 indices) go, from *off on (*off may wrap to 0).  glBufferSubData
   ring: a full ring is renewed (orphaned; the draws before keep the old bytes).  Persistent ring: each quarter it moves
   into, the fence of when it last left that quarter is waited for (the GPU has drawn from it by then; at ~1 MB a frame
   that was ~12 frames ago, so the wait is normally already over), and the quarter it leaves gets a new fence. */
static void sv_room(VState *s, int w, u32 *off, u32 total)
{
    u32 size = sv_size(w);
    if (!total)
        return;
    if (sv_mode != 2) {
        if (*off + total > size) {
            p_glBufferData(w ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER, size, 0, GL_STREAM_DRAW);
            *off = 0;
        }
        return;
    }
    if (*off + total > size)
        *off = 0;
    u32 seg = size / SV_SEGS, last = (*off + total - 1) / seg, cur = s->sv_seg[w];
    while (cur != last) {
        s->sv_fence[w][cur] = sv_fencesync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        cur = (cur + 1) % SV_SEGS;
        void **f = &s->sv_fence[w][cur];
        if (*f) {
            for (int i = 0; i < 50; i++) {     /* GL_SYNC_FLUSH_COMMANDS_BIT the first time; 0.1 s a try */
                u32 r = sv_clientwait(*f, i ? 0 : 1, 100000000ull);
                if (r != GL_TIMEOUT_EXPIRED)               /* not GL_TIMEOUT_EXPIRED: signalled, or failed */
                    break;
            }
            sv_delsync(*f);
            *f = NULL;
        }
    }
    s->sv_seg[w] = cur;
}

static void sv_write(VState *s, int w, u32 off, u32 bytes, u32 src)
{
    if (sv_mode == 2)
        memcpy(s->sv_map[w] + off, (const void *)(uintptr_t)src, bytes);
    else
        sv_subdata(w ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER, off, bytes, (const void *)(uintptr_t)src);
}

/* the arrays in m back to the game's memory (a draw that is not streamed) */
static void sv_put_back(VState *s, u32 m)
{
    m &= s->sv_moved;
    if (!m)
        return;
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (u32 k = m; k; k &= k - 1) {
        u32 i = (u32)__builtin_ctz(k);
        Attr *a = &s->a[i];
        if (a->type != GL_FIXED)      /* draw_prepare points those at their floats at each draw */
            p_glVertexAttribPointer(i, a->size, a->type, a->norm, a->stride, a->ptr);
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, s->array_buf);
    s->sv_moved &= ~m;
}

/* vertices first..last of the client arrays in m into the ring.  rebase: the ring's copy starts at vertex first
   (the draw then counts from there); else at vertex 0's place, which needs room for first vertices before it.
   0: it did not fit (nothing changed). */
static int sv_move(VState *s, u32 m, u32 first, u32 last, int rebase, u32 pend)
{
    struct { u32 lo, hi, base, stride; u32 i; } a[MAX_ATTR];
    int na = 0;
    for (u32 k = m; k; k &= k - 1) {
        u32 i = (u32)__builtin_ctz(k);
        const Attr *at = &s->a[i];
        u32 elem, stride, base;
        if (at->type == GL_FIXED) {   /* its floats (draw_prepare): size floats a vertex, from vertex 0 */
            elem = stride = at->size * 4;
            base = (u32)(uintptr_t)at->conv;
        } else {
            elem = at->size * type_bytes(at->type);
            stride = at->stride ? at->stride : elem;
            base = at->ptr;
        }
        a[na].lo = base + first * stride;
        a[na].hi = base + last * stride + elem;
        a[na].base = base, a[na].stride = stride, a[na].i = i;
        na++;
    }
    for (int x = 1; x < na; x++)        /* by lo: arrays in one interleaved block are copied once */
        for (int y = x; y > 0 && a[y].lo < a[y - 1].lo; y--) {
            __typeof__(a[0]) t = a[y];
            a[y] = a[y - 1];
            a[y - 1] = t;
        }
    /* the blocks first, so that a draw that does not fit changes nothing */
    struct { u32 glo, ghi, need; int x, y; } g[MAX_ATTR];
    int ng = 0;
    u32 total = 0;
    for (int x = 0; x < na;) {
        u32 glo = a[x].lo, ghi = a[x].hi;
        int y = x + 1;
        while (y < na && a[y].lo < ghi) {
            if (a[y].hi > ghi)
                ghi = a[y].hi;
            y++;
        }
        u32 need = 0;                   /* not rebased: the pointer is the copy's place - (glo - base) */
        for (int k = x; k < y && !rebase; k++)
            if (glo - a[k].base > need)
                need = glo - a[k].base;
        if (need > SV_RING || ghi - glo > SV_RING - need) {
            sv_st.nofit++;
            return 0;
        }
        total += ((need + 15) & ~15u) + ghi - glo + 16;
        g[ng].glo = glo, g[ng].ghi = ghi, g[ng].need = need, g[ng].x = x, g[ng].y = y;
        ng++;
        x = y;
    }
    if (total > SV_RING) {
        sv_st.nofit++;
        return 0;
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, s->sv_ring);
    sv_room(s, 0, &s->sv_off, total);
    for (int j = 0; j < ng; j++) {
        u32 off = (s->sv_off + 15) & ~15u;
        if (off < g[j].need)
            off = (g[j].need + 15) & ~15u;
        u32 bytes = g[j].ghi - g[j].glo;
        /* FIXED_FAST: a GL_FIXED array not converted yet (pend) is converted straight into the mapped ring when
           its float copy is a block of its own (always: the copy is the host's memory); else into the copy first */
        int direct = 0;
        for (int k = g[j].x; k < g[j].y; k++) {
            u32 i = a[k].i;
            if (!(pend & 1u << i))
                continue;
            Attr *at = &s->a[i];
            u32 st = at->stride ? at->stride : at->size * 4;
            const u8 *src = (const u8 *)(uintptr_t)(at->ptr + first * st);
            if (g[j].y - g[j].x == 1 && sv_mode == 2) {
                fixed_convert((float *)(s->sv_map[0] + off), src, last - first + 1, at->size, st);
                fx_direct += last - first + 1;
                direct = 1;
            } else
                fixed_convert(at->conv + first * at->size, src, last - first + 1, at->size, st);
        }
        if (!direct)
            sv_write(s, 0, off, bytes, g[j].glo);
        s->sv_off = off + bytes;
        sv_st.bytes += bytes;
        for (int k = g[j].x; k < g[j].y; k++) {
            u32 i = a[k].i;
            const Attr *at = &s->a[i];
            u32 at_off = rebase ? off + (a[k].lo - g[j].glo) : off + a[k].base - g[j].glo;
            if (at->type == GL_FIXED)
                p_glVertexAttribPointer(i, at->size, GL_FLOAT, 0, 0, at_off);
            else
                p_glVertexAttribPointer(i, at->size, at->type, at->norm, at->stride, at_off);
            s->sv_moved |= 1u << i;
        }
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, s->array_buf);
    return 1;
}

/* client indices into the index ring: the offset to draw with, or ~0 (then the draw reads the game's memory) */
static u32 sv_indices(VState *s, u32 ptr, u32 bytes)
{
    if (bytes > SV_IRING)
        return ~0u;
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->sv_iring);
    u32 off = (s->sv_ioff + 3) & ~3u;
    sv_room(s, 1, &off, bytes);
    sv_write(s, 1, off, bytes, ptr);
    s->sv_ioff = off + bytes;
    sv_st.bytes += bytes;
    return off;                         /* the ring stays bound for the draw; the caller binds the game's back */
}

/* the vertices a draw reads: [*lo, *hi]; 0 if the indices cannot be read */
static int index_range(VState *s, u32 count, u32 type, u32 ptr, u32 *lo, u32 *hi)
{
    u32 n = type_bytes(type);
    const unsigned char *p;
    unsigned char *tmp = NULL;
    if (s->elem_buf) {
        sv_st.readback++;
        tmp = malloc(count * n);
        if (!tmp)
            return 0;
        p_glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, ptr, count * n, GA(tmp));
        p = tmp;
    } else {
        if (!readable(s, ptr, count * n))
            return 0;
        p = (const unsigned char *)(uintptr_t)ptr;
    }
    u32 mn, mx;
    if (ix_enabled())
        index_sse2(p, count, n, &mn, &mx);
    else
        index_plain(p, count, n, &mn, &mx);
    free(tmp);
    *lo = count ? mn : 0, *hi = count ? mx : 0;
    return 1;
}

/* GT_GL_STATS: an array left out of a draw that its program reads (the draw then misses that data) */
static void unreadable_used(u32 i)
{
    if (st_on < 0)
        st_on = dev_getenv("GT_GL_STATS") && *dev_getenv("GT_GL_STATS") == '1';
    if (!GT_DEV || !st_on)
        return;
    u32 prog = 0;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, GA(&prog));
    u32 r = prog_reads(prog);
    static volatile LONG used, unused;
    if (r & 1u << i) {
        LONG n = InterlockedIncrement(&used);
        if (n <= 4 || n == 100 || n == 10000)
            rt_log("[gl] left out: attribute %u IS read by program %u (reads 0x%x; %ld such draws so far)\n", i, prog, r,
                   n);
    } else if (InterlockedIncrement(&unused) <= 4)
        rt_log("[gl] left out: attribute %u is not read by program %u (reads 0x%x)\n", i, prog, r);
}

/* before a draw over vertices [lo, hi] (have_range 0: unknown); returns a mask of arrays switched off for it.
   pend (or NULL): GL_FIXED arrays left for sv_move / fixed_finish to convert (FIXED_FAST) */
static u32 draw_prepare(VState *s, int have_range, u32 lo, u32 hi, u32 *pend)
{
    u32 off = 0;
    for (u32 m = s->client; m; m &= m - 1) {          /* DRAW_FAST: only the client-memory arrays */
        u32 i = (u32)__builtin_ctz(m);
        Attr *a = &s->a[i];
        u32 elem = a->size * type_bytes(a->type);
        u32 stride = a->stride ? a->stride : elem;
        u32 first = have_range ? lo : 0, last = have_range ? hi : 0;
        u32 start = a->ptr + first * stride, len = (last - first) * stride + elem;
        if (!a->ptr || !readable(s, start, len)) {
            unreadable_note(i, a, first, last, s->bufarr);
            unreadable_used(i);
            p_glDisableVertexAttribArray(i);
            off |= 1u << i;
            continue;
        }
        if (a->type == GL_FIXED) {
            u32 need = (last + 1) * a->size;
            if (a->conv_n < need) {
                free(a->conv);
                a->conv = malloc(need * sizeof(float));
                a->conv_n = a->conv ? need : 0;
                if (!a->conv) {
                    p_glDisableVertexAttribArray(i);
                    off |= 1u << i;
                    continue;
                }
            }
            /* FIXED_FAST with the mapped ring: converted by sv_move straight into the ring, or by fixed_finish
               when the draw is not streamed (the float copy's address still places the array in sv_move) */
            if (pend && fx_enabled() && sv_enabled() && sv_mode == 2) {
                *pend |= 1u << i;
                continue;
            }
            fixed_convert(a->conv + first * a->size, (const u8 *)(uintptr_t)(a->ptr + first * stride),
                          last - first + 1, a->size, stride);
            p_glVertexAttribPointer(i, a->size, GL_FLOAT, 0, 0, GA(a->conv));
        }
    }
    return off;
}
/* the GL_FIXED arrays in pend (draw_prepare) into their float copies, pointed at them: a draw not streamed */
static void fixed_finish(VState *s, u32 pend, int have_range, u32 lo, u32 hi)
{
    for (; pend; pend &= pend - 1) {
        u32 i = (u32)__builtin_ctz(pend);
        Attr *a = &s->a[i];
        u32 stride = a->stride ? a->stride : a->size * 4;
        u32 first = have_range ? lo : 0, last = have_range ? hi : 0;
        fixed_convert(a->conv + first * a->size, (const u8 *)(uintptr_t)(a->ptr + first * stride), last - first + 1,
                      a->size, stride);
        p_glVertexAttribPointer(i, a->size, GL_FLOAT, 0, 0, GA(a->conv));
    }
}
static void draw_done(u32 off)
{
    for (u32 i = 0; off; i++, off >>= 1)
        if (off & 1)
            p_glEnableVertexAttribArray(i);
}

void h_glDrawArrays(u32 mode, u32 first, u32 count)
{
    VState *s = vs();
    s->draw_mode = mode, s->draw_count = count, s->draw_type = 0, s->draw_ptr = first, s->draws++;
    if (!count)
        return;
    gles_draw_lock();
    check_deleted(s);
    sv_st.draws++;
    u32 off = 0;
    if (s->client) {
        sv_st.client++;
        u32 pend = 0;
        off = draw_prepare(s, 1, first, first + count - 1, &pend);
        u32 m = s->client & ~off;
        int keep = first && (s->bufarr & ~off) && mk_enabled();    /* MIXED_KEEP */
        if (m && sv_enabled() && sv_ready(s) && sv_move(s, m, first, first + count - 1, !keep, pend)) {
            sv_st.streamed++;
            if (s->bufarr & ~off)
                sv_st.mixed++, sv_st.kept += keep != 0, mixed_note(s, s->bufarr & ~off, first, keep);
            p_glDrawArrays(mode, keep ? first : 0, count);
            draw_done(off);
            gles_draw_unlock();
            return;
        }
        sv_put_back(s, m);
        fixed_finish(s, pend, 1, first, first + count - 1);
    }
    p_glDrawArrays(mode, first, count);
    draw_done(off);
    gles_draw_unlock();
}
void h_glDrawElements(u32 mode, u32 count, u32 type, u32 ptr)
{
    VState *s = vs();
    s->draw_mode = mode, s->draw_count = count, s->draw_type = type, s->draw_ptr = ptr, s->draws++;
    if (!count)
        return;
    gles_draw_lock();
    check_deleted(s);
    if (!s->elem_buf && !readable(s, ptr, count * type_bytes(type))) {
        once("glDrawElements with unreadable client indices: skipped");
        gles_draw_unlock();
        return;
    }
    /* the index range is needed only when some enabled array is in client memory */
    u32 lo = 0, hi = 0, off = 0;
    sv_st.draws++;
    if (s->client) {
        sv_st.client++;
        int have = index_range(s, count, type, ptr, &lo, &hi);
        u32 pend = 0;
        off = draw_prepare(s, have, lo, hi, &pend);
        u32 m = s->client & ~off;
        /* STREAM_VBO: rebased (the copy starts at vertex lo) when lo is 0 or the draw can say where it starts */
        int keep = lo && (s->bufarr & ~off) && mk_enabled();       /* MIXED_KEEP: arrays in buffers too */
        int rebase = lo == 0 || (!keep && sv_basevertex && lo <= 0x7fffffffu);
        if (have && m && sv_enabled() && sv_ready(s) && sv_move(s, m, lo, hi, rebase, pend)) {
            sv_st.streamed++;
            if (s->bufarr & ~off)
                sv_st.mixed++, sv_st.kept += keep != 0, mixed_note(s, s->bufarr & ~off, lo, keep);
            u32 ip = s->elem_buf ? ptr : sv_indices(s, ptr, count * type_bytes(type));
            if (ip == ~0u)
                p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0), ip = ptr;
            if (rebase && lo)
                sv_basevertex(mode, count, type, ip, -(int)lo);
            else
                p_glDrawElements(mode, count, type, ip);
            if (!s->elem_buf)
                p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            draw_done(off);
            gles_draw_unlock();
            return;
        }
        sv_put_back(s, m);
        fixed_finish(s, pend, have, lo, hi);
    }
    p_glDrawElements(mode, count, type, ptr);
    draw_done(off);
    gles_draw_unlock();
}

/* STATE_CACHE (port addition, optimisation pass 2): state shadowing for glBindTexture, glUseProgram and
   glBlendFunc, to avoid redundant OpenGL driver calls while the GameSWF UI renders
   gameswf binds the texture, the program and the blend function again before each of its many small draws, mostly
   to what is already bound; each such call is a trip into the driver.  The context's bindings are shadowed here
   (in its VState, so per context as GL keeps them) and a call that changes nothing is not passed on.
   What keeps the shadow true:
   - a texture or program deleted on any thread (the loader thread's shared context deletes and makes them while
     loading; GL can hand the same name out again, and a context that still has the old one bound must bind the
     new one for real) makes every context forget its texture / program shadow at its next bind;
   - glBlendFuncSeparate (set behind glBlendFunc's back) forgets the blend shadow;
   - the host's own GL work (eagl.c's present, glenhance.c) restores what it changes, and the shadow is forgotten
     after each present as well (gles_state_forget), so nothing the host does can leave it stale for long.
   GT_GL_STATE_CACHE=0: every call goes to the driver.  GT_GL_STATS=1 (testing): a log line every 600
   frames with how many calls were left out, of each kind.
   Optimisation pass 3 extends it to glBindBuffer, glEnable / glDisable (GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE,
   GL_SCISSOR_TEST), glDepthMask, glColorMask and glUniform* per program object: the same, below.  Buffers: a bind
   is left out only
   while no buffer was deleted or made on any thread since (buf_gen), as a name can come back for a new buffer.
   The caps: glEnable(GL_TEXTURE_2D) is per texture unit and is always passed on.  Uniforms: per program (GL
   keeps them in the program, across contexts and frames), see UNIFORMS below. */
static volatile LONG tex_del_gen, prog_del_gen;
static int sc_on = -1;
static int sc_enabled(void)
{
    if (sc_on < 0) {
        const char *e = getenv("GT_GL_STATE_CACHE");
        sc_on = !(e && *e == '0');
        rt_log("[gl] state cache: %s\n", sc_on ? "on" : "off (GT_GL_STATE_CACHE=0)");
    }
    return sc_on;
}
void gles_state_forget(void)
{
    if (vs_)
        sc_forget(vs_);
    dd_sweep();                          /* DEFER_DELETE: kept buffers no context names any more */
    static int stats = -1;
    if (stats < 0)
        stats = dev_getenv("GT_GL_STATS") && *dev_getenv("GT_GL_STATS") == '1';
    if (GT_DEV && stats && eagl_frames() % 600 == 0) {
        static const char *const what[SC_KINDS] = {"texture", "program", "blend", "enable", "mask", "buffer",
                                                   "uniform", "array"};
        char line[512];
        int o = 0;
        for (int k = 0; k < SC_KINDS && o < (int)sizeof line - 64; k++) {
            o += snprintf(line + o, sizeof line - (size_t)o, "%s%s %u/%u", k ? ", " : "", what[k], sc_skipped[k],
                          sc_calls[k]);
            sc_skipped[k] = sc_calls[k] = 0;
        }
        rt_log("[gl] state cache, calls left out / made in 600 frames: %s\n", line);
        rt_log("[gl] client arrays in 600 frames: %u memory queries; %u in the exe's image, %u on the guest stack and "
               "%u in live heap blocks (LIVE_BLOCKS) without one\n", rd_query, rd_image, rd_stack, rd_live);
        rd_query = rd_image = rd_stack = rd_live = 0;
        rt_log("[gl] GL_FIXED vertices in 600 frames: %u turned into floats, %u of them straight into the ring "
               "(FIXED_FAST)\n", fx_verts, fx_direct);
        fx_verts = fx_direct = 0;
        gles_pvrtc_stats();
        rt_log("[gl] draws in 600 frames: %u, %u with client arrays, %u of them streamed (STREAM_VBO, %.0f KB a frame); "
               "%u index ranges read back from an element buffer, %u did not fit; %u streamed with arrays in buffers "
               "too (MIXED_KEEP: %u kept their vertex numbers; %u from a vertex past 0 whose program reads an array from a "
               "buffer)\n", sv_st.draws, sv_st.client, sv_st.streamed, sv_st.bytes / 600.0 / 1024, sv_st.readback,
               sv_st.nofit, sv_st.mixed, sv_st.kept, sv_st.hit);
        memset(&sv_st, 0, sizeof sv_st);
        rt_log("[gl] DEFER_DELETE in 600 frames: %u deletes held back, %u held ones deleted, %d held now\n", dd_kept,
               dd_freed, dd_n);
        dd_kept = dd_freed = 0;
    }
}
void h_glActiveTexture(u32 unit)
{
    VState *s = vs();
    s->tk_unit = unit, s->tk_known |= TK_UNIT;
    sc_calls[SC_TEX]++;
    if (sc_enabled() && s->sc_unit == unit) {
        sc_skipped[SC_TEX]++;
        return;
    }
    s->sc_unit = unit;
    p_glActiveTexture(unit);
}
void h_glBindTexture(u32 target, u32 id)
{
    VState *s = vs();
    if (target == GL_TEXTURE_2D && (s->tk_known & TK_UNIT)) {
        if (s->tk_unit == GL_TEXTURE0)
            s->tk_tex0 = id, s->tk_known |= TK_TEX0;
    } else if (target == GL_TEXTURE_2D)
        s->tk_known &= ~(u32)TK_TEX0;
    u32 u = s->sc_unit - GL_TEXTURE0;
    if (target != GL_TEXTURE_2D || u >= 32 || !sc_enabled()) {
        if (u < 32 && target == GL_TEXTURE_2D)
            s->sc_tex[u] = id;
        p_glBindTexture(target, id);
        return;
    }
    u32 g = (u32)tex_del_gen;
    if (s->sc_tex_gen != g) {
        s->sc_tex_gen = g;
        memset(s->sc_tex, 0xff, sizeof s->sc_tex);
    }
    sc_calls[SC_TEX]++;
    if (s->sc_tex[u] == id) {
        sc_skipped[SC_TEX]++;
        return;
    }
    s->sc_tex[u] = id;
    p_glBindTexture(target, id);
}
void h_glDeleteTextures(u32 n, u32 ids)
{
    VState *s = vs();
    const u32 *v = GP(ids);
    for (u32 i = 0; v && i < n; i++) {          /* GL unbinds them in this context: the shadow follows */
        for (u32 u = 0; u < 32; u++)
            if (v[i] && s->sc_tex[u] == v[i])
                s->sc_tex[u] = 0;
        if (v[i] && s->tk_tex0 == v[i])
            s->tk_tex0 = 0;
    }
    p_glDeleteTextures(n, ids);
    InterlockedIncrement(&tex_del_gen);         /* other contexts: forget at their next bind */
}
/* Host uniforms (gles_add_host_uniform, gl.h): a shader given one of these uniforms (by a source patch,
   gles_add_shader_patch) gets its value from the host whenever its program is used and the value is not the one it
   holds.  A program's locations are looked up once, at its link. */
#define HOST_UNI_MAX 32                 /* programs that have one */
#define HOST_UNI_KINDS 4                /* uniforms registered */
static struct { const char *name; float (*value)(void); } host_uni_kind[HOST_UNI_KINDS];
static int nhost_uni_kinds;
static struct { u32 prog; int loc[HOST_UNI_KINDS]; float val[HOST_UNI_KINDS]; } host_uni[HOST_UNI_MAX];
static int nhost_uni;
static SRWLOCK host_uni_lock = SRWLOCK_INIT;

int gles_add_host_uniform(const char *name, float (*value)(void))
{
    if (nhost_uni_kinds >= HOST_UNI_KINDS)
        return 0;
    host_uni_kind[nhost_uni_kinds].name = name;
    host_uni_kind[nhost_uni_kinds++].value = value;
    return 1;
}

static float host_uni_value(int k) { return host_uni_kind[k].value(); }

static void host_uni_linked(u32 prog)
{
    int loc[HOST_UNI_KINDS], any = 0;
    for (int k = 0; k < HOST_UNI_KINDS; k++) {
        loc[k] = k < nhost_uni_kinds ? (int)p_glGetUniformLocation(prog, GA(host_uni_kind[k].name)) : -1;
        any |= loc[k] >= 0;
    }
    AcquireSRWLockExclusive(&host_uni_lock);
    int i = 0;
    while (i < nhost_uni && host_uni[i].prog != prog)
        i++;
    if (any && i == nhost_uni && nhost_uni < HOST_UNI_MAX)
        nhost_uni++;
    if (i < nhost_uni) {
        host_uni[i].prog = any ? prog : 0;
        for (int k = 0; k < HOST_UNI_KINDS; k++) {
            host_uni[i].loc[k] = loc[k];
            host_uni[i].val[k] = -1.0f;     /* not set yet */
        }
    }
    ReleaseSRWLockExclusive(&host_uni_lock);
}

static void host_uni_used(u32 prog)
{
    if (!nhost_uni || !prog)
        return;
    AcquireSRWLockExclusive(&host_uni_lock);
    for (int i = 0; i < nhost_uni; i++)
        if (host_uni[i].prog == prog) {
            for (int k = 0; k < HOST_UNI_KINDS; k++) {
                if (host_uni[i].loc[k] < 0)
                    continue;
                float v = host_uni_value(k);
                if (host_uni[i].val[k] != v) {
                    host_uni[i].val[k] = v;
                    p_glUniform1f((u32)host_uni[i].loc[k], v);
                }
            }
            break;
        }
    ReleaseSRWLockExclusive(&host_uni_lock);
}

void h_glUseProgram(u32 prog)
{
    VState *s = vs();
    s->tk_prog = prog, s->tk_known |= TK_PROG;
    if (!sc_enabled()) {
        p_glUseProgram(prog);
        host_uni_used(prog);
        return;
    }
    u32 g = (u32)prog_del_gen;
    if (s->sc_prog_gen != g) {
        s->sc_prog_gen = g;
        s->sc_prog = SC_UNKNOWN;
    }
    sc_calls[SC_PROG]++;
    if (s->sc_prog == prog) {
        sc_skipped[SC_PROG]++;
        host_uni_used(prog);
        return;
    }
    s->sc_prog = prog;
    p_glUseProgram(prog);
    host_uni_used(prog);
}
static volatile LONG uni_gen;           /* UNIFORMS: bumped when program values may have changed behind the cache */
void h_glDeleteProgram(u32 prog)
{
    p_glDeleteProgram(prog);
    InterlockedIncrement(&prog_del_gen);
    InterlockedIncrement(&uni_gen);
}
void h_glBlendFunc(u32 src, u32 dst)
{
    VState *s = vs();
    sc_calls[SC_BLEND]++;
    if (sc_enabled() && s->sc_bsrc == src && s->sc_bdst == dst) {
        sc_skipped[SC_BLEND]++;
        return;
    }
    s->sc_bsrc = src, s->sc_bdst = dst;
    p_glBlendFunc(src, dst);
}
void h_glBlendFuncSeparate(u32 a, u32 b, u32 c_, u32 d)
{
    VState *s = vs();
    s->sc_bsrc = s->sc_bdst = SC_UNKNOWN;
    p_glBlendFuncSeparate(a, b, c_, d);
}

/* glEnable / glDisable: the context-wide switches gameswf and the scene renderer set before their draws */
static int cap_bit(u32 cap)
{
    switch (cap) {
    case GL_BLEND: return 0;
    case GL_DEPTH_TEST: return 1;
    case GL_CULL_FACE: return 2;
    case GL_SCISSOR_TEST: return 3;
    case GL_STENCIL_TEST: return 4;
    case GL_ALPHA_TEST: return 5;               /* GL_ALPHA_TEST (ES 1) */
    case GL_POLYGON_OFFSET_FILL: return 6;
    case GL_DITHER: return 7;
    }
    return -1;
}
static void cap_set(u32 cap, int on)
{
    if (cap == GL_SCISSOR_TEST) {                 /* GL_SCISSOR_TEST (QUERY_FREE) */
        VState *s = vs();
        s->tk_scissor_on = on != 0, s->tk_known |= TK_SCISSOR_ON;
    }
    int i = cap_bit(cap);
    if (i >= 0) {
        VState *s = vs();
        u32 b = 1u << i;
        sc_calls[SC_CAP]++;
        if ((s->sc_caps_known & b) && !(s->sc_caps_on & b) == !on && sc_enabled()) {
            sc_skipped[SC_CAP]++;
            return;
        }
        s->sc_caps_known |= b;
        s->sc_caps_on = on ? s->sc_caps_on | b : s->sc_caps_on & ~b;
    }
    if (on)
        p_glEnable(cap);
    else
        p_glDisable(cap);
}
void h_glEnable(u32 cap) { cap_set(cap, 1); }
void h_glDisable(u32 cap) { cap_set(cap, 0); }
void h_glDepthMask(u32 flag)
{
    VState *s = vs();
    u32 v = (flag & 0xff) != 0;
    sc_calls[SC_MASK]++;
    if (s->sc_dmask == v && sc_enabled()) {
        sc_skipped[SC_MASK]++;
        return;
    }
    s->sc_dmask = v;
    p_glDepthMask(v);
}
void h_glColorMask(u32 r, u32 g, u32 b, u32 a)
{
    VState *s = vs();
    u32 v = ((r & 0xff) != 0) | ((g & 0xff) != 0) << 1 | ((b & 0xff) != 0) << 2 | ((a & 0xff) != 0) << 3;
    sc_calls[SC_MASK]++;
    if (s->sc_cmask == v && sc_enabled()) {
        sc_skipped[SC_MASK]++;
        return;
    }
    s->sc_cmask = v;
    p_glColorMask(v & 1, v >> 1 & 1, v >> 2 & 1, v >> 3 & 1);
}

/* UNIFORMS (STATE_CACHE, pass 3): a glUniform* that gives the current program the values it already has at
   that location is not passed on.  GL keeps uniform values in the program object, the same for every context
   and until the program is linked again, so the cache is by (program, location), across frames.  What keeps it
   true:
   - only one thread uses it, the one that sets uniforms first (the game's render thread); a uniform set on any
     other thread, a glLinkProgram (it resets the values) or a glDeleteProgram (the name can come back) anywhere
     empties it (uni_gen);
   - the program must be known (the STATE_CACHE shadow of glUseProgram, per context), else the call is passed on;
   - the values are compared as they are given (bits, the call's kind, count and transpose), at most 16 words;
     a longer array is passed on and its slot forgotten. */
#define UNI_SLOTS 2048
typedef struct { u32 prog, loc, kind, n, v[16]; } Uni;
static Uni *uni;
static volatile LONG uni_owner;          /* the thread that uses the cache */
static __thread int uni_mine = -1;       /* this thread is it: 1, is not: 0, not asked yet: -1 */
static u32 uni_seen = SC_UNKNOWN;
enum { U_1F = 1, U_2F, U_3F, U_4F, U_1I, U_2I, U_3I, U_4I, U_1FV, U_2FV, U_3FV, U_4FV, U_1IV, U_2IV, U_3IV, U_4IV,
       U_M2, U_M3, U_M4 };

/* 1: the program already has these values (the call is left out) */
static int uni_same(u32 kind, u32 loc, const void *vals, u32 words)
{
    if ((s32)loc < 0 || !sc_enabled())
        return 0;
    VState *s = vs();
    u32 prog = s->sc_prog;
    if (prog == SC_UNKNOWN || !prog)
        return 0;
    if (uni_mine < 0) {
        LONG me = (LONG)GetCurrentThreadId();
        InterlockedCompareExchange(&uni_owner, me, 0);
        uni_mine = uni_owner == me;
    }
    if (!uni_mine) {
        InterlockedIncrement(&uni_gen);
        return 0;
    }
    if (!uni && !(uni = calloc(UNI_SLOTS, sizeof *uni)))
        return 0;
    u32 g = (u32)uni_gen;
    if (g != uni_seen) {
        memset(uni, 0, UNI_SLOTS * sizeof *uni);
        uni_seen = g;
    }
    Uni *u = &uni[(prog * 0x9E3779B1u ^ loc * 0x85EBCA77u) >> 21 & (UNI_SLOTS - 1)];
    sc_calls[SC_UNI]++;
    if (words > 16 || !vals) {
        if (u->prog == prog && u->loc == loc)
            u->prog = 0;
        return 0;
    }
    if (u->prog == prog && u->loc == loc && u->kind == kind && u->n == words && !memcmp(u->v, vals, words * 4)) {
        sc_skipped[SC_UNI]++;
        return 1;
    }
    u->prog = prog, u->loc = loc, u->kind = kind, u->n = words;
    memcpy(u->v, vals, words * 4);
    return 0;
}

#define UNI_F(N, ...)                                                                   \
    {                                                                                   \
        float v[] = {__VA_ARGS__};                                                      \
        if (!uni_same(U_##N##F, loc, v, sizeof v / 4))                                  \
            p_glUniform##N##f(loc, __VA_ARGS__);                                        \
    }
#define UNI_I(N, ...)                                                                   \
    {                                                                                   \
        u32 v[] = {__VA_ARGS__};                                                        \
        if (!uni_same(U_##N##I, loc, v, sizeof v / 4))                                  \
            p_glUniform##N##i(loc, __VA_ARGS__);                                        \
    }
void h_glUniform1f(u32 loc, float a) UNI_F(1, a)
void h_glUniform2f(u32 loc, float a, float b) UNI_F(2, a, b)
void h_glUniform3f(u32 loc, float a, float b, float c_) UNI_F(3, a, b, c_)
void h_glUniform4f(u32 loc, float a, float b, float c_, float d) UNI_F(4, a, b, c_, d)
void h_glUniform1i(u32 loc, u32 a) UNI_I(1, a)
void h_glUniform2i(u32 loc, u32 a, u32 b) UNI_I(2, a, b)
void h_glUniform3i(u32 loc, u32 a, u32 b, u32 c_) UNI_I(3, a, b, c_)
void h_glUniform4i(u32 loc, u32 a, u32 b, u32 c_, u32 d) UNI_I(4, a, b, c_, d)
/* the vector forms: count arrays of N words from the game's memory */
#define UNI_V(name, kind, N)                                                            \
    void h_##name(u32 loc, u32 count, u32 ptr)                                          \
    {                                                                                   \
        if (!uni_same(kind, loc, ptr ? GP(ptr) : NULL, count > 16 ? 17 : count * (N)))  \
            p_##name(loc, count, ptr);                                                  \
    }
UNI_V(glUniform1fv, U_1FV, 1)
UNI_V(glUniform2fv, U_2FV, 2)
UNI_V(glUniform3fv, U_3FV, 3)
UNI_V(glUniform4fv, U_4FV, 4)
UNI_V(glUniform1iv, U_1IV, 1)
UNI_V(glUniform2iv, U_2IV, 2)
UNI_V(glUniform3iv, U_3IV, 3)
UNI_V(glUniform4iv, U_4IV, 4)
#define UNI_M(name, kind, N)                                                            \
    void h_##name(u32 loc, u32 count, u32 transpose, u32 ptr)                           \
    {                                                                                   \
        if (!uni_same(kind | (transpose & 0xff ? 0x100 : 0), loc, ptr ? GP(ptr) : NULL,                 \
                      count > 16 ? 17 : count * (N)))                                                    \
            p_##name(loc, count, transpose, ptr);                                       \
    }
UNI_M(glUniformMatrix2fv, U_M2, 4)
UNI_M(glUniformMatrix3fv, U_M3, 9)
UNI_M(glUniformMatrix4fv, U_M4, 16)

/* from the fault handler, on the faulting thread */
void gles_fault_dump(void)
{
    VState *s = vs_;
    if (!s)
        return;
    rt_log("[gl] this thread's last draw: mode %u, count %u, index type 0x%x, indices/first 0x%08x (draw #%u); "
           "array buffer %u, element buffer %u\n", s->draw_mode, s->draw_count, s->draw_type, s->draw_ptr, s->draws,
           s->array_buf, s->elem_buf);
    for (u32 i = 0; i < MAX_ATTR; i++) {
        Attr *a = &s->a[i];
        if (a->on)
            rt_log("[gl]   attribute %u: %u x type 0x%x%s stride %u, %s 0x%08x\n", i, a->size, a->type,
                   a->norm ? " normalized" : "", a->stride, a->vbo ? "buffer offset" : "memory", a->ptr);
    }
}

void h_glVertexPointer(u32 size, u32 type, u32 stride, u32 ptr)
{
    if (type == GL_FIXED)
        once("GL_FIXED vertex arrays are not supported");
    p_glVertexPointer(size, type, stride, ptr);
}

/* shaders */

/* gles_add_shader_patch (gl.h): source patches run at glShaderSource, in the order registered */
#define SHADER_PATCHES_MAX 4
static char *(*shader_patches[SHADER_PATCHES_MAX])(const char *src);
static int nshader_patches;

int gles_add_shader_patch(char *(*patch)(const char *src))
{
    if (nshader_patches >= SHADER_PATCHES_MAX)
        return 0;
    shader_patches[nshader_patches++] = patch;
    return 1;
}

/* GLSL ES 1.00 -> GLSL 1.20: no #version / #extension lines of ES, no precision statements, and the
   precision words defined away */
#if !GT_GL_DESKTOP
/* OpenGL ES (Android): the game's GLSL ES as it is, but for what the host's shader patches put in front of it
   (gles_add_shader_patch: "uniform float gt_...;" on the first line, for desktop GLSL): a #version line goes back
   to the top, and the host's uniforms get a precision of their own (a fragment shader has no default one for
   float before its own precision statement) */
static char *translate_shader_es(const char *src)
{
    size_t n = strlen(src);
    char *out = malloc(n * 2 + 64), *o = out;
    if (!out)
        return NULL;
    const char *ver = NULL, *ver_end = NULL;
    for (const char *p = src; *p;) {
        const char *q = p;
        while (*q == ' ' || *q == '\t')
            q++;
        if (!strncmp(q, "#version", 8)) {
            ver = p;
            ver_end = strchr(p, '\n');
            ver_end = ver_end ? ver_end + 1 : p + strlen(p);
            break;
        }
        p = strchr(p, '\n');
        if (!p)
            break;
        p++;
    }
    if (ver) {
        memcpy(o, ver, (size_t)(ver_end - ver));
        o += ver_end - ver;
        if (o[-1] != '\n')
            *o++ = '\n';
    }
    static const char host_uni[] = "uniform float gt_";
    for (const char *p = src; *p;) {
        if (p == ver) {
            p = ver_end;
            continue;
        }
        if (!strncmp(p, host_uni, sizeof host_uni - 1)) {
            static const char now[] = "uniform mediump float gt_";
            memcpy(o, now, sizeof now - 1);
            o += sizeof now - 1;
            p += sizeof host_uni - 1;
            continue;
        }
        *o++ = *p++;
    }
    *o = 0;
    return out;
}
#endif

static char *translate_shader(const char *src)
{
#if !GT_GL_DESKTOP
    return translate_shader_es(src);
#endif
    static const char head[] = "#version 120\n#define lowp\n#define mediump\n#define highp\n";
    size_t n = strlen(src);
    char *out = malloc(n + sizeof head + 1), *o = out;
    memcpy(o, head, sizeof head - 1);
    o += sizeof head - 1;
    const char *p = src;
    int line_start = 1;
    while (*p) {
        if (line_start) {
            const char *q = p;
            while (*q == ' ' || *q == '\t')
                q++;
            if (!strncmp(q, "#version", 8) || !strncmp(q, "#extension", 10)) {
                while (*p && *p != '\n')
                    p++;
                continue;
            }
        }
        if (!strncmp(p, "precision", 9) && (p == src || !(p[-1] == '_' || isalnum((u8)p[-1]))) &&
            (p[9] == ' ' || p[9] == '\t')) {
            while (*p && *p != ';')
                p++;
            if (*p)
                p++;
            continue;
        }
        line_start = *p == '\n';
        *o++ = *p++;
    }
    *o = 0;
    return out;
}

void h_glShaderSource(u32 shader, u32 count, u32 strings, u32 lengths)
{
    size_t total = 1;
    for (u32 i = 0; i < count; i++) {
        const char *s = GP(rd32(strings + 4 * i));
        int len = lengths && (int)rd32(lengths + 4 * i) >= 0 ? (int)rd32(lengths + 4 * i) : (int)strlen(s);
        total += (size_t)len;
    }
    char *all = malloc(total), *o = all;
    for (u32 i = 0; i < count; i++) {
        const char *s = GP(rd32(strings + 4 * i));
        int len = lengths && (int)rd32(lengths + 4 * i) >= 0 ? (int)rd32(lengths + 4 * i) : (int)strlen(s);
        memcpy(o, s, (size_t)len);
        o += len;
    }
    *o = 0;
    for (int k = 0; k < nshader_patches; k++) {     /* gles_add_shader_patch: each in turn, NULL = unchanged */
        char *patched = shader_patches[k](all);
        if (patched) {
            free(all);
            all = patched;
        }
    }
    char *t = translate_shader(all);
    const char *one = t;
    p_glShaderSource(shader, 1, GA(&one), 0);
    free(t);
    free(all);
}

void h_glCompileShader(u32 shader)
{
    p_glCompileShader(shader);
    int ok = 0;
    p_glGetShaderiv(shader, GL_COMPILE_STATUS, GA(&ok));
    if (!ok) {
        static char log[4096], src[32768];
        int n = 0;
        p_glGetShaderInfoLog(shader, sizeof log, GA(&n), GA(log));
        p_glGetShaderSource(shader, sizeof src, GA(&n), GA(src));
        rt_log("[gl] shader %u did not compile:\n%s\n---- source ----\n%s\n----\n", shader, log, src);
    }
}

void h_glLinkProgram(u32 prog)
{
    /* the position attribute at 0, as desktop GL draws nothing without attribute 0 on some drivers */
    p_glBindAttribLocation(prog, 0, GA("position"));
    p_glLinkProgram(prog);
    InterlockedIncrement(&uni_gen);      /* STATE_CACHE: a link resets the program's uniforms */
    host_uni_linked(prog);
    int ok = 0;
    p_glGetProgramiv(prog, GL_LINK_STATUS, GA(&ok));
    if (!ok) {
        static char log[4096];
        int n = 0;
        int att = -1;
        p_glGetProgramInfoLog(prog, sizeof log, GA(&n), GA(log));
        p_glGetProgramiv(prog, GL_ATTACHED_SHADERS, GA(&att));
        rt_log("[gl] program %u did not link (%d shaders attached, thread %lu, context %p):\n%s\n", prog, att,
               GetCurrentThreadId(), (void *)gt_gl_current(), log);
    }
}

/* QUERY_FREE (optimisation pass 4): the host's own GL work (eagl.c's present each frame,
   widescreen.c's side bands) saves the state it changes; with glGetIntegerv / glGetFloatv that is 8 + 3 queries
   a frame.
   A query makes a threaded driver finish the commands queued so far first: measured (GT_GL_STATS) 0.04-0.17 ms a
   frame in present and 0.20 ms in the side bands, which run in the middle of a frame.  So each context keeps what
   was last set through the game's calls and the host's binds (gles_bind_fb / gles_bind_rb: every host bind of a
   framebuffer or renderbuffer goes through them); a value not known yet is asked once and kept.  Whatever the
   host changes it restores (present: glPopAttrib and its rebinds; the side bands: their own restore), so what is
   kept stays true.  GT_GL_QUERY_FREE=0: every value asked of the driver.  GT_GL_QUERY_CHECK=1 (testing): both, compared. */
static int qf_on = -1, qf_check;
static int qf_enabled(void)
{
    if (qf_on < 0) {
        const char *e = getenv("GT_GL_QUERY_FREE");
        qf_on = !(e && *e == '0');
        e = dev_getenv("GT_GL_QUERY_CHECK");
        qf_check = qf_on && e && *e == '1';
    }
    return qf_on;
}
void gles_bind_fb(u32 target, u32 fb)
{
    VState *s = vs();
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
        s->tk_rfb = fb, s->tk_known |= TK_RFB;
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
        s->tk_dfb = fb, s->tk_known |= TK_DFB;
    p_glBindFramebuffer(target, fb);
}
void gles_bind_rb(u32 rb)
{
    VState *s = vs();
    s->tk_rb = rb, s->tk_known |= TK_RB;
    p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
}
void h_glBindRenderbuffer(u32 target, u32 rb)
{
    if (target == GL_RENDERBUFFER) {
        gles_bind_rb(rb);
        return;
    }
    p_glBindRenderbuffer(target, rb);
}
void h_glScissor(u32 x, u32 y, u32 w, u32 h)
{
    VState *s = vs();
    s->tk_box[0] = x, s->tk_box[1] = y, s->tk_box[2] = w, s->tk_box[3] = h, s->tk_known |= TK_BOX;
    p_glScissor(x, y, w, h);
}
void h_glClearColor(float r, float g, float b, float a)
{
    VState *s = vs();
    s->tk_clear[0] = r, s->tk_clear[1] = g, s->tk_clear[2] = b, s->tk_clear[3] = a, s->tk_known |= TK_CLEAR;
    p_glClearColor(r, g, b, a);
}

/* the current value of `pname` (one of those below) into out: kept, or asked (then kept) */
void gles_state(u32 pname, void *out)
{
    VState *s = vs();
    u32 *o = out;
    u32 bit, n = 1, *k;
    switch (pname) {
    case GL_CURRENT_PROGRAM: bit = TK_PROG, k = &s->tk_prog; break;
    case GL_ACTIVE_TEXTURE: bit = TK_UNIT, k = &s->tk_unit; break;
    case GL_READ_FRAMEBUFFER_BINDING: bit = TK_RFB, k = &s->tk_rfb; break;
    case GL_FRAMEBUFFER_BINDING: bit = TK_DFB, k = &s->tk_dfb; break;                   /* GL_DRAW_FRAMEBUFFER_BINDING */
    case GL_RENDERBUFFER_BINDING: bit = TK_RB, k = &s->tk_rb; break;
    case GL_SCISSOR_TEST: bit = TK_SCISSOR_ON, k = &s->tk_scissor_on; break;
    case GL_SCISSOR_BOX: bit = TK_BOX, k = s->tk_box, n = 4; break;
    case GL_COLOR_CLEAR_VALUE: bit = TK_CLEAR, k = (u32 *)s->tk_clear, n = 4; break;  /* GL_COLOR_CLEAR_VALUE (floats) */
    case GL_TEXTURE_BINDING_2D: bit = TK_TEX0, k = &s->tk_tex0; break;                 /* GL_TEXTURE_BINDING_2D of unit 0 */
    case GL_ARRAY_BUFFER_BINDING: *o = s->array_buf; return;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: *o = s->elem_buf; return;
    default:
        p_glGetIntegerv(pname, GA(o));
        return;
    }
    if (qf_enabled() && (s->tk_known & bit) && !qf_check) {
        memcpy(o, k, n * 4);
        return;
    }
    u32 v[4] = {0};
    if (pname == GL_COLOR_CLEAR_VALUE)
        p_glGetFloatv(pname, GA(v));
    else if (pname == GL_TEXTURE_BINDING_2D) {                                         /* unit 0's, the active unit kept */
        u32 was = 0;
        p_glGetIntegerv(GL_ACTIVE_TEXTURE, GA(&was));
        p_glActiveTexture(GL_TEXTURE0);
        p_glGetIntegerv(pname, GA(v));
        p_glActiveTexture(was);
    } else
        p_glGetIntegerv(pname, GA(v));
    if (qf_check && (s->tk_known & bit) && memcmp(v, k, n * 4)) {
        static volatile LONG bad;
        if (InterlockedIncrement(&bad) <= 20)
            dev_log("[gl] QUERY_FREE check: 0x%04x kept %u, the driver says %u (frame %u)\n", pname, k[0], v[0],
                   eagl_frames());
    }
    memcpy(o, v, n * 4);
    memcpy(k, v, n * 4);
    s->tk_known |= bit;
}
