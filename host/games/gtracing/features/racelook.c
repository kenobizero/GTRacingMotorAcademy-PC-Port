/* racelook.c -- RACE_LOOK (port addition): a 360-degree look around the car while racing, with the pad's right
   stick or the numpad; released, the view goes straight back ahead.  The race's chase views only, as the replay
   turns only its chase view.

   The original has no free look while racing: only the look-back (HUD.btnRearView, the V key, steer.c
   REAR_VIEW) and, in a replay, the swipe that turns the orbit views.  Both end in the same place of the
   race camera ([race+0x10a8], update 0x19ca5c): the chase views 4 .. 6 (and the replay's 7) are made by
   0x19bf38, which takes the camera's heading in s20, adds the look-back's half turn
   (+0x28c blend x pi x +0x290 side, 0x19c34c ..) or, in a replay, the swipe's yaw (race+0xac0), and at
   0x19c384 turns its (0, 0, -1) by that heading (0x14d514) for the line from the car to the eye; the eye
   (+0x1e0) is the car (+0x3f8) less that line x the distance, shortened where a wall is in the way
   (0x1559c4), and the target (+0x1f8) is ahead along it.  The in-car views 1 .. 3 (0x19ab38, 0x19a8a0,
   0x19a658) are left alone.
   Here (recomp/codegen.py HOST_INSNS 0x19c384, gt_race_look) the look's angle is added to s20 there, so the
   game places the eye around the car itself, walls included.  The look goes:
     pad:  the right stick's direction (past 35 % of its travel): up ahead, right to the car's right, down
           behind, and between; let go, the view is ahead again;
     keys: the numpad, held: 8 ahead, 9 front right, 6 right, 3 rear right, 2 behind, 1 rear left, 4 left,
           7 front left (two held: between them); NumLock on or off (scancodes).
   It eases over GT_RACE_LOOK_SMOOTH ms (default 60; 0: at once), the short way round, and only while a
   race is driven (state 2 .. 8, not a replay: G+0x4179 0) in the chase views.  The right stick is left
   out when KEY_MAP gives one of its halves to a driving action.
   GT_RACE_LOOK=0 leaves the camera as it was.  Testing: GT_RACE_LOOKTEST=from:to:digits;... holds those
   numpad digits on those frames. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "features.h"
#include "game.h"

#define PI_F 3.14159265f

int steer_racing(void);
int keymap_right_stick_bound(void);

static int opt = -1;
static float smooth_s;
static float target;                    /* the look's angle now asked (radians, + = to the car's right) */
static int asked;                       /* a direction is held */
static float shown;                     /* the angle given to the camera */
static LARGE_INTEGER t_last;

/* GT_RACE_LOOKTEST: the numpad digits held on this frame */
static const char *look_test(void)
{
    static const char *env;
    static char keys[12];
    if (!env)
        env = dev_getenv("GT_RACE_LOOKTEST") ? dev_getenv("GT_RACE_LOOKTEST") : "";
    int now = (int)eagl_frames();
    keys[0] = 0;
    for (const char *p = env; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        int from, to;
        char k[10];
        if (sscanf(p, "%d:%d:%9[0-9]", &from, &to, k) == 3 && now >= from && now <= to &&
            strlen(keys) + strlen(k) < sizeof keys)
            strcat(keys, k);
    }
    return keys;
}

/* the numpad's digits held (and GT_RACE_LOOKTEST's): x to the right, y ahead; 0 = none (or opposite ones) */
int numpad_dir(float *x, float *y)
{
    static const signed char dir[10][2] = {
        {0, 0}, {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {0, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    static const SDL_Scancode kp[10] = {SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
                                        SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7,
                                        SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9};
    const bool *ks = SDL_GetKeyboardState(NULL);
    const char *test = look_test();
    float kx = 0, ky = 0;
    for (int d = 1; d <= 9; d++)
        if ((ks && ks[kp[d]]) || strchr(test, '0' + d))
            kx += dir[d][0], ky += dir[d][1];
    *x = kx, *y = ky;
    return kx != 0 || ky != 0;
}

/* once a frame from ui_pump: the right stick (0 when the pad is busy or absent) */
void race_look_input(float rx, float ry)
{
    if (opt < 0) {
        opt = !(getenv("GT_RACE_LOOK") && *getenv("GT_RACE_LOOK") == '0');
        smooth_s = rt_setting_float("GT_RACE_LOOK_SMOOTH", 60, 0, 2000) / 1000.0f;   /* ms; 0 = no easing */
        rt_log("[look] race chase views: right stick / numpad look around %s (eased %.0f ms)\n",
               opt ? "on" : "off (GT_RACE_LOOK=0)", smooth_s * 1000);
    }
    asked = 0;
    if (!opt || !steer_racing())
        return;
    float kx, ky;
    if (numpad_dir(&kx, &ky)) {
        target = atan2f(kx, ky);
        asked = 1;
        return;
    }
    if (keymap_right_stick_bound())
        return;
    if (rx * rx + ry * ry > 0.35f * 0.35f) {
        target = atan2f(rx, -ry);
        asked = 1;
    }
}

/* from the generated 0x19bf38 (codegen HOST_INSNS 0x19c384): the angle added to the chase camera's heading */
float gt_race_look(u32 cam)
{
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    int view = (int)rd32(cam + 0x23c);
    if (!race || rd32(race + 0x10a8) != cam)
        return 0;                       /* another camera: the look's state stays */
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = t_last.QuadPart ? (float)(t.QuadPart - t_last.QuadPart) / (float)f.QuadPart : 0.0f;
    t_last = t;
    int on = opt > 0 && !*(volatile u8 *)GP(g + G_REPLAY_ON) && view >= 4 && view <= 6 && steer_racing();
    float want = on && asked ? target : 0.0f;
    static int was, logs;
    if (!on) {
        shown = 0;
        was = 0;
        return 0;
    }
    float d = remainderf(want - shown, 2 * PI_F);
    if (smooth_s <= 0 || dt <= 0 || dt > 0.25f)
        shown += d;
    else
        shown += d * (1.0f - expf(-dt / smooth_s));
    if (fabsf(remainderf(want - shown, 2 * PI_F)) < 0.002f)
        shown = want;
    shown = remainderf(shown, 2 * PI_F);
    if (asked != was && logs++ < 40)
        rt_log("[look] %s (view %d)\n", asked ? "look" : "ahead again", view);
    if (asked && logs < 40 && fabsf(want - shown) < 0.002f) {
        static float logged = 99;
        if (logged != want) {
            logged = want;
            logs++;
            rt_log("[look] %.0f deg to the right of ahead\n", want * 180 / PI_F);
        }
    }
    was = asked;
    /* the game's heading turns the other way: + is to the car's left */
    return -shown;
}
