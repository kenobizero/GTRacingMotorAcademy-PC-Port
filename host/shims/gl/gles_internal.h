/* gles_internal.h -- what the parts of the OpenGL ES layer share: gles.c (the calls, the state, the vertex arrays,
   the shaders) and gles_tex.c (the texture formats, PVRTC). */
#ifndef GT_GLES_INTERNAL_H
#define GT_GLES_INTERNAL_H

#include "abi.h"
#include "game_hooks.h"

/* the contexts (gles.c, for eagl.c) */
extern int gles_api;                    /* the API of the last EAGLContext made: 1 or 2 */
void gles_context_created(void);
void gles_context_current(void *gl);
void gles_context_destroyed(void *gl);
void gles_state_forget(void);           /* the shadowed state is unknown (after the host's own GL work) */
void gles_pvrtc_stats(void);            /* gles_tex.c: GT_GL_STATS's PVRTC line */

#ifndef _WIN32
/* Android (gles_android.c): EAGLContext's contexts on EGL, and present()'s quad on OpenGL ES 3 */
#include <SDL3/SDL_video.h>
SDL_GLContext gla_create(void);         /* the first: SDL's; later ones share with it */
void gla_destroy(SDL_GLContext c);
int gla_make_current(SDL_GLContext c);  /* NULL: none; with the window when no other thread has it */
int gla_present_begin(void);            /* 0: nothing can be shown now */
void gla_swap(void);
void gla_swap_interval(int n);
void gla_quad_save(void);
void gla_quad_draw(u32 tex, const float *vx, const float *vy, int W, int H);
void gla_quad_restore(void);
#endif

/* --selftest's random bytes (xorshift32): the same sequence on every run */
static inline u32 st_rand(u32 *x)
{
    *x ^= *x << 13, *x ^= *x >> 17, *x ^= *x << 5;
    return *x;
}

#endif
