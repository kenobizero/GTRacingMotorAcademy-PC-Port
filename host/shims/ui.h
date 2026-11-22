/* ui.h -- the host's UIKit, EAGL, OpenGL ES and OpenAL (stage 4): uikit.c, eagl.c, gles.c, al.c.
   The game layer's own entry points are games/gtracing/features/features.h; what the shims call there is
   game_hooks.h.

   The iPad's screen is portrait, 768 x 1024 points (UIScreen bounds); the game runs in landscape, so
   iOS (or the game) rotates its views.  The PC window shows the landscape picture the player would
   see: 1024 x 768 points, scaled to the window. */
#ifndef GT_UI_H
#define GT_UI_H

#include <SDL3/SDL.h>

#include "abi.h"
#include "ns.h"

#define UI_SCREEN_W 768.0f          /* portrait points */
#define UI_SCREEN_H ui_screen_h     /* 1024; GT_WIDE=<landscape width> reports a wider screen (WIDE_UI) */
extern float ui_screen_h;
extern int ui_wide_ui;              /* WIDE_UI: on a wide screen the stage is drawn at one scale, centred */
extern float ui_stage_dx;           /* ... ui_stage_dx landscape points from the left (0 on a 4:3 screen) */
#define UI_ORIENTATION 3            /* UIInterfaceOrientationLandscapeRight = UIDeviceOrientationLandscapeLeft */

extern SDL_Window *ui_window;
extern int ui_quit;                 /* the window was closed */

/* uikit.c */
void ui_init(void);                 /* host class sizes and methods (before gt_load) */
void cg_init(void);                 /* cg.c: UIImage (the garage's screenshot), from ui_init */
int ui_is_main_thread(void);
int ui_background_fps(void);        /* BACKGROUND_THROTTLE: the cap behind other windows (0: none) */
int ui_minimized(void);             /* the window is minimized */
/* the view's transform from its own coordinates to the screen (portrait points): p' = m * p + t */
void ui_view_to_screen(id view, GtAffine *m);
id ui_layer_view(id layer);         /* the view whose layer this is (0 if none) */
void ui_layer_size(id layer, float *w, float *h, float *scale);

/* eagl.c */
void eagl_init(void);
void eagl_window_created(void);
u32 eagl_frames(void);              /* frames presented so far (the clock of the GT_* test switches) */
void eagl_drawable(u32 *rb, int *w, int *h);   /* the drawable's renderbuffer and its size in pixels */
/* gles.c */
void gles_load(void);               /* the GL entry points (after the first context) */
/* al.c */
void al_init(void);
void al_stop_all(void);             /* every source silent (a crash's box) */
/* the accelerometer (uikit.c), fed by the PC input (games/gtracing/features/pcinput.c) */
extern float ui_tilt;               /* -1 .. 1 steering tilt from the keyboard / pad */
extern volatile int ui_app_background;  /* Android: the app is in the background */
extern int ui_sensor_ok;            /* Android: the device's accelerometer, as the iPad's (in g) */
extern double ui_sensor_g[3];
void ui_gravity(double g[3]);       /* the iPad's gravity (x, y, z in g) for that tilt */

#endif
