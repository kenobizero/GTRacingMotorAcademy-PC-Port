/* gles_tex.c -- the game's textures on desktop OpenGL: the ES-only formats mapped, PVRTC unpacked.

   - glTexImage2D / glRenderbufferStorage: GL_BGRA_EXT, GL_HALF_FLOAT_OES and GL_RGB565_OES as desktop names;
   - glCompressedTexImage2D: PVRTC 1 (GL_IMG_texture_compression_pvrtc, 2 and 4 bits per pixel), which desktop
     drivers do not have, unpacked to RGBA8 (PVRTC_FAST, optimisation pass 6: the same bytes, checked by --selftest);
   - every level made goes to glenhance.c (glx_tex_level: anisotropic filtering).
   Split from gles.c (the calls, the state, the vertex arrays and the shaders). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "gl.h"
#include "gles_internal.h"
#include "ui.h"

/* formats */

static u32 tex_internal(u32 internal, u32 format, u32 type)
{
    (void)type;
    if (internal == GL_BGRA)              /* GL_BGRA_EXT: desktop wants the components */
        return GL_RGBA;
    if (internal == GL_RGBA || internal == GL_RGB || internal == GL_ALPHA || internal == GL_LUMINANCE ||
        internal == GL_LUMINANCE_ALPHA)
        return internal;
    (void)format;
    return internal;
}

void h_glTexImage2D(u32 target, u32 level, u32 internal, u32 w, u32 h, u32 border, u32 format, u32 type, u32 pixels)
{
    if (type == GL_HALF_FLOAT_OES)
        type = GL_HALF_FLOAT;
    p_glTexImage2D(target, level, tex_internal(internal, format, type), w, h, border, format, type, pixels);
    glx_tex_level(target, level);
}

void h_glRenderbufferStorage(u32 target, u32 internal, u32 w, u32 h)
{
    if (internal == GL_RGB565)              /* GL_RGB565_OES */
        internal = GL_RGB8;
    p_glRenderbufferStorage(target, internal, w, h);
}

/* PVRTC (GL_IMG_texture_compression_pvrtc)
   The block layout and colour unpacking of Imagination's PVRTC 1 (as in the PowerVR SDK's
   PVRTDecompress); the per-pixel loop is the Fast Five host's (pvrtc.c), whose output matched a
   reference decoder byte for byte, with its copied quirk undone: colour B's 4-bit blue gets its last
   bit (the Mac game's code set colour A's blue instead). */

typedef struct {
    const u32 *blk[4];
    int col[4][8];                       /* per block: colour A r g b a, colour B r g b a (5/4 bits) */
    u8 val[8 * 16], mode[8 * 16];
} PvState;

static const u8 pv_mod[4] = {0, 3, 5, 8};
static const u8 pv_mod_punch[4] = {0, 4, 4, 8};

static u32 pv_twiddle(u32 y, u32 x, u32 nx, u32 ny)
{
    u32 high = ny < nx ? x : y, side = ny < nx ? ny : nx, r = 0;
    int n = 0;
    if (side >= 2)
        for (u32 bit = 1, shift = 1;; shift <<= 2) {
            if (y & bit)
                r |= shift;
            if (x & bit)
                r |= shift * 2;
            bit *= 2;
            n++;
            if (side <= bit)
                break;
        }
    return ((high >> (n & 31)) << ((n * 2) & 31)) | r;
}

static void pv_unpack(PvState *s, int b, const u32 *blk, int is2bit, int bw)
{
    u32 d = blk[1], bits = blk[0], w = d & 0xfffe, hi = d >> 16, mode = d & 1;
    int *c = s->col[b];
    if (!(d & 0x8000)) {                 /* colour A: ARGB 3443 */
        c[0] = w >> 7 & 0x1e;
        c[1] = w >> 3 & 0x1e;
        c[0] = c[0] >> 4 | c[0];
        c[1] = c[1] >> 4 | c[1];
        c[2] = (int)(d & 0xe) * 2;
        c[2] = c[2] >> 3 | c[2];
        c[3] = w >> 11 & 0xe;
    } else {                             /* RGB 554 */
        c[0] = w >> 10 & 0x1f;
        c[1] = w >> 5 & 0x1f;
        c[2] = (int)((d & 0x1e) >> 4 | (d & 0x1e));
        c[3] = 0xf;
    }
    if (hi & 0x8000) {                   /* colour B: RGB 555 */
        c[4] = d >> 26 & 0x1f;
        c[5] = d >> 21 & 0x1f;
        c[6] = hi & 0x1f;
        c[7] = 0xf;
    } else {                             /* ARGB 3444 */
        int r = d >> 23 & 0x1e, g = d >> 19 & 0x1e;
        c[4] = r >> 4 | r;
        c[5] = g >> 4 | g;
        c[6] = (int)(hi & 0xf) * 2;
        c[6] = c[6] >> 4 | c[6];
        c[7] = d >> 27 & 0xe;
    }
    u8 *val = s->val + (b >> 1) * 4 * 16 + (b & 1) * bw;
    u8 *mod = s->mode + (b >> 1) * 4 * 16 + (b & 1) * bw;
    if (!is2bit) {
        for (int y = 0; y < 4; y++, val += 16, mod += 16)
            for (int x = 0; x < 4; x++, bits >>= 2) {
                mod[x] = (u8)mode;
                val[x] = bits & 3;
            }
    } else if (!mode) {
        for (int y = 0; y < 4; y++, val += 16, mod += 16)
            for (int x = 0; x < 8; x++, bits >>= 1) {
                mod[x] = 0;
                val[x] = bits & 1 ? 3 : 0;
            }
    } else {
        for (int y = 0; y < 4; y++, val += 16, mod += 16)
            for (int x = 0; x < 8; x++) {
                mod[x] = 1;
                if (!((x ^ y) & 1)) {
                    val[x] = bits & 3;
                    bits >>= 2;
                }
            }
    }
}

/* rows y0 .. y1-1 of the picture (each row depends only on the data: PVRTC_FAST decodes bands of rows at once) */
static void pv_decode_rows(const u8 *data, int is2bit, int w, int h, u8 *out, int y0, int y1)
{
    const int bw = is2bit ? 8 : 4, full = bw;
    const int rgb_shift = is2bit ? 2 : 1, a_shift = is2bit ? 1 : 0;
    const u32 nx = (u32)(w / bw) < 2 ? 2 : (u32)(w / bw);
    const u32 ny = h < 8 ? 2 : (u32)(h >> 2);
    PvState s;
    memset(s.blk, 0, sizeof s.blk);
    for (int y = y0; y < y1; y++) {
        const u32 by = (((u32)y - 2) & ((u32)h - 1)) >> 2, by1 = (by + 1) & (ny - 1);
        const int row = ((~y & 2) * 2 | (y & 3));
        const int v = row - 2;
        u8 *o = out + (size_t)y * (size_t)w * 4;
        u32 last_bx = (u32)-1;
        for (int x = 0; x < w; x++, o += 4) {
            const u32 bx = (u32)((int)(((u32)w - 1) & (u32)(x - bw / 2)) / bw);
            if (bx != last_bx) {
                const u32 bx1 = (bx + 1) & (nx - 1);
                const u32 *b[4] = {
                    (const u32 *)(data + pv_twiddle(by, bx, nx, ny) * 8u),
                    (const u32 *)(data + pv_twiddle(by, bx1, nx, ny) * 8u),
                    (const u32 *)(data + pv_twiddle(by1, bx, nx, ny) * 8u),
                    (const u32 *)(data + pv_twiddle(by1, bx1, nx, ny) * 8u),
                };
                last_bx = bx;
                if (memcmp(b, s.blk, sizeof b)) {
                    memcpy(s.blk, b, sizeof b);
                    for (int k = 0; k < 4; k++)
                        pv_unpack(&s, k, b[k], is2bit, bw);
                }
            }
            const int col = is2bit ? ((~x & 4) * 2 | (x & 7)) : ((~x & 2) * 2 | (x & 3));
            const int u = col - (is2bit ? 4 : 2);
            int c[8];
            for (int k = 0; k < 8; k++) {
                const int top = (s.col[1][k] - s.col[0][k]) * u + s.col[0][k] * full;
                const int bot = (s.col[3][k] - s.col[2][k]) * u + s.col[2][k] * full;
                c[k] = (bot - top) * v + top * 4;
            }
            for (int k = 0; k < 8; k++)
                if ((k & 3) == 3) {
                    c[k] >>= a_shift;
                    c[k] += c[k] >> 4;
                } else {
                    c[k] >>= rgb_shift;
                    c[k] += c[k] >> 5;
                }
            const int i = row * 16 + col;
            int m, punch = 0;
            const u8 *V = s.val;
            if (s.mode[i] && !is2bit) {
                m = pv_mod_punch[V[i]];
                punch = V[i] == 2;
            } else if (s.mode[i] && ((row ^ col) & 1)) {
                m = (pv_mod[V[i + 16]] + pv_mod[V[i - 16]] + 2 + pv_mod[V[i - 1]] + pv_mod[V[i + 1]]) >> 2;
            } else {
                m = pv_mod[V[i]];
            }
            o[0] = (u8)(((c[4] - c[0]) * m + c[0] * 8) >> 3);
            o[1] = (u8)(((c[5] - c[1]) * m + c[1] * 8) >> 3);
            o[2] = (u8)(((c[6] - c[2]) * m + c[2] * 8) >> 3);
            o[3] = punch ? 0 : (u8)(((c[7] - c[3]) * m + c[3] * 8) >> 3);
        }
    }
}
static void pv_decode(const u8 *data, int is2bit, int w, int h, u8 *out) { pv_decode_rows(data, is2bit, w, h, out, 0, h); }

/* PVRTC_FAST (optimisation pass 6; verified byte-exact by --selftest)
   Why.  The PVRTC textures are unpacked to RGBA on the CPU (desktop GPUs have no PVRTC): ~180 textures, 13.4 million
   pixels, 0.61 s of every race's loading on the loader thread (test save, France Raceway), on one core.  Not
   a fault of the game or of the port.
   What.  A picture of 64 K pixels or more is cut into bands of rows, decoded at once by the Windows thread pool and
   the calling thread (each band by pv_decode_rows, the same code: the bytes are the same; --selftest decodes random
   pictures both ways and compares them byte for byte).  At most 6 helpers and one fewer than the CPU's cores, so
   the render thread keeps a core.  Two threads decoding at once each use the pool; a pool that cannot take work
   leaves the calling thread to decode alone.
   GT_PVRTC_FAST=0: one thread. */
static int pvf_on = -1, pvf_threads;
static int pvf_enabled(void)
{
    if (pvf_on < 0) {
        const char *e = getenv("GT_PVRTC_FAST");
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int n = (int)si.dwNumberOfProcessors - 1;
        pvf_threads = n > 6 ? 6 : n;
        pvf_on = !(e && *e == '0') && pvf_threads >= 1;
        rt_log("[gl] PVRTC_FAST: %s (%d helper threads)\n", pvf_on ? "on" : e && *e == '0' ? "off (GT_PVRTC_FAST=0)" :
               "off (one core)", pvf_on ? pvf_threads : 0);
    }
    return pvf_on;
}
typedef struct {
    const u8 *data;
    u8 *out;
    int is2bit, w, h, band, nbands;
    volatile LONG next, refs;
    HANDLE done;
} PvJob;
static void pv_bands(PvJob *j)
{
    for (;;) {
        int b = (int)InterlockedIncrement(&j->next) - 1;
        if (b >= j->nbands)
            return;
        int y0 = b * j->band, y1 = y0 + j->band > j->h ? j->h : y0 + j->band;
        pv_decode_rows(j->data, j->is2bit, j->w, j->h, j->out, y0, y1);
    }
}
static void CALLBACK pv_worker(PTP_CALLBACK_INSTANCE inst, void *arg)
{
    (void)inst;
    PvJob *j = arg;
    pv_bands(j);
    if (!InterlockedDecrement(&j->refs))  /* the last one out: the caller may go on (it does not touch j after) */
        SetEvent(j->done);
}
static void pv_decode_fast(const u8 *data, int is2bit, int w, int h, u8 *out, int threads)
{
    PvJob j;
    j.data = data, j.out = out, j.is2bit = is2bit, j.w = w, j.h = h;
    j.band = 16;                         /* rows a band: four block rows */
    j.nbands = (h + j.band - 1) / j.band;
    j.next = 0;
    if (threads > j.nbands - 1)
        threads = j.nbands - 1;
    j.done = threads > 0 ? CreateEventA(NULL, TRUE, FALSE, NULL) : NULL;
    j.refs = 1;
    for (int i = 0; j.done && i < threads; i++) {
        InterlockedIncrement(&j.refs);
        if (!TrySubmitThreadpoolCallback(pv_worker, &j, NULL)) {
            InterlockedDecrement(&j.refs);
            break;
        }
    }
    pv_bands(&j);
    if (InterlockedDecrement(&j.refs))
        WaitForSingleObject(j.done, INFINITE);
    if (j.done)
        CloseHandle(j.done);
}
static struct { u32 n; double px, us; } pv_st;   /* GT_GL_STATS: pictures, pixels, time decoding */
static void pv_decode_any(const u8 *data, int is2bit, int w, int h, u8 *out)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if ((size_t)w * (size_t)h >= 65536 && pvf_enabled())
        pv_decode_fast(data, is2bit, w, h, out, pvf_threads);
    else
        pv_decode(data, is2bit, w, h, out);
    QueryPerformanceCounter(&t1);
    pv_st.n++, pv_st.px += (double)w * h, pv_st.us += (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart;
}

/* GT_GL_STATS: the PVRTC pictures unpacked since the last line (loading) */
void gles_pvrtc_stats(void)
{
    if (pv_st.n)
        rt_log("[gl] PVRTC: %u pictures, %.1f million pixels unpacked in %.0f ms (PVRTC_FAST %s)\n", pv_st.n,
               pv_st.px / 1e6, pv_st.us / 1000, pvf_on > 0 ? "on" : "off");
    memset(&pv_st, 0, sizeof pv_st);
}

/* --selftest: random PVRTC pictures (4 and 2 bits a pixel, square, wide, tall, the smallest) decoded by one thread
   and by bands on the pool, the bytes compared; the number of pictures that differ */
int gles_selftest_pvrtc(void)
{
    static const int sz[][2] = {{8, 8}, {16, 8}, {32, 32}, {64, 32}, {32, 128}, {256, 256}, {512, 128}, {1024, 512}};
    u32 x = 0x2545f491u;
    int bad = 0;
    for (unsigned k = 0; k < sizeof sz / sizeof sz[0]; k++)
        for (int is2bit = 0; is2bit < 2; is2bit++) {
            int w = sz[k][0] < (is2bit ? 16 : 8) ? (is2bit ? 16 : 8) : sz[k][0], h = sz[k][1];
            size_t bytes = (size_t)w * (size_t)h / (is2bit ? 4 : 2) + 64, px = (size_t)w * (size_t)h * 4;
            u8 *data = malloc(bytes), *a = malloc(px), *b = malloc(px);
            if (!data || !a || !b) {
                bad++;
            } else {
                for (size_t i = 0; i < bytes; i++)
                    data[i] = (u8)st_rand(&x);
                memset(a, 0x11, px);
                memset(b, 0x22, px);
                pv_decode(data, is2bit, w, h, a);
                pv_decode_fast(data, is2bit, w, h, b, 6);
                bad += memcmp(a, b, px) != 0;
            }
            free(data), free(a), free(b);
        }
    return bad;
}

void h_glCompressedTexImage2D(u32 target, u32 level, u32 internal, u32 w, u32 h, u32 border, u32 size, u32 data)
{
    if (internal >= GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG && internal <= GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG) {
        int is2bit = internal & 1;
        /* PVRTC pictures are at least 2 x 2 blocks: unpack that much, upload the corner */
        int pw = (int)w, ph = (int)h, minw = is2bit ? 16 : 8;
        if (pw < minw)
            pw = minw;
        if (ph < 8)
            ph = 8;
        u8 *rgba = malloc((size_t)pw * (size_t)ph * 4);
        if (!rgba || !data) {
            free(rgba);
            return;
        }
        pv_decode_any(GP(data), is2bit, pw, ph, rgba);
        if ((int)w != pw)
            for (u32 y = 1; y < h; y++)
                memmove(rgba + y * w * 4, rgba + y * (u32)pw * 4, w * 4);
        u32 align;
        p_glGetIntegerv(GL_UNPACK_ALIGNMENT, GA(&align));
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        p_glTexImage2D(target, level, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, GA(rgba));
        p_glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        free(rgba);
        glx_tex_level(target, level);
        return;
    }
    rt_log("[gl] compressed texture format 0x%x (%ux%u) is not supported\n", internal, w, h);
    p_glCompressedTexImage2D(target, level, internal, w, h, border, size, data);
}
