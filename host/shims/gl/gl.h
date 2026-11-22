/* gl.h -- pointers to the OpenGL entry points (gl_list.h), loaded by gles_load(): desktop OpenGL 2.1 on Windows,
   OpenGL ES 3.0 on Android (gles.c passes the game's ES calls straight through there; what ES lacks of the list,
   the ES 1.1 fixed-function calls and the desktop ones, gets a stand-in, gles_load). */
#ifndef GT_GL_H
#define GT_GL_H

#ifdef _WIN32
#include <SDL3/SDL_opengl.h>
#define GT_GLAPI __attribute__((stdcall))
#define GT_GL_DESKTOP 1
#else
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#define GT_GLAPI
#define GT_GL_DESKTOP 0
#include "gl_es_names.h"
#endif

#include "arm_rt.h"

/* the context current on this thread (an SDL_GLContext; on Android the EGLContext, which is the same thing) */
#if GT_GL_DESKTOP
#define gt_gl_current() ((void *)SDL_GL_GetCurrentContext())
#else
void *gt_gl_current(void);              /* gles_android.c */
#endif
#define GLP(name, params, args) typedef void(GT_GLAPI *T_##name) params; extern T_##name p_##name;
#define GLR(ret, name, params, args) typedef ret(GT_GLAPI *T_##name) params; extern T_##name p_##name;
#define GLX(ret, name, params) typedef ret(GT_GLAPI *T_##name) params; extern T_##name p_##name;
#include "gl_list.h"

/* OpenGL ES enums the desktop headers lack (the game passes them; gles.c, gles_tex.c translate them) */
#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif
#ifndef GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGB_PVRTC_4BPPV1_IMG  0x8C00
#define GL_COMPRESSED_RGB_PVRTC_2BPPV1_IMG  0x8C01
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG 0x8C02
#define GL_COMPRESSED_RGBA_PVRTC_2BPPV1_IMG 0x8C03
#endif

/* QUERY_FREE (gles.c): the host binds framebuffers / renderbuffers through these, and reads the state it saves
   around its own GL work from gles_state (kept values; asked of the driver only when not known) */
void gles_bind_fb(u32 target, u32 fb);
void gles_bind_rb(u32 rb);
void gles_state(u32 pname, void *out);

/* Shader hooks for the game layer (games/), registered before the first shader is made (a constructor will do):
   - gles_add_shader_patch: called at glShaderSource with the shader's whole ES source; returns a new malloc'd
     source, or NULL to leave it as it is.  Patches run in the order registered.
   - gles_add_host_uniform: a float uniform of that name, in any program that has it (a patch adds it), set to
     value() whenever the program is used and the value changed (at glUseProgram).
   Both return 0 when their table is full. */
int gles_add_shader_patch(char *(*patch)(const char *src));
int gles_add_host_uniform(const char *name, float (*value)(void));
/* what the game layer reads of the GL layer */
u32 gles_draws(void);                   /* draws so far on this thread's context */
const char *gles_gpu_vendor(void);      /* GL_VENDOR / GL_RENDERER, once a context was made */
const char *gles_gpu_renderer(void);
void gles_fault_dump(void);             /* a crash: the last draw's state, to the log */
/* --selftest (core/selftest.c): FIXED_FAST, INDEX_FAST, PVRTC_FAST against the plain loops; 0 = passed */
int gles_selftest_fixed(void), gles_selftest_index(void), gles_selftest_pvrtc(void);

#endif
