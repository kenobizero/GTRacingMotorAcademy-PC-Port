/* postfx.c -- POSTFX, the picture's post effects: GT_POSTFX=1 the game's own bloom, switched off in the
   shipped build; GT_POSTFX=2 Classic, a bloom drawn as the paid iPad version (GT Racing 1.1.4, 2010) draws it;
   GT_POSTFX=3 Classic Natural, the same with its colours made grey (neutral(), below); GT_POSTFX=4 Enhanced: the
   game's own bloom balanced (below) with Classic Natural over it, the default.  0: none, as the free build
   shipped.
   2, 3 and 4 also behind the menus' 3D scene (__wrap_F_14a660, below).
   Options > Display > PostFX: Off (0), Classic (2), Classic Natural (3), Enhanced (4).

   1, the game's own bloom.  The original (read from the code): F_28d6c0 is "mov r0, #0; bx lr", a gate that is
   always false.  It is asked by F_2a4f38 (the post effects' render targets) and seven times by F_16f3fc (the
   frame's post effect passes).  The bundle still has the whole pipeline: gtr_preBloomCar /
   gtr_preBloomStaticObject (the specular highlights only), gtr_preBloom (bright parts), gtr_BloomBlur (7-tap
   blur), gtr_PostBloom (screen x2 + the blurred highlights).  The generated F_28d6c0 returns gt_postfx_gate()
   instead of 0 (its caller F_2a4f38 sits in the same generated file, so a linker --wrap cannot reach it).

   4, Enhanced: the game's own bloom balanced (an enhancement of 1), then Classic Natural (3, below) over it.
   1 looks too bright and pinkish red; the causes are in the shipped shaders (read from the bundle, every byte
   inverted):
   - gtr_preBloom keeps a pixel when (r + g + a) / 3 > 0.9: alpha, not blue, so with alpha 1 any pixel whose red
     and green add up to 1.7 glows, red and orange ones included (in the bundle; a race never compiles it);
   - gtr_preBloomStaticObject / gtr_preBloomCar draw the highlights in the colour 1.28, 1, 1 (red);
   - gtr_BloomBlur's weights add up to 3.462 but are divided by 3, so each of its passes brightens by 15 %;
   - gtr_PostBloom draws texture0 x 2 + texture1.
   With 4 the same shaders read a host uniform, gt_balance (gles.c sets it at glUseProgram, gt_shader_balance):
   luminance from r, g and b, white highlights, a blur that keeps the brightness, texture0 x 1, and the strengths
   measured on a sunlit banked turn (BAL_STATIC, BAL_CAR, BAL_POST, BAL_SCREEN, below).  The source is patched once
   (gt_shader_patch), with the original's terms kept for gt_balance 0, so 1 and 4 can be switched at once.  The
   game's own targets are made whatever the mode (gt_postfx_targets: F_2a4f38 makes the post effect object's
   targets once, as the first race is got ready, and asks the gate for them) and its shaders the first time a pass
   draws them (F_150ce4), so every mode, the game's own part included, is taken on at once from the menu.
   Enhanced draws the Classic Natural part after the game's own composite (gtr_PostBloom, drawn by F_16f3fc after
   F_2a36e8: 0x2a3fc0 / 0x2a3fdc / 0x2a37dc), at F_14a660 (return address 0x1700b4), the call before the HUD that
   both of F_16f3fc's paths reach.  Both glows add up over one picture, Enhanced's tone (enhanced_tone, below): the
   Classic Natural part with a higher threshold, its glow in its own colour and added as a screen, and the scene a
   little more saturated, so the lights, the sky and the highlights glow while the shadows stay as deep and the
   colours as rich as Off's and no bright surface goes to flat white.  The menus' 3D scene has no game's own pass (no car highlights): Enhanced gives it its own settings
   (ENHANCED_MENU, below).

   2, the 2010 bloom.  The paid iPad build (1.1.4) and the iPhone build (1.4.4, 2009) draw it with OpenGL ES 1.1, in
   every race (read from their code; iPad: end 0x1bd628, settings copied at 0x84a28, on while the byte 0x471eb1, 1,
   is set: nothing writes it; iPhone: begin 0x1617fc, end 0x161a08, blur 0x160ae4).  All three builds read the
   same bloom settings from every track's .ba2 (track script commands 0x208 / 0x209, the same bytes): a bloom tint,
   a threshold and a scene tint (RGB each), a blur radius and a blend mode.  The free build's F_16f3fc still copies
   them into its post effect object each race frame (+0xdc..+0xf6, the same layout), but its GLSL bloom reads only
   the on / off byte +0xf6.  The 2010 steps:
   - the scene into a target the screen's size rounded up to a power of two (iPad 0x1be6a4: 1024 x 1024);
   - bright pass into half that (512 x 512): the scene minus the threshold (texture environment SUBTRACT), so
     everything a bit brighter than it glows, not only the highlights;
   - blurred twice, into a quarter (256 x 256, twice): 0.2 of the texel itself + 0.2 of each of four texels at
     (+-1.5, +-0.5) and (-+0.5, +-1.5) texels x the radius;
   - the screen: the scene x the scene tint, then + the blurred texture x the bloom tint (mode 2, glBlendFunc
     (ONE, ONE); mode 4, never in the tracks' data: (texture + tint) with glBlendFunc(SRC_COLOR, ZERO)).
   The iPhone's are the same at half the size (512 / 256 / 128 / 128 for its 480 x 320 screen), so its glow is
   twice as wide.  Each target covers the whole screen, so on a 1024 x 768 iPad a blur texel is 1/256 of the
   screen's width and height: here the bright pass is 512 x 512 and the blurs 256 x 256 at any resolution, the
   glow as wide on the screen as the iPad's.
   The steps run in GLSL at F_2a36e8, an empty function (bx lr) F_16f3fc calls after the race's 3D picture and
   before the HUD, with the object it is given.  The scene comes from the frame's framebuffer (resolved by a blit
   with MSAA), mipmapped so the bright pass reads it box-filtered on a large screen.  The game's own bloom stays
   off.  An enhancement, not a port fault: the free build shipped without any bloom. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"
#include "rt.h"
#include "features.h"
#include "game.h"

static int mode_ = -1, making;

#define GAME_OWN(m) ((m) == 1 || (m) == 4)      /* the game's own pipeline (its gate F_28d6c0) */
#define POSTFX_DEFAULT 4                         /* Enhanced; GT_POSTFX=0 Off */

static int gt_postfx_mode(void)
{
    if (mode_ < 0) {
        const char *e = getenv("GT_POSTFX");
        mode_ = e && *e >= '0' && *e <= '4' && !e[1] ? *e - '0' : POSTFX_DEFAULT;
#if !GT_GL_DESKTOP
        if (mode_ >= 2) {               /* Android: the host's own passes are desktop OpenGL; the game's bloom only */
            rt_log("[postfx] POSTFX=%d needs desktop OpenGL: off on OpenGL ES\n", mode_);
            mode_ = 0;
        }
#endif
        rt_log(mode_ == 0   ? "[postfx] off (POSTFX=0)\n"
               : mode_ == 1 ? "[postfx] the game's own bloom on (POSTFX=1)\n"
               : mode_ == 2 ? "[postfx] the iPad 2010 bloom on (POSTFX=2)\n"
               : mode_ == 3 ? "[postfx] the iPad 2010 bloom, the world's colours kept (POSTFX=3)\n"
                            : "[postfx] Enhanced: the game's own bloom balanced, and Classic Natural (POSTFX=4)\n");
    }
    return mode_;
}

/* the value chosen last (the one in gtracing.ini): every mode now takes effect at once */
int gt_postfx_wanted(void) { return gt_postfx_mode(); }

/* the 2010 bloom (2, 3 and Enhanced's part) is drawn by the host */
static int classic(void)
{
    int m = gt_postfx_mode();
    return m >= 2 && m <= 4;
}

/* Options > Display > PostFX (display.c): Off (0), Classic (2), Classic Natural (3) or Enhanced (4), and the game's
   own bloom (1, set in the file), all at once: the game's own bloom's render targets are made whatever the mode
   (gt_postfx_targets) and its shaders the first time a pass draws them. */
void gt_postfx_set(int m)
{
    gt_postfx_mode();
    mode_ = m >= 0 && m <= 4 ? m : 0;
#if !GT_GL_DESKTOP
    if (mode_ >= 2)
        mode_ = 1;                      /* Android: Classic / Enhanced are desktop OpenGL; the game's own bloom */
#endif
    rt_log("[postfx] %s (Options > Display)\n", mode_ == 4   ? "Enhanced"
                                                 : mode_ == 3 ? "Classic Natural"
                                                 : mode_ == 2 ? "Classic"
                                                 : mode_ == 1 ? "the game's own bloom"
                                                              : "off");
}

/* fullblur.c's __wrap_F_2a4f38: the post effect object's targets being made (once, as the first race is got ready).
   The gate is true meanwhile, so the game's own bloom's targets (+0x148 the screen's size, +0x14c and +0x150 half
   of it) are there whatever PostFX is, and choosing 1 or 4 later switches the game's own passes on at once. */
void gt_postfx_targets(int on) { making = on; }

/* the game's own pipeline (F_28d6c0): 1 or 4, and while its targets are made */
int gt_postfx_gate(void)
{
    return making || GAME_OWN(gt_postfx_mode());
}

/* POSTFX=4 (Enhanced): the value of the shaders' gt_balance (gles.c, at glUseProgram) */
static float gt_shader_balance(void)
{
    return gt_postfx_mode() == 4 ? 1.0f : 0.0f;
}

/* POSTFX=4 (Enhanced): how strongly the balanced pipeline glows.  In a race the game's own bloom is three passes
   (read from the shaders the race compiles: gtr_preBloom, the threshold pass, is never compiled): the highlights
   alone (gtr_preBloomCar: pow(N.H, 350) x 4, a small hot spot; gtr_preBloomStaticObject: pow(N.H, 8) x
   highLightRatio x the texture's specular mask, a broad sheen), the 7-tap blur (gtr_BloomBlur) and the composite
   (gtr_PostBloom: the scene + the blurred highlights).  On U.S. Speed Ring's banked turns, which face the sun, the
   static sheen covers the whole asphalt and the grass beside it; with the original composite (a plain add) every
   grain of the asphalt's mask went to white over a road already bright, and the Classic Natural glow over it went
   further.  Balanced: the static highlights scaled by BAL_STATIC, the car's by BAL_CAR, and the blurred highlights
   added as a screen when BAL_SCREEN (x BAL_POST x (1 - the scene)), so a bright pixel takes less of the glow and
   none passes white.  BAL_STATIC 0.30 keeps the asphalt's grain and the grass green on the banked turns (1: the
   grass beside them near 225 grey; 0: no sheen at all); BAL_CAR 1.15 makes the paint's and the glass's hot spots
   a little wider and brighter than the original's, so the car shines more than under Classic Natural alone;
   BAL_POST 1.10 brings back the glow the screen takes from mid-tones (1.0: Test Center's drag strip about 8 levels
   darker).  GT_POSTFX_BALANCE_TUNE="static post screen [car]"
   (testing: 0..2, 0..2, 0 / 1, 0..2) tries other values; "1 1 0 1" is the balance before the banked turns were
   measured. */
#define BAL_STATIC 0.30f
#define BAL_CAR 1.15f
#define BAL_POST 1.10f
#define BAL_SCREEN 1
static void balance_tune(float *st, float *car, float *post, int *screen)
{
    static float v[3] = {-1.0f};
    static int s;
    if (v[0] < 0.0f) {
        v[0] = BAL_STATIC, v[1] = BAL_CAR, v[2] = BAL_POST, s = BAL_SCREEN;
        const char *e = dev_getenv("GT_POSTFX_BALANCE_TUNE");
        float a, b, d = BAL_CAR;
        int c, n = e ? sscanf(e, "%f %f %d %f", &a, &b, &c, &d) : 0;
        if (n >= 3 && a >= 0.0f && a <= 2.0f && b >= 0.0f && b <= 2.0f && d >= 0.0f && d <= 2.0f) {
            v[0] = a, v[1] = d, v[2] = b, s = c != 0;
            rt_log("[postfx] Balanced: static highlights %.2f, car highlights %.2f, glow %.2f%s "
                   "(GT_POSTFX_BALANCE_TUNE)\n", (double)a, (double)d, (double)b, s ? " as a screen" : " added");
        }
    }
    *st = v[0], *car = v[1], *post = v[2], *screen = s;
}

/* POSTFX=4 (Enhanced): the game's bloom shaders given gt_balance (gles.c, at glShaderSource; NULL: the source as it
   is).  Each change keeps the original's term for gt_balance 0. */
static char *gt_shader_patch(const char *src)
{
    float st, car, post;
    int screen;
    balance_tune(&st, &car, &post, &screen);
    char e_static[96], e_car[96], e_post[128];
    snprintf(e_static, sizeof e_static, "specularIntensity *= highLightRatio * mix(1.0, %.4f, gt_balance);",
             (double)st);
    snprintf(e_car, sizeof e_car, "specularIntensity = 4.0 * mix(1.0, %.4f, gt_balance);", (double)car);
    if (screen)
        snprintf(e_post, sizeof e_post, "+ color1 * mix(vec4(1.0), vec4(%.4f * (vec3(1.0) - color0.rgb), 1.0), "
                 "gt_balance);", (double)post);
    else
        snprintf(e_post, sizeof e_post, "+ color1 * mix(vec4(1.0), vec4(vec3(%.4f), 1.0), gt_balance);",
                 (double)post);
    const struct { const char *was, *now; int highlights_only; } edits[] = {
        /* gtr_preBloom: the luminance's third term is alpha in the original */
        {"color.a * 0.33333", "mix(color.a, color.b, gt_balance) * 0.33333", 0},
        /* gtr_preBloomStaticObject, gtr_preBloomCar: the highlights' colour */
        {"vec3(1.28, 1.0, 1.0)", "mix(vec3(1.28, 1.0, 1.0), vec3(1.0), gt_balance)", 1},
        /* gtr_preBloomStaticObject: the static objects' highlights */
        {"specularIntensity *= highLightRatio;", e_static, 1},
        /* gtr_preBloomCar: the car's highlights */
        {"specularIntensity = 4.0;", e_car, 1},
        /* gtr_BloomBlur: divided by the weights' sum (3.462), not 3 */
        {"color / 3.0", "color / mix(3.0, 3.462, gt_balance)", 0},
        /* gtr_PostBloom: texture0 x 1, not x 2 */
        {"color0 * 2.0", "color0 * mix(2.0, 1.0, gt_balance)", 0},
        /* gtr_PostBloom: the blurred highlights' strength, added or as a screen (the scene's alpha term kept) */
        {"+ color1;", e_post, 0},
    };
    /* the highlight passes start from black; gtr_road and gtr_high_light_reflection (the picture itself) have the
       same specular colour and are left as they are */
    int highlights = strstr(src, "specularColor") && strstr(src, "vec4(0.0, 0.0, 0.0, 1.0)");
    if (!strstr(src, "luminanceCoef") && !highlights && !strstr(src, "blurWeight") && !strstr(src, "color0 * 2.0"))
        return NULL;
    size_t n = strlen(src);
    char *out = malloc(n + 1024);
    if (!out)
        return NULL;
    static const char head[] = "uniform float gt_balance;\n";
    memcpy(out, head, sizeof head - 1);
    char *o = out + sizeof head - 1;
    int changed = 0, which = 0;
    for (const char *p = src; *p;) {
        size_t k = 0;
        while (k < sizeof edits / sizeof *edits &&
               (strncmp(p, edits[k].was, strlen(edits[k].was)) || (edits[k].highlights_only && !highlights)))
            k++;
        if (k < sizeof edits / sizeof *edits && (size_t)(o - out) + strlen(edits[k].now) < n + 1000) {
            size_t l = strlen(edits[k].now);
            memcpy(o, edits[k].now, l);
            o += l;
            p += strlen(edits[k].was);
            changed++;
            which |= 1 << k;
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
    if (!changed) {
        free(out);
        return NULL;
    }
    static int logs;
    if (logs++ < 8)
        rt_log("[postfx] a bloom shader given gt_balance (%d change%s, edits 0x%02x, POSTFX=4)\n", changed,
               changed > 1 ? "s" : "", which);
    return out;
}

/* GT_POSTFX=2 */

void __real_F_2a36e8(CPU *c);

typedef struct {
    u32 fb, tex, w, h;
} Target;
static Target scene, bright, blur1, blur2;     /* the screen (mipmapped), 512 x 512, 256 x 256, 256 x 256 */

enum { P_BRIGHT, P_BLUR, P_ADD, P_FLAT, P_TONE, NPROG };
static u32 prog[NPROG], loc_k[NPROG], loc_a, loc_b, loc_add, loc_sat;
static int progs_ok = -1;

static const char *const vs_src =
    "#version 120\n"
    "varying vec2 v;\n"
    "void main() { v = gl_MultiTexCoord0.xy; gl_Position = gl_Vertex; }\n";
static const char *const fs_src[NPROG] = {
    /* the bright pass: the scene minus the threshold (the 2010 builds' texture environment SUBTRACT) */
    "#version 120\n"
    "uniform sampler2D t; uniform vec3 k; varying vec2 v;\n"
    "void main() { gl_FragColor = vec4(max(texture2D(t, v).rgb - k, 0.0), 1.0); }\n",
    /* the blur (0x160ae4): 0.2 of the texel, 0.2 of each of four texels around it */
    "#version 120\n"
    "uniform sampler2D t; uniform vec2 a, b; varying vec2 v;\n"
    "void main() {\n"
    "  vec3 c = texture2D(t, v).rgb + texture2D(t, v + a).rgb + texture2D(t, v - a).rgb\n"
    "         + texture2D(t, v + b).rgb + texture2D(t, v - b).rgb;\n"
    "  gl_FragColor = vec4(0.2 * c, 1.0);\n"
    "}\n",
    /* the blurred texture onto the screen: x the bloom tint (mode 2), + the tint (mode 4) */
    "#version 120\n"
    "uniform sampler2D t; uniform vec3 k; uniform float add; varying vec2 v;\n"
    "void main() {\n"
    "  vec3 c = texture2D(t, v).rgb;\n"
    "  gl_FragColor = vec4(clamp(add > 0.5 ? c + k : c * k, 0.0, 1.0), 1.0);\n"
    "}\n",
    /* the scene tint (drawn with glBlendFunc(ZERO, SRC_COLOR): the screen x k) */
    "#version 120\n"
    "uniform vec3 k;\n"
    "void main() { gl_FragColor = vec4(k, 1.0); }\n",
    /* Enhanced: the scene x the scene tint, its saturation scaled around its luminance (Rec. 601 weights) */
    "#version 120\n"
    "uniform sampler2D t; uniform vec3 k; uniform float sat; varying vec2 v;\n"
    "void main() {\n"
    "  vec3 c = texture2D(t, v).rgb * k;\n"
    "  float y = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "  gl_FragColor = vec4(clamp(mix(vec3(y), c, sat), 0.0, 1.0), 1.0);\n"
    "}\n",
};

static u32 compile(u32 kind, const char *src)
{
    u32 s = p_glCreateShader(kind), ok = 0;
    const char *srcs[1] = {src};
    p_glShaderSource(s, 1, GA(srcs), 0);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, GA(&ok));
    if (!ok) {
        char log[512] = "";
        p_glGetShaderInfoLog(s, sizeof log, 0, GA(log));
        rt_log("[postfx] 2010 bloom: a shader did not compile: %s\n", log);
        return 0;
    }
    return s;
}

static int make_progs(void)
{
    if (progs_ok >= 0)
        return progs_ok;
    progs_ok = 0;
    u32 vs = compile(GL_VERTEX_SHADER, vs_src);
    if (!vs)
        return 0;
    for (int i = 0; i < NPROG; i++) {
        u32 fs = compile(GL_FRAGMENT_SHADER, fs_src[i]), ok = 0;
        if (!fs)
            return 0;
        prog[i] = p_glCreateProgram();
        p_glAttachShader(prog[i], vs);
        p_glAttachShader(prog[i], fs);
        p_glLinkProgram(prog[i]);
        p_glGetProgramiv(prog[i], GL_LINK_STATUS, GA(&ok));
        if (!ok) {
            char log[512] = "";
            p_glGetProgramInfoLog(prog[i], sizeof log, 0, GA(log));
            rt_log("[postfx] 2010 bloom: a program did not link: %s\n", log);
            return 0;
        }
        loc_k[i] = p_glGetUniformLocation(prog[i], GA("k"));
        p_glUseProgram(prog[i]);
        p_glUniform1i(p_glGetUniformLocation(prog[i], GA("t")), 0);
    }
    loc_a = p_glGetUniformLocation(prog[P_BLUR], GA("a"));
    loc_b = p_glGetUniformLocation(prog[P_BLUR], GA("b"));
    loc_add = p_glGetUniformLocation(prog[P_ADD], GA("add"));
    loc_sat = p_glGetUniformLocation(prog[P_TONE], GA("sat"));
    progs_ok = 1;
    return 1;
}

static void target(Target *t, u32 w, u32 h, int mip)
{
    if (t->tex && t->w == w && t->h == h)
        return;
    if (!t->tex) {
        p_glGenTextures(1, GA(&t->tex));
        p_glGenFramebuffers(1, GA(&t->fb));
    }
    p_glBindTexture(GL_TEXTURE_2D, t->tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mip)
        p_glGenerateMipmap(GL_TEXTURE_2D);
    gles_bind_fb(GL_FRAMEBUFFER, t->fb);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    t->w = w, t->h = h;
}

static void quad(void)
{
    p_glBegin(GL_QUADS);
    p_glTexCoord2f(0, 0), p_glVertex2f(-1, -1);
    p_glTexCoord2f(1, 0), p_glVertex2f(1, -1);
    p_glTexCoord2f(1, 1), p_glVertex2f(1, 1);
    p_glTexCoord2f(0, 1), p_glVertex2f(-1, 1);
    p_glEnd();
}

/* src into dst, blurred: offsets in src's texels x the radius (as 0x160ae4) */
static void blur(const Target *src, const Target *dst, float radius)
{
    gles_bind_fb(GL_FRAMEBUFFER, dst->fb);
    p_glViewport(0, 0, dst->w, dst->h);
    p_glBindTexture(GL_TEXTURE_2D, src->tex);
    float dx = radius / (float)src->w, dy = radius / (float)src->h;
    p_glUniform2f(loc_a, 1.5f * dx, 0.5f * dy);
    p_glUniform2f(loc_b, -0.5f * dx, 1.5f * dy);
    quad();
}

/* the settings of one picture: as the track gives them (0..255), the radius in texels, the blend mode */
typedef struct {
    unsigned char tint[3], thr[3], scn[3];
    float radius;
    u32 blend;
    int tint_scene, add_bloom;
    int screen;                                 /* Enhanced: the glow added as a screen */
    float sat;                                  /* Enhanced: the scene's saturation (0: no tone pass, the scene tint
                                                   drawn as a blend) */
} Params;

/* GT_POSTFX=3 (an enhancement of 2) and Enhanced's part: the 2010 steps, their colours made grey (each its own
   luminance, Rec. 601 weights), so the glow and the scene's darkening stay as bright as the track asks but the world
   keeps the free build's colours (the tracks' tints cast the whole picture: Snowfield blue, France warm). */
static void neutral(unsigned char v[3])
{
    unsigned char y = (unsigned char)((299 * v[0] + 587 * v[1] + 114 * v[2] + 500) / 1000);
    v[0] = v[1] = v[2] = y;
}

static void postfx_draw(const Params *p);

/* Enhanced's tone, in a race over the game's own glow and in the menus' 3D scene:
   - the bright pass's threshold at least ENHANCED_THRESHOLD (grey, of 255).  The tracks ask far less (Test Center
     and U.S. Speed Ring 42: a pixel a sixth of white glows), so with the glow as a screen every mid-tone took some
     of it: the asphalt, the grass and the shadows lifted and greyed.  At ENHANCED_THRESHOLD only what is brighter
     than most of the picture glows (the sky, the lights, bright paint, the game's own highlights);
   - the glow keeps its colour: the track's bloom tint as it is (not made grey, as 3 makes it) x ENHANCED_GLOW
     (its largest channel held at 255, the others in the same ratio), and the bright pass keeps the source's
     colour, so the sky glows blue and a red light red.  Above the threshold less is left to glow, so the tint is
     stronger than the track's (Test Center's 130 grey x 2: the drag strip's mean 1.6 levels over 0.85's);
   - added as a screen when ENHANCED_SCREEN (glBlendFunc(ONE_MINUS_DST_COLOR, ONE)): a bright pixel takes less of
     it, none goes past white;
   - ENHANCED_SCENE of the track's scene darkening (grey), the contrast under the glow;
   - the scene redrawn with its saturation x ENHANCED_SATURATION (the tone pass, before the glow): the screen and
     the game's own sheen take some colour from what they brighten.
   Measured on Test Center's drag strip (frames 4600-4800, against Off): the threshold at 42 (the track's) lifted
   the lower quartile 8 levels and took a twentieth of the saturation; at 150, 160 or 175 the shadows are Off's
   (within a level) and the three differ by a level in the mean; the saturation 1.15 makes the picture about a
   tenth more saturated than Off's.  The scene darkening keeps the game's own sheen on the sunlit strip from white
   (no darkening: a sixth of the picture clipped).
   The game's own part (BAL_STATIC, BAL_CAR, BAL_POST) is left as it is.  GT_POSTFX_ENHANCED_GLOW="share screen
   [scene]" (testing: 0..4, 0 / 1, 0..1) and GT_POSTFX_ENHANCED_TONE="threshold saturation" (testing: 0..255,
   0.5..2) try other values. */
#define ENHANCED_GLOW 2.0f
#define ENHANCED_SCENE 0.85f
#define ENHANCED_SCREEN 1
#define ENHANCED_THRESHOLD 160
#define ENHANCED_SATURATION 1.15f
typedef struct {
    float glow, scene, sat;
    int screen, threshold;
} Tone;
static Tone enhanced_tone(void)
{
    static Tone t;
    static int made;
    if (!made) {
        made = 1;
        t.glow = ENHANCED_GLOW, t.scene = ENHANCED_SCENE, t.screen = ENHANCED_SCREEN;
        t.threshold = ENHANCED_THRESHOLD, t.sat = ENHANCED_SATURATION;
        const char *e = dev_getenv("GT_POSTFX_ENHANCED_GLOW");
        float a, c = t.scene;
        int b = t.screen;
        if (e && sscanf(e, "%f %d %f", &a, &b, &c) >= 1 && a >= 0.0f && a <= 4.0f && c >= 0.0f && c <= 1.0f) {
            t.glow = a, t.screen = b != 0, t.scene = c;
            rt_log("[postfx] Enhanced: glow %.2f%s, scene darkening %.2f (GT_POSTFX_ENHANCED_GLOW)\n", (double)a,
                   t.screen ? " as a screen" : " added", (double)c);
        }
        e = dev_getenv("GT_POSTFX_ENHANCED_TONE");
        int h;
        float s;
        if (e && sscanf(e, "%d %f", &h, &s) == 2 && h >= 0 && h <= 255 && s >= 0.5f && s <= 2.0f) {
            t.threshold = h, t.sat = s;
            rt_log("[postfx] Enhanced: threshold %d, saturation %.2f (GT_POSTFX_ENHANCED_TONE)\n", h, (double)s);
        }
    }
    return t;
}

/* Enhanced in the menus' 3D scene (MENU_3D's main menu, the car menus): no game's own pass runs there (only the
   race's renderer draws gtr_preBloomCar), and Classic Natural's menu settings (tint 210, threshold 90, scene 255,
   added) wash the sky and the car's bright paint to white (measured in the Garage: over a quarter of the 3D
   picture clipped).  Enhanced's own: the race's threshold, a full grey tint added as a screen, the scene darkened a
   little, and the race's saturation (enhanced_tone), so the menu's car looks as it does in a race.  Measured in the
   Garage (the 3D area, against Off): scene 245 a little darker than Off, 255 a little brighter with more clipped;
   250 between, with the shadows Off's.
   GT_POSTFX_ENHANCED_MENU="tint threshold scene screen" (testing: 0..255 each, 0 / 1) tries other values. */
#define ENHANCED_MENU_TINT 255
#define ENHANCED_MENU_THRESHOLD ENHANCED_THRESHOLD
#define ENHANCED_MENU_SCENE 250
#define ENHANCED_MENU_SCREEN 1
static Params enhanced_menu(void)
{
    static Params p;
    static int made;
    if (!made) {
        made = 1;
        int t = ENHANCED_MENU_TINT, h = ENHANCED_MENU_THRESHOLD, c = ENHANCED_MENU_SCENE, s = ENHANCED_MENU_SCREEN;
        const char *e = dev_getenv("GT_POSTFX_ENHANCED_MENU");
        int a[4];
        if (e && sscanf(e, "%d %d %d %d", &a[0], &a[1], &a[2], &a[3]) == 4 && a[0] >= 0 && a[0] <= 255 &&
            a[1] >= 0 && a[1] <= 255 && a[2] >= 0 && a[2] <= 255) {
            t = a[0], h = a[1], c = a[2], s = a[3] != 0;
            rt_log("[postfx] Enhanced: the menus' 3D scene: tint %d, threshold %d, scene %d%s "
                   "(GT_POSTFX_ENHANCED_MENU)\n", t, h, c, s ? ", as a screen" : "");
        }
        for (int i = 0; i < 3; i++)
            p.tint[i] = (unsigned char)t, p.thr[i] = (unsigned char)h, p.scn[i] = (unsigned char)c;
        p.radius = 1.5f, p.blend = 2, p.tint_scene = c < 255, p.add_bloom = 1, p.screen = s;
        p.sat = enhanced_tone().sat;
    }
    return p;
}

/* p as the track (or the menu) gives it; enhanced: Enhanced's tone (in a race over the game's own glow: the track's
   values given Enhanced's threshold, shares and saturation; in the menus: enhanced_menu's, already made) */
static void postfx_with(Params p, const char *where, int enhanced, int race)
{
    static Params last;
    static int told;
    if (memcmp(&last, &p, sizeof p) && told < 20) {
        told++;
        last = p;
        rt_log("[postfx] %s: tint %d %d %d, threshold %d %d %d, scene %d %d %d, radius %.2f, mode %u%s%s%s\n", where,
               p.tint[0], p.tint[1], p.tint[2], p.thr[0], p.thr[1], p.thr[2], p.scn[0], p.scn[1], p.scn[2],
               (double)p.radius, p.blend, p.tint_scene ? "" : ", no scene tint", p.add_bloom ? "" : ", no bloom",
               mode_ == 3 ? " (POSTFX=3: drawn grey)" : enhanced ? " (Enhanced: its own tone)" : "");
    }
    if (mode_ == 3 || mode_ == 4) {
        if (!enhanced)
            neutral(p.tint);                        /* Enhanced: the glow keeps its colour */
        neutral(p.thr);
        neutral(p.scn);
    }
    if (enhanced && race) {
        Tone t = enhanced_tone();
        float top = p.tint[0] > p.tint[1] ? p.tint[0] : p.tint[1];
        top = top > p.tint[2] ? top : p.tint[2];
        float g = top * t.glow > 255.0f ? 255.0f / top : t.glow;   /* the tint's colour kept at the cap */
        for (int i = 0; i < 3; i++) {
            p.tint[i] = (unsigned char)(p.tint[i] * g + 0.5f);
            p.scn[i] = (unsigned char)(255 - (255 - p.scn[i]) * t.scene + 0.5f);
            if (p.thr[i] < t.threshold)
                p.thr[i] = (unsigned char)t.threshold;
        }
        p.screen = t.screen, p.sat = t.sat;
    }
    postfx_draw(&p);
}

/* a race frame: the track's settings, as F_16f3fc copied them into the post effect object o */
static void race_postfx(u32 o)
{
    const unsigned char *m = GP(o);
    if (!m[0xf6])                                   /* the object's own on / off */
        return;
    Params p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 3; i++) {
        p.tint[i] = m[0xdc + 3 * i];
        p.thr[i] = m[0xdd + 3 * i];
        p.scn[i] = m[0xde + 3 * i];
    }
    memcpy(&p.radius, m + 0xf0, 4);
    p.blend = rd32(o + 0xe8);
    p.tint_scene = m[0xf4], p.add_bloom = m[0xf5];
    postfx_with(p, "a race", mode_ == 4, 1);
}

static void postfx_draw(const Params *p)
{
    float tint[3], thr[3], scn[3], radius = p->radius;
    for (int i = 0; i < 3; i++)
        tint[i] = p->tint[i] / 255.0f, thr[i] = p->thr[i] / 255.0f, scn[i] = p->scn[i] / 255.0f;
    u32 blend = p->blend;
    int tint_scene = p->tint_scene, add_bloom = p->add_bloom;

    u32 vp[4] = {0};
    p_glGetIntegerv(GL_VIEWPORT, GA(vp));
    if (vp[2] < 16 || vp[3] < 16)
        return;
    u32 cur_prog, rfbo, dfbo, active, tex0;         /* as eagl.c's present: the state kept (QUERY_FREE) */
    gles_state(GL_CURRENT_PROGRAM, &cur_prog);
    gles_state(GL_READ_FRAMEBUFFER_BINDING, &rfbo);
    gles_state(GL_DRAW_FRAMEBUFFER_BINDING, &dfbo);
    gles_state(GL_ACTIVE_TEXTURE, &active);
    gles_state(GL_TEXTURE_BINDING_2D, &tex0);       /* unit 0's */
    p_glActiveTexture(GL_TEXTURE0);
    p_glPushAttrib(GL_ALL_ATTRIB_BITS);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_STENCIL_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_ALPHA_TEST);
    p_glColorMask(1, 1, 1, 1);
    p_glDepthMask(0);

    if (make_progs()) {
        target(&scene, vp[2], vp[3], 1);
        target(&bright, 512, 512, 0);
        target(&blur1, 256, 256, 0);
        target(&blur2, 256, 256, 0);

        /* the race's picture, resolved, then its mipmaps (the bright pass reads it box-filtered) */
        gles_bind_fb(GL_READ_FRAMEBUFFER, dfbo);
        gles_bind_fb(GL_DRAW_FRAMEBUFFER, scene.fb);
        p_glBlitFramebuffer(vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3], 0, 0, vp[2], vp[3], GL_COLOR_BUFFER_BIT,
                            GL_NEAREST);
        p_glBindTexture(GL_TEXTURE_2D, scene.tex);
        p_glGenerateMipmap(GL_TEXTURE_2D);

        gles_bind_fb(GL_FRAMEBUFFER, bright.fb);
        p_glViewport(0, 0, bright.w, bright.h);
        p_glUseProgram(prog[P_BRIGHT]);
        p_glUniform3f(loc_k[P_BRIGHT], thr[0], thr[1], thr[2]);
        quad();

        p_glUseProgram(prog[P_BLUR]);
        blur(&bright, &blur1, radius);
        blur(&blur1, &blur2, radius);

        gles_bind_fb(GL_FRAMEBUFFER, dfbo);
        p_glViewport(vp[0], vp[1], vp[2], vp[3]);
        p_glEnable(GL_BLEND);
        if (p->sat > 0.0f) {
            /* Enhanced: the scene redrawn from its copy, tinted and its saturation scaled (the alpha kept) */
            p_glDisable(GL_BLEND);
            p_glColorMask(1, 1, 1, 0);
            p_glUseProgram(prog[P_TONE]);
            p_glUniform3f(loc_k[P_TONE], tint_scene ? scn[0] : 1.0f, tint_scene ? scn[1] : 1.0f,
                          tint_scene ? scn[2] : 1.0f);
            p_glUniform1f(loc_sat, p->sat);
            p_glBindTexture(GL_TEXTURE_2D, scene.tex);
            quad();
            p_glColorMask(1, 1, 1, 1);
            p_glEnable(GL_BLEND);
        } else if (tint_scene) {
            p_glUseProgram(prog[P_FLAT]);
            p_glUniform3f(loc_k[P_FLAT], scn[0], scn[1], scn[2]);
            p_glBlendFunc(GL_ZERO, GL_SRC_COLOR);
            quad();
        }
        if (add_bloom) {
            p_glUseProgram(prog[P_ADD]);
            p_glUniform3f(loc_k[P_ADD], tint[0], tint[1], tint[2]);
            p_glUniform1f(loc_add, blend == 4 ? 1.0f : 0.0f);
            if (blend == 4)
                p_glBlendFunc(GL_SRC_COLOR, GL_ZERO);
            else
                p_glBlendFunc(p->screen ? GL_ONE_MINUS_DST_COLOR : GL_ONE, GL_ONE);
            p_glBindTexture(GL_TEXTURE_2D, blur2.tex);
            quad();
        }
    }

    /* back as it was: the game's framebuffers before the attributes (their draw buffers), then the bindings */
    gles_bind_fb(GL_READ_FRAMEBUFFER, rfbo);
    gles_bind_fb(GL_DRAW_FRAMEBUFFER, dfbo);
    p_glPopAttrib();
    p_glUseProgram(cur_prog);
    p_glBindTexture(GL_TEXTURE_2D, tex0);
    p_glActiveTexture(active);
}

/* F_16f3fc: after the race's 3D picture, before the HUD (an empty function in the free build), with the post
   effect object (r0).  With the game's own bloom on (1, Enhanced), its composite is
   still to come: the object is kept for F_14a660 (below). */
static u32 race_post;
void __wrap_F_2a36e8(CPU *c)
{
    u32 o = c->r[0];
    __real_F_2a36e8(c);
    race_post = 0;
    if (classic() && o) {
        if (gt_postfx_gate())
            race_post = o;
        else
            race_postfx(o);
    }
}

/* the menus' render (0x2520c4, the scene's vtable +0x18) draws the 3D menu scene (0x24ef64, while G+0x4403 is
   set: MENU_3D's main menu, the car menus) and then calls F_14a660 (return address 0x25213c) before gameswf.
   The bloom goes there, with the settings the free build gives a track without its own (0x174904: tint 210,
   threshold 90, scene 255, radius 1.5, mode 2), or Enhanced's own (enhanced_menu); the 2010 builds' menus drew no
   bloom. */
#define RET_RACE_AFTER_COMPOSITE 0x1700b4u   /* F_16f3fc's call after the game's own composite, before the HUD */
void __real_F_14a660(CPU *c);
void __wrap_F_14a660(CPU *c)
{
    if (c->r[14] == RET_RACE_AFTER_COMPOSITE && race_post) {
        u32 o = race_post;
        race_post = 0;
        if (classic())
            race_postfx(o);
    }
    if (c->r[14] == 0x25213cu && classic()) {
        u32 g = gt_game();
        if (g && *(volatile unsigned char *)GP(g + G_MENU_SCENE)) {
            Params p = {{210, 210, 210}, {90, 90, 90}, {255, 255, 255}, 1.5f, 2, 1, 1, 0, 0.0f};
            if (mode_ == 4)
                p = enhanced_menu();
            postfx_with(p, "the menus' 3D scene", mode_ == 4, 0);
        }
    }
    __real_F_14a660(c);
}

/* the hooks into the GL layer (gl.h): the bloom shaders' patch and gt_balance, before any shader is made */
__attribute__((constructor)) static void postfx_hooks(void)
{
    gles_add_shader_patch(gt_shader_patch);
    gles_add_host_uniform("gt_balance", gt_shader_balance);
}
