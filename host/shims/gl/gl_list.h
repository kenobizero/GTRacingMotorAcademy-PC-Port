/* gl_list.h -- the desktop OpenGL entry points the host uses (gles.c loads them, eagl.c draws with
   some).  Integers, enums and pointers are all 32-bit words on this host, so they are written u32;
   APIENTRY (stdcall) is in the pointer types.

   GLP(name, params, args)        void, passed through as the game's h_<name>
   GLR(ret, name, params, args)   returns a value, passed through
   GLX(ret, name, params)         loaded only: gles.c writes h_<name> itself, or the host uses it */
#ifndef GLX
#define GLX(ret, name, params)
#endif

/* straight from OpenGL ES 1.1 / 2.0 to desktop OpenGL 2.1 (compatibility) */
GLX(void, glActiveTexture, (u32 a))                    /* STATE_CACHE (gles.c) */
GLP(glAlphaFunc, (u32 a, float b), (a, b))
GLP(glAttachShader, (u32 a, u32 b), (a, b))
GLX(void, glBindRenderbuffer, (u32 a, u32 b))          /* QUERY_FREE (gles.c) */
GLX(void, glBindTexture, (u32 a, u32 b))               /* STATE_CACHE */
GLX(void, glBlendFunc, (u32 a, u32 b))                 /* STATE_CACHE */
GLX(void, glBlendFuncSeparate, (u32 a, u32 b, u32 c_, u32 d))   /* STATE_CACHE */
GLP(glBufferData, (u32 a, u32 b, u32 c_, u32 d), (a, b, c_, d))
GLR(u32, glCheckFramebufferStatus, (u32 a), (a))
GLP(glClear, (u32 a), (a))
GLX(void, glClearColor, (float a, float b, float c_, float d))   /* QUERY_FREE */
GLP(glClearStencil, (u32 a), (a))
GLP(glClientActiveTexture, (u32 a), (a))
GLP(glColor4f, (float a, float b, float c_, float d), (a, b, c_, d))
GLP(glColor4ub, (u32 a, u32 b, u32 c_, u32 d), (a, b, c_, d))
GLX(void, glColorMask, (u32 a, u32 b, u32 c_, u32 d))   /* STATE_CACHE */
GLP(glColorPointer, (u32 a, u32 b, u32 c_, u32 d), (a, b, c_, d))
GLR(u32, glCreateProgram, (void), ())
GLR(u32, glCreateShader, (u32 a), (a))
GLP(glCullFace, (u32 a), (a))
GLX(void, glDeleteProgram, (u32 a))                    /* STATE_CACHE */
GLP(glDeleteShader, (u32 a), (a))
GLX(void, glDeleteTextures, (u32 a, u32 b))            /* STATE_CACHE */
GLP(glDepthFunc, (u32 a), (a))
GLX(void, glDepthMask, (u32 a))                        /* STATE_CACHE */
GLX(void, glDisable, (u32 a))                          /* STATE_CACHE */
GLX(void, glDisableClientState, (u32 a))               /* ARRAY_CACHE (gles.c) */
GLX(void, glEnable, (u32 a))                           /* STATE_CACHE */
GLX(void, glEnableClientState, (u32 a))                /* ARRAY_CACHE (gles.c) */
GLP(glFlush, (void), ())
GLP(glFogf, (u32 a, float b), (a, b))
GLP(glFogfv, (u32 a, u32 b), (a, b))
GLP(glFrontFace, (u32 a), (a))
GLX(void, glGenBuffers, (u32 a, u32 b))                /* STATE_CACHE: a new name may be an old one */
GLP(glGenFramebuffers, (u32 a, u32 b), (a, b))
GLP(glGenRenderbuffers, (u32 a, u32 b), (a, b))
GLP(glGenTextures, (u32 a, u32 b), (a, b))
GLP(glGetActiveAttrib, (u32 a, u32 b, u32 c_, u32 d, u32 e, u32 f, u32 g), (a, b, c_, d, e, f, g))
GLP(glGetActiveUniform, (u32 a, u32 b, u32 c_, u32 d, u32 e, u32 f, u32 g), (a, b, c_, d, e, f, g))
GLR(u32, glGetAttribLocation, (u32 a, u32 b), (a, b))
GLR(u32, glGetError, (void), ())
GLP(glGetFloatv, (u32 a, u32 b), (a, b))
GLP(glGetProgramInfoLog, (u32 a, u32 b, u32 c_, u32 d), (a, b, c_, d))
GLP(glGetProgramiv, (u32 a, u32 b, u32 c_), (a, b, c_))
GLP(glGetShaderInfoLog, (u32 a, u32 b, u32 c_, u32 d), (a, b, c_, d))
GLP(glGetShaderiv, (u32 a, u32 b, u32 c_), (a, b, c_))
GLR(u32, glGetUniformLocation, (u32 a, u32 b), (a, b))
GLP(glHint, (u32 a, u32 b), (a, b))
GLP(glLightModelfv, (u32 a, u32 b), (a, b))
GLP(glLightf, (u32 a, u32 b, float c_), (a, b, c_))
GLP(glLightfv, (u32 a, u32 b, u32 c_), (a, b, c_))
GLP(glLineWidth, (float a), (a))
GLP(glLoadIdentity, (void), ())
GLP(glLoadMatrixf, (u32 a), (a))
GLP(glMaterialf, (u32 a, u32 b, float c_), (a, b, c_))
GLP(glMaterialfv, (u32 a, u32 b, u32 c_), (a, b, c_))
GLP(glMatrixMode, (u32 a), (a))
GLP(glMultMatrixf, (u32 a), (a))
GLP(glNormalPointer, (u32 a, u32 b, u32 c_), (a, b, c_))
GLP(glPixelStorei, (u32 a, u32 b), (a, b))
GLP(glPointSize, (float a), (a))
GLP(glPopMatrix, (void), ())
GLP(glPushMatrix, (void), ())
GLP(glRotatef, (float a, float b, float c_, float d), (a, b, c_, d))
GLX(void, glScissor, (u32 a, u32 b, u32 c_, u32 d))   /* QUERY_FREE */
GLP(glStencilFunc, (u32 a, u32 b, u32 c_), (a, b, c_))
GLP(glStencilMask, (u32 a), (a))
GLP(glStencilOp, (u32 a, u32 b, u32 c_), (a, b, c_))
GLP(glTexCoordPointer, (u32 a, u32 b, u32 c_, u32 d), (a, b, c_, d))
GLP(glTexEnvf, (u32 a, u32 b, float c_), (a, b, c_))
GLP(glTexEnvfv, (u32 a, u32 b, u32 c_), (a, b, c_))
GLP(glTexEnvi, (u32 a, u32 b, u32 c_), (a, b, c_))
GLP(glTexSubImage2D, (u32 a, u32 b, u32 c_, u32 d, u32 e, u32 f, u32 g, u32 h, u32 i), (a, b, c_, d, e, f, g, h, i))
/* the uniforms: STATE_CACHE (gles.c, UNI_* below) */
GLX(void, glUniform1f, (u32 a, float b))
GLX(void, glUniform1fv, (u32 a, u32 b, u32 c_))
GLX(void, glUniform1i, (u32 a, u32 b))
GLX(void, glUniform1iv, (u32 a, u32 b, u32 c_))
GLX(void, glUniform2f, (u32 a, float b, float c_))
GLX(void, glUniform2fv, (u32 a, u32 b, u32 c_))
GLX(void, glUniform2i, (u32 a, u32 b, u32 c_))
GLX(void, glUniform2iv, (u32 a, u32 b, u32 c_))
GLX(void, glUniform3f, (u32 a, float b, float c_, float d))
GLX(void, glUniform3fv, (u32 a, u32 b, u32 c_))
GLX(void, glUniform3i, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glUniform3iv, (u32 a, u32 b, u32 c_))
GLX(void, glUniform4f, (u32 a, float b, float c_, float d, float e))
GLX(void, glUniform4fv, (u32 a, u32 b, u32 c_))
GLX(void, glUniform4i, (u32 a, u32 b, u32 c_, u32 d, u32 e))
GLX(void, glUniform4iv, (u32 a, u32 b, u32 c_))
GLX(void, glUniformMatrix2fv, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glUniformMatrix3fv, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glUniformMatrix4fv, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glUseProgram, (u32 a))                       /* STATE_CACHE */
GLP(glViewport, (u32 a, u32 b, u32 c_, u32 d), (a, b, c_, d))

/* the game's that need work (gles.c), and what the host itself draws with */
GLX(void, glBindAttribLocation, (u32 a, u32 b, u32 c_))
GLX(void, glClearDepth, (double a))
GLX(void, glCompileShader, (u32 a))
GLX(void, glCompressedTexImage2D, (u32 a, u32 b, u32 c_, u32 d, u32 e, u32 f, u32 g, u32 h))
GLX(void, glGetIntegerv, (u32 a, u32 b))
GLX(u32, glGetString, (u32 a))
GLX(void, glLinkProgram, (u32 a))
GLX(u32, glMapBuffer, (u32 a, u32 b))
GLX(void, glRenderbufferStorage, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glShaderSource, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glTexImage2D, (u32 a, u32 b, u32 c_, u32 d, u32 e, u32 f, u32 g, u32 h, u32 i))
GLX(u32, glUnmapBuffer, (u32 a))
GLX(void, glVertexAttribPointer, (u32 a, u32 b, u32 c_, u32 d, u32 e, u32 f))
GLX(void, glVertexPointer, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glBindBuffer, (u32 a, u32 b))
GLX(void, glDeleteBuffers, (u32 a, u32 b))
GLX(u32, glIsBuffer, (u32 a))
GLX(void, glGetVertexAttribiv, (u32 a, u32 b, u32 c_))
GLX(void, glEnableVertexAttribArray, (u32 a))
GLX(void, glDisableVertexAttribArray, (u32 a))
GLX(void, glDrawArrays, (u32 a, u32 b, u32 c_))
GLX(void, glDrawElements, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glGetBufferSubData, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glBegin, (u32 a))
GLX(void, glEnd, (void))
GLX(void, glTexCoord2f, (float a, float b))
GLX(void, glVertex2f, (float a, float b))
GLX(void, glPushAttrib, (u32 a))
GLX(void, glPopAttrib, (void))
GLX(void, glPushClientAttrib, (u32 a))
GLX(void, glPopClientAttrib, (void))
GLX(void, glBlitFramebuffer, (u32 a, u32 b, u32 c_, u32 d, u32 e, u32 f, u32 g, u32 h, u32 i, u32 j))
GLX(void, glGetShaderSource, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glFinish, (void))

/* glenhance.c: MSAA and anisotropic filtering */
GLX(void, glBindFramebuffer, (u32 a, u32 b))
GLX(void, glFramebufferRenderbuffer, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glFramebufferTexture2D, (u32 a, u32 b, u32 c_, u32 d, u32 e))
GLX(void, glGenerateMipmap, (u32 a))
GLX(void, glGetFramebufferAttachmentParameteriv, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glReadPixels, (u32 a, u32 b, u32 c_, u32 d, u32 e, u32 f, u32 g))
GLX(void, glTexParameterf, (u32 a, u32 b, float c_))
GLX(void, glTexParameteri, (u32 a, u32 b, u32 c_))
GLX(void, glRenderbufferStorageMultisample, (u32 a, u32 b, u32 c_, u32 d, u32 e))
GLX(u32, glIsTexture, (u32 a))

#if !GT_GL_DESKTOP
/* OpenGL ES 3 (Android): what the stand-ins for the desktop calls above use (gles.c), and present's quad (eagl.c) */
GLX(void, glClearDepthf, (float a))
GLX(u32, glMapBufferRange, (u32 a, u32 b, u32 c_, u32 d))
GLX(void, glGetBufferParameteriv, (u32 a, u32 b, u32 c_))
GLX(void, glGenVertexArrays, (u32 a, u32 b))
GLX(void, glBindVertexArray, (u32 a))
GLX(u32, glIsEnabled, (u32 a))
GLX(void, glGetBooleanv, (u32 a, u32 b))
#endif

#undef GLP
#undef GLR
#undef GLX
