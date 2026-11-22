/* pcinput.c -- the PC's input for the game: SDL's events turned into what the iPad game reads, and the pointer.

   - The mouse is a finger (ui_touch_event, uikit.c): a press, a drag and a release are a touch's phases, at the stage
     point under the pointer (to_landscape: the letterboxed picture, eagl.c; WIDE_UI's wider stage).
   - The keyboard and the gamepad drive the race (ui_tilt, ui_gas ..., read by steer.c), the menus (padmenu.c,
     KEY_MENU) and the port's pages (keymap.c, p2p.c's forms, display.c's Full Screen key).
   - The pointer: put on a button and glided there (PAD_MENU_SNAP, PAD_GLIDE), taps and swipes made for the pad,
     the gold cursor (GT_CURSOR=0 keeps the system arrow), hidden while a race is driven.
   - The test switches (GT_TAPS, GT_KEYNAV, GT_PADNAV, GT_PEDALS, GT_STEER, GT_PAUSE, GT_CAM: development builds
     only, rt.h dev_getenv) are played through the same paths.
   Split from uikit.c, which keeps UIKit's classes: the shims stay free of the game's own controls. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "features.h"
#include "game_hooks.h"
#include "uikit_input.h"

int ui_steer_active, ui_steer_dir;
int ui_gas, ui_brake, ui_gear_up, ui_gear_down, ui_gear_tap, ui_cam_tap, ui_rear;

/* the driving keys (steer, pedals, gears, look back, camera, pause) are the player's (KEY_MAP, keymap.c);
   the menus' keys stay fixed: W A S D / the arrows, Enter, Esc */
static int key_enter;               /* KEY_MENU: Enter held (the start's "Touch to continue") */
static int key_nav_tap;             /* KEY_MENU: presses latched in the key event (bits as padmenu_frame) */
static int key_nav_held;            /* KEY_MENU: the move keys held (bits 1 up 2 down 4 left 8 right) */
static int key_menu_used;           /* the keyboard drove the menu pointer (it follows its button then) */
#define PAUSE_X 48                  /* the HUD's pause icon (HUD.btnBack), landscape points */
#define PAUSE_Y 46
#define RESUME_X 226                /* PauseMenu's Resume button */
#define RESUME_Y 340
static SDL_Gamepad *pad;

/* window pixels -> stage points (the picture is letterboxed to the screen's shape, see eagl.c; on a wide
   screen the stage's sides reach below 0 and past 1024, WIDE_UI) */
static int to_landscape(float wx, float wy, float *lx, float *ly)
{
    int ww, wh;
    SDL_GetWindowSize(ui_window, &ww, &wh);
    float s = fminf(ww / UI_SCREEN_H, wh / 768.0f);
    float ox = (ww - UI_SCREEN_H * s) / 2, oy = (wh - 768.0f * s) / 2;
    *lx = (wx - ox) / s;
    *ly = (wy - oy) / s;
    if (*lx < 0) *lx = 0;
    if (*ly < 0) *ly = 0;
    if (*lx > UI_SCREEN_H - 1) *lx = UI_SCREEN_H - 1;
    if (*ly > 767) *ly = 767;
    *lx = stage_from_landscape(*lx);
    return 1;
}

static void cursor_idle(void);
static void cursor_wake(void);

/* ui_tap: one tap at a stage point, given (held 4 frames) by the UI thread's next frame, as GT_TAPS's are.
   TOP_CARS (topcars.c): the Car Dealer's Buy / Rent button, so its own ActionScript opens the popup. */
static volatile int ui_tap_pending;
static volatile float ui_tap_x, ui_tap_y;
void ui_tap(float x, float y)
{
    ui_tap_x = x, ui_tap_y = y;
    ui_tap_pending = 1;
}

/* PAD_MENU_SNAP (padmenu.c): the pointer put on a button, where it is, and a tap there */
static float warp_x = -1, warp_y = -1;  /* window pixels of the last put, so its motion event is not the mouse */
static int pad_tap_up_at = -1;
static float pad_tap_x, pad_tap_y;

/* PAD_GLIDE (port addition): the pad's pointer glides to its button
   instead of jumping there, as Gran Turismo's menu cursor does: an ease-out (fast off, slowing into
   the button) over GT_PAD_GLIDE ms, default 140 (0 = a jump).  Timed by the clock, not
   frames.  A new put while gliding starts from where the pointer is.  The glide's own warps make
   mouse motion events; the last few warp points are kept so those are not taken for the mouse (a
   late event of an older warp would end the pad's hold).  A tap goes to the button's spot, not the
   pointer, so pressing Cross mid-glide still taps the chosen button. */
static float glide_x0, glide_y0, glide_x1, glide_y1; /* window pixels */
static Uint64 glide_t0, glide_len;                   /* ns; glide_len 0 = no glide running */
static float warp_ring[16][2];
static int warp_n;

static void pointer_warp(float wx, float wy)
{
    warp_x = wx, warp_y = wy;
    warp_ring[warp_n & 15][0] = wx, warp_ring[warp_n & 15][1] = wy;
    warp_n++;
    cursor_wake();
    SDL_WarpMouseInWindow(ui_window, wx, wy);
}

/* a motion event at a point the pad put the pointer on lately (not the mouse) */
static int our_warp(float wx, float wy)
{
    for (int i = 0; i < 16 && i < warp_n; i++)
        if (fabsf(wx - warp_ring[i][0]) <= 2 && fabsf(wy - warp_ring[i][1]) <= 2)
            return 1;
    return 0;
}

static int glide_ms(void)
{
    static int ms = -1;
    if (ms < 0) {
        ms = rt_setting_int("GT_PAD_GLIDE", 140, 0, 1000);
        rt_log("[ui] pad pointer %s (GT_PAD_GLIDE %d ms)\n", ms ? "glides to its button" : "jumps to its button", ms);
    }
    return ms;
}

static void pointer_glide(void)
{
    if (!glide_len)
        return;
    /* the main loop turns thousands of times a second: a step every 4 ms (~35 a glide) is smooth on
       any screen and keeps the warps and their motion events few */
    static Uint64 last;
    Uint64 now = SDL_GetTicksNS(), t = now - glide_t0;
    if (t < glide_len && now - last < 4000000u)
        return;
    last = now;
    float x, y;
    static int steps;
    steps++;
    if (t >= glide_len) {
        x = glide_x1, y = glide_y1;
        glide_len = 0;
        if (dev_getenv("GT_PADMENU_LOG"))
            rt_log("[ui] pointer glide done: %d steps, %.0f px\n", steps, hypotf(glide_x1 - glide_x0, glide_y1 - glide_y0));
        steps = 0;
    } else {
        float u = 1 - (float)t / (float)glide_len;
        float k = 1 - u * u * u;                     /* ease-out cubic */
        x = glide_x0 + (glide_x1 - glide_x0) * k;
        y = glide_y0 + (glide_y1 - glide_y0) * k;
    }
    pointer_warp(x, y);
}

void ui_pointer_to(float lx, float ly)
{
    int ww, wh;
    SDL_GetWindowSize(ui_window, &ww, &wh);
    float s = fminf(ww / UI_SCREEN_H, wh / 768.0f);
    float tx = (ww - UI_SCREEN_H * s) / 2 + landscape_from_stage(lx) * s, ty = (wh - 768.0f * s) / 2 + ly * s;
    int ms = glide_ms();
    float cx, cy;
    SDL_GetMouseState(&cx, &cy);
    if (glide_len)
        cx = warp_x, cy = warp_y;                    /* mid-glide: from where it is now */
    float d = hypotf(tx - cx, ty - cy);
    if (!ms || d < 3 || !SDL_CursorVisible()) {
        glide_len = 0;                               /* no glide, or the pointer was hidden: straight there */
        pointer_warp(tx, ty);
        return;
    }
    glide_x0 = cx, glide_y0 = cy, glide_x1 = tx, glide_y1 = ty;
    glide_t0 = SDL_GetTicksNS();
    glide_len = (Uint64)ms * 1000000u;
}

void ui_pointer_at(float *lx, float *ly)
{
    float wx, wy;
    SDL_GetMouseState(&wx, &wy);
    to_landscape(wx, wy, lx, ly);
}

static int pad_tap_down_at;
static float pad_tap_x0, pad_tap_y0;   /* a swipe's start (pad_tap_x, _y: its end; a tap: both alike) */

/* REPLAY_PAD (replay.c): a finger held for the pad's right stick, as the mouse's drag turns the replay's
   camera.  It goes down in the middle of the stage (no button of ReplayMenu there), moves by dx, dy stage
   points, and is lifted when the stick is let go (0, 0); far from the middle it is lifted and put down in
   the middle again.  Not while a pad tap is under way (two fingers would be a pinch). */
static int drag_down;

void ui_pad_drag(float dx, float dy)
{
    static float x, y;
    if (dx == 0 && dy == 0) {
        if (drag_down)
            ui_touch_event(6, 3, x, y);
        drag_down = 0;
        return;
    }
    if (drag_down && (fabsf(x - 512) > 300 || fabsf(y - 384) > 220)) {
        ui_touch_event(6, 3, x, y);
        drag_down = 0;
        return;                         /* down again next frame: a lift and a press in one frame look like a tap */
    }
    if (!drag_down) {
        if (pad_tap_up_at >= 0)
            return;
        x = 512, y = 384;
        ui_touch_event(6, 0, x, y);
        drag_down = 1;
    }
    x += dx, y += dy;
    ui_touch_event(6, 1, x, y);
}

int ui_pad_dragging(void) { return drag_down; }

void ui_pad_swipe(float x, float y, float x2, float y2, int frames)
{
    if (pad_tap_up_at >= 0 || drag_down)
        return;
    pad_tap_x0 = x, pad_tap_y0 = y, pad_tap_x = x2, pad_tap_y = y2;
    ui_touch_event(7, 0, x, y);
    pad_tap_down_at = (int)eagl_frames();
    pad_tap_up_at = pad_tap_down_at + (frames > 0 ? frames : 3);
}

void ui_pad_tap(float lx, float ly)
{
    ui_pad_swipe(lx, ly, lx, ly, 3);
}

/* PLAYER_NAME / ONLINE_P2P: SDL's text input while one of p2p.c's forms is open */
static void text_input_follow_forms(void)
{
    static int text_on;
    int want = p2p_form_open();
    if (want != text_on && ui_window) {
        if (want)
            SDL_StartTextInput(ui_window);
        else
            SDL_StopTextInput(ui_window);
        text_on = want;
    }
}

static void test_keynav(void)
{
    /* GT_KEYNAV=frame:k;... (testing KEY_MENU): a key press through SDL's event queue on that frame, let go
       2 frames later; k: U D L R arrows, E Enter, X Esc, a .. z that letter (the MAX_STATS code too) */
    static const char *kn;
    static SDL_Keycode up_key;
    static int up_at = -1;
    int now = (int)eagl_frames(), f;
    char c;
    if (!kn)
        kn = dev_getenv("GT_KEYNAV") ? dev_getenv("GT_KEYNAV") : "";
    SDL_Event k;
    if (up_at >= 0 && now >= up_at) {
        memset(&k, 0, sizeof k);
        k.type = SDL_EVENT_KEY_UP, k.key.key = up_key, k.key.down = false;
        SDL_PushEvent(&k);
        up_at = -1;
    } else if (up_at < 0 && *kn && sscanf(kn, "%d:%c", &f, &c) == 2 && now >= f) {
        static const char from[] = "UDLRwasdEXc";
        static const SDL_Keycode to[] = {SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_W, SDLK_A, SDLK_S, SDLK_D, SDLK_RETURN, SDLK_ESCAPE, SDLK_C};
        const char *p = strchr(from, c);
        if ((p && c) || (c >= 'a' && c <= 'z')) {
            memset(&k, 0, sizeof k);
            k.type = SDL_EVENT_KEY_DOWN, k.key.down = true;
            k.key.key = up_key = p && c ? to[p - from] : SDLK_A + (SDL_Keycode)(c - 'a');
            SDL_PushEvent(&k);
            up_at = now + 2;
            dev_log("[ui] test key %c at frame %d\n", c, now);
        }
        const char *n = strchr(kn, ';');
        kn = n ? n + 1 : "";
    }
}

/* a key went down or up */
static void key_event(const SDL_Event *e)
{
    int down = e->type == SDL_EVENT_KEY_DOWN;
    SDL_Keycode k = e->key.key;
    if (down && k == SDLK_RETURN && (e->key.mod & SDL_KMOD_ALT)) {
        if (!e->key.repeat)
            display_fullscreen_toggle();    /* FULL_SCREEN: remembered in gtracing.ini (display.c) */
        return;
    }
    /* MAX_STATS cheat code (maxmoney.c): in the main menu, a letter that continues the code once it has
       started is taken by it, its repeats and its release too (W A S D would move the menu pointer).
       The modifiers are left out, so capitals can be typed */
    {
        static SDL_Keycode cheat_taken;
        int modifier = k == SDLK_LSHIFT || k == SDLK_RSHIFT || k == SDLK_LCTRL || k == SDLK_RCTRL ||
                       k == SDLK_LALT || k == SDLK_RALT || k == SDLK_LGUI || k == SDLK_RGUI ||
                       k == SDLK_CAPSLOCK;
        if (cheat_taken && k == cheat_taken) {
            if (!down)
                cheat_taken = 0;
            if (!down || e->key.repeat)
                return;
        }
        if (down && !e->key.repeat && !modifier &&
            cheat_key(k >= SDLK_A && k <= SDLK_Z ? (int)(k - SDLK_A) + 'A' : 0)) {
            cheat_taken = k;
            return;
        }
    }
    /* the driving keys (KEY_MAP); a cell of the Keyboard / Gamepad page waiting for a key takes it */
    if (keymap_key(k, down, e->key.repeat))
        return;
    /* KEY_MENU: a press shorter than a frame still moves / selects (bits: up down left right
       select back) */
    int bit = k == SDLK_UP || k == SDLK_W ? 1 : k == SDLK_DOWN || k == SDLK_S ? 2 : k == SDLK_LEFT || k == SDLK_A ? 4 :
              k == SDLK_RIGHT || k == SDLK_D ? 8 :
              (k == SDLK_RETURN || k == SDLK_KP_ENTER) && !(e->key.mod & SDL_KMOD_ALT) ? 16 : k == SDLK_ESCAPE ? 32 : 0;
    if (down && !e->key.repeat)
        key_nav_tap |= bit;
    if (bit & 15)
        key_nav_held = down ? key_nav_held | bit : key_nav_held & ~bit;
    if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (!(e->key.mod & SDL_KMOD_ALT) || !down))
        key_enter = down;
}

/* SDL's events: the mouse and the touch screen to touches, the keys, the gamepads coming and going */
static void events(void)
{
    SDL_Event e;
    float lx, ly;
    while (SDL_PollEvent(&e)) {
        if (p2p_form_event(&e))
            continue;                   /* typed into a name / room code form (p2p.c) */
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_TERMINATING:
            ui_quit = 1;
            break;
        /* Android: the app leaves the screen and comes back (BACKGROUND_THROTTLE pauses the game as iOS does) */
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            ui_app_background = 1;
            break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND:
            ui_app_background = 0;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.which == SDL_TOUCH_MOUSEID)
                break;
            cursor_wake();
            keymap_cancel();              /* a click: a cell waiting for a key stays as it was */
            to_landscape(e.button.x, e.button.y, &lx, &ly);
            ui_touch_event(e.button.button == SDL_BUTTON_LEFT ? 0 : 1, 0, lx, ly);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID)
                break;
            cursor_wake();
            if (!our_warp(e.motion.x, e.motion.y)) {
                glide_len = 0;            /* the mouse took the pointer: no more glide */
                padmenu_mouse_moved();    /* not the pad's own put */
            }
            to_landscape(e.motion.x, e.motion.y, &lx, &ly);
            if (e.motion.state & SDL_BUTTON_LMASK)
                ui_touch_event(0, 1, lx, ly);
            if (e.motion.state & SDL_BUTTON_RMASK)
                ui_touch_event(1, 1, lx, ly);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID)
                break;
            to_landscape(e.button.x, e.button.y, &lx, &ly);
            ui_touch_event(e.button.button == SDL_BUTTON_LEFT ? 0 : 1, 3, lx, ly);
            break;
        case SDL_EVENT_MOUSE_WHEEL:     /* replay zoom (replay.c) */
            cursor_wake();
            ui_zoom_wheel += e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP: {
            int ww, wh;
            SDL_GetWindowSize(ui_window, &ww, &wh);
            to_landscape(e.tfinger.x * ww, e.tfinger.y * wh, &lx, &ly);
            int f = 2 + (int)(e.tfinger.fingerID % (MAX_FINGERS - 2));
            ui_touch_event(f, e.type == SDL_EVENT_FINGER_DOWN ? 0 : e.type == SDL_EVENT_FINGER_UP ? 3 : 1, lx, ly);
            break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            if (e.key.key == SDLK_AC_BACK)  /* Android's Back: Esc (the pause menu, back in the menus) */
                e.key.key = SDLK_ESCAPE;
            key_event(&e);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!pad)
                pad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) {
                SDL_CloseGamepad(pad);
                pad = NULL;
            }
            break;
        default:
            break;
        }
    }
}

static void test_taps(void)
{
    /* GT_TAPS=frame:x,y[,hold[,x2,y2]];frame:x,y... taps at landscape points on those frames, held 4 frames or
       hold frames; with x2,y2 the finger slides there during the hold (a swipe) (testing) */
    static const char *taps;
    static int tap_up_at = -1, tap_down_at;
    static float tap_x, tap_y, tap_x2, tap_y2;
    if (!taps)
        taps = dev_getenv("GT_TAPS") ? dev_getenv("GT_TAPS") : "";
    if (*taps || tap_up_at >= 0 || ui_tap_pending) {
        int now = (int)eagl_frames();
        if (ui_tap_pending && tap_up_at < 0) {       /* ui_tap(): a tap asked by the host (TOP_CARS) */
            tap_x = tap_x2 = ui_tap_x, tap_y = tap_y2 = ui_tap_y;
            ui_touch_event(9, 0, tap_x, tap_y);
            tap_down_at = now;
            tap_up_at = now + 4;
            ui_tap_pending = 0;
        }
        if (tap_up_at >= 0 && now < tap_up_at && (tap_x2 != tap_x || tap_y2 != tap_y)) {
            float k = (float)(now - tap_down_at) / (float)(tap_up_at - tap_down_at);
            ui_touch_event(9, 1, tap_x + (tap_x2 - tap_x) * k, tap_y + (tap_y2 - tap_y) * k);
        }
        if (tap_up_at >= 0 && now >= tap_up_at) {
            ui_touch_event(9, 3, tap_x2, tap_y2);
            tap_up_at = -1;
        }
        int f, hold = 4;
        float x, y, x2 = -1, y2 = -1;
        if (tap_up_at < 0 && *taps && sscanf(taps, "%d:%f,%f,%d,%f,%f", &f, &x, &y, &hold, &x2, &y2) >= 3 &&
            now >= f) {
            if (x2 < 0 || y2 < 0)
                x2 = x, y2 = y;
            dev_log("[ui] test tap at (%.0f, %.0f) to (%.0f, %.0f), frame %d, held %d frames\n", x, y, x2, y2, now,
                   hold);
            tap_x = x, tap_y = y, tap_x2 = x2, tap_y2 = y2;
            ui_touch_event(9, 0, x, y);
            tap_down_at = now;
            tap_up_at = now + (hold > 0 ? hold : 4);
            const char *n = strchr(taps, ';');
            taps = n ? n + 1 : "";
        }
    }
}

static float steer_wanted(int pad_busy)
{
    /* tilt eases toward the keys; the pad's stick sets it directly (KEY_MAP: the player's keys and pad
       inputs; a stick half gives its amount, a button all of it) */
    float want = (float)(keymap_key_held(KM_RIGHT) - keymap_key_held(KM_LEFT));
    if (pad && !pad_busy) {
        float s = keymap_pad(pad, KM_RIGHT) - keymap_pad(pad, KM_LEFT);
        if (fabsf(s) > 0.12f)
            want = s;
    }
    /* GT_STEER=from:to:value;... holds the steering at value (-1 .. 1) on those frames (testing) */
    static const char *steer_env;
    if (!steer_env)
        steer_env = dev_getenv("GT_STEER") ? dev_getenv("GT_STEER") : "";
    if (*steer_env) {
        int now = (int)eagl_frames();
        for (const char *p = steer_env; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
            int from, to;
            float v;
            if (sscanf(p, "%d:%d:%f", &from, &to, &v) == 3 && now >= from && now <= to)
                want = v;
        }
    }
    return want;
}

/* the pedals, gears and views of this frame; *key_pause: the pause key was pressed.  Returns padmenu_active() */
static int driving_controls(int pad_busy, int *key_pause)
{
    /* pedals, gears, views: keys, or the pad laid out like Gran Turismo 7's gamepad defaults (PAD_GT7,
       a port addition): left stick steers, R2 gas, L2 brake, Cross (Xbox A) gear up,
       Square (Xbox X) gear down, R1 next camera, L1 held = look back, Options (Xbox Menu) = pause */
    ui_gas = keymap_key_held(KM_GAS), ui_brake = keymap_key_held(KM_BRAKE);
    ui_gear_up = keymap_key_held(KM_GEAR_UP), ui_gear_down = keymap_key_held(KM_GEAR_DOWN);
    ui_rear = keymap_key_held(KM_REAR);           /* held: look back (steer.c rear_view) */
    if (keymap_key_tap(KM_GEAR_UP))               /* a tap shorter than a frame still shifts */
        ui_gear_tap |= 1;
    if (keymap_key_tap(KM_GEAR_DOWN))
        ui_gear_tap |= 2;
    if (keymap_key_tap(KM_CAMERA))
        ui_cam_tap = 1;
    *key_pause = keymap_key_tap(KM_PAUSE);
    int menus = padmenu_active();
    maxmoney_frame(menus);
    if (pad && !pad_busy) {
        /* 8000 of 32767 on a trigger; in the menus Cross selects (PAD_MENU_SNAP): an action on
           one of the menus' pad buttons waits for the race */
        ui_gas |= keymap_pad(pad, KM_GAS) > 0.244f;
        ui_brake |= keymap_pad(pad, KM_BRAKE) > 0.244f;
        if (!menus || !keymap_pad_menu_input(KM_GEAR_UP))
            ui_gear_up |= keymap_pad(pad, KM_GEAR_UP) > 0.5f;
        if (!menus || !keymap_pad_menu_input(KM_GEAR_DOWN))
            ui_gear_down |= keymap_pad(pad, KM_GEAR_DOWN) > 0.5f;
        if (!menus || !keymap_pad_menu_input(KM_REAR))
            ui_rear |= keymap_pad(pad, KM_REAR) > 0.5f;
    }
    return menus;
}

/* MAX_STATS cheat code (maxmoney.c): LB + RB held in the main menu, then Up Up Down Down X Y X Y (Square
   Triangle on a PlayStation pad).  While both bumpers are held there, the D-pad and the stick move no menu
   pointer (padmenu.c gets no direction), so entering the code does not jump between the menu's buttons.
   Returns 1 while the code has the pad. */
static int cheat_pad_step(int pad_busy)
{
    static int cheat_was;
    int b = 0, lb = 0, rb = 0;
    if (pad && !pad_busy) {
        static const SDL_GamepadButton others[] = {
            SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
            SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
            SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
        lb = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        rb = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        b = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) |
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) << 1 |
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST) << 2 |
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH) << 3;
        int other = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000 ||
                    SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
        for (size_t i = 0; i < sizeof others / sizeof others[0]; i++)
            other |= SDL_GetGamepadButton(pad, others[i]);
        b |= other << 4;
    }
    int press = b & ~cheat_was;
    cheat_was = b;
    return cheat_pad(lb, rb, press & 1, press & 2, press & 4, press & 8, press & 16);
}

static void menu_nav(int pad_busy, int menus)
{
    /* PAD_MENU_SNAP (padmenu.c): in the menus the D-pad or a flick of the left stick jumps the pointer
       between buttons (held: repeats after 0.4 s, then every 0.15 s), Cross taps, Circle goes back */
    int now = (int)eagl_frames();
    if (pad_tap_up_at >= 0 && now < pad_tap_up_at && (pad_tap_x != pad_tap_x0 || pad_tap_y != pad_tap_y0)) {
        float k = (float)(now - pad_tap_down_at) / (float)(pad_tap_up_at - pad_tap_down_at);
        ui_touch_event(7, 1, pad_tap_x0 + (pad_tap_x - pad_tap_x0) * k, pad_tap_y0 + (pad_tap_y - pad_tap_y0) * k);
    }
    if (pad_tap_up_at >= 0 && now >= pad_tap_up_at) {
        ui_touch_event(7, 3, pad_tap_x, pad_tap_y);
        pad_tap_up_at = -1;
    }
    static int held_was;
    static Uint64 repeat_at;
    int held = 0;
    if (pad && !pad_busy) {
        float sx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float sy = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || (sy < -0.6f && fabsf(sy) > fabsf(sx));
        held |= (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || (sy > 0.6f && fabsf(sy) > fabsf(sx))) << 1;
        held |= (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || (sx < -0.6f && fabsf(sx) >= fabsf(sy))) << 2;
        held |= (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || (sx > 0.6f && fabsf(sx) >= fabsf(sy))) << 3;
        held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) << 4;
        held |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) << 5;
    }
    if (cheat_pad_step(pad_busy))           /* MAX_STATS cheat code */
        held &= ~15;
    /* KEY_MENU: in the menus W A S D / the arrows
       move like the D-pad (held: repeat), Enter is Cross, Esc is Circle.  They still give the gas /
       brake / steering too (nothing drives in a menu; at the start's "Touch to continue"
       the menus do not take them) */
    /* a key counts as pressed only by its key-down event (key_nav_tap), not by being held when a
       menu comes: Esc pauses the race, and still held as the pause menu came in, it would have
       pressed Resume at once.  Held, the arrows repeat as the D-pad does */
    int kb = 0;
    int kb_tap = key_nav_tap;
    key_nav_tap = 0;
    if (menus) {
        kb = key_nav_held;
        if (kb_tap)
            key_menu_used = 1;
    }
    int pressed = (held & ~held_was) | (menus ? kb_tap : 0);
    held_was = held;
    held |= kb;
    Uint64 t = SDL_GetTicks();
    if (pressed & 15)
        repeat_at = t + 400;
    else if ((held & 15) && t >= repeat_at) {
        pressed |= held & 15;
        repeat_at = t + 150;
    }
    /* GT_PADNAV=frame:keys;... presses u d l r a b on those frames (testing) */
    static const char *nav_env;
    if (!nav_env)
        nav_env = dev_getenv("GT_PADNAV") ? dev_getenv("GT_PADNAV") : "";
    int f;
    char keys[8];
    while (*nav_env && sscanf(nav_env, "%d:%7[udlrab]", &f, keys) == 2 && now >= f) {
        for (const char *k = keys; *k; k++)
            pressed |= 1 << (int)(strchr("udlrab", *k) - "udlrab");
        const char *n = strchr(nav_env, ';');
        nav_env = n ? n + 1 : "";
    }
    padmenu_frame(menus ? pressed : 0, pad != NULL || key_menu_used || dev_getenv("GT_PADNAV") != NULL);
}

static void look_inputs(int pad_busy)
{
    /* REPLAY_PAD (replay.c): in a replay the right stick turns the camera, Y / Triangle zooms in, X / Square
       out, RT / R2 and LT / L2 go 10 s on and back */
    int ok = pad && !pad_busy;
    replay_pad(ok ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f : 0,
               ok ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f : 0,
               ok && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH),
               ok && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST),
               ok && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000,
               ok && SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000);
    /* RACE_LOOK (racelook.c): while racing, the right stick or the numpad looks around the car */
    race_look_input(ok ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f : 0,
                    ok ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f : 0);
}

static void pause_button(int pad_busy, int key_pause, int menus)
{
    /* Options: a tap on the HUD's pause icon (HUD.btnBack, top left) while a race is being driven; with
       the pause menu on top, a tap on its Resume button */
    static int pad_start_was, pause_up_at = -1;
    static float pause_x, pause_y;
    int pad_start = pad && !pad_busy && keymap_pad(pad, KM_PAUSE) > 0.5f;   /* Options (Xbox Menu) by default */
    int now = (int)eagl_frames();
    static const char *pause_env;      /* GT_PAUSE=frame;frame... presses it (testing) */
    if (!pause_env)
        pause_env = dev_getenv("GT_PAUSE") ? dev_getenv("GT_PAUSE") : "";
    int test = 0, f;
    if (*pause_env && sscanf(pause_env, "%d", &f) == 1 && now >= f) {
        test = 1;
        const char *n = strchr(pause_env, ';');
        pause_env = n ? n + 1 : "";
    }
    if (pause_up_at >= 0 && now >= pause_up_at) {
        ui_touch_event(8, 3, pause_x, pause_y);
        pause_up_at = -1;
    }
    /* KEY_MENU: Esc (the pause key, KEY_MAP) while the race is driven is Options (pause); in the pause
       menu Esc is Circle (Resume, padmenu.c), so not here */
    int pressed = (pad_start && !pad_start_was) || test || (key_pause && !menus);
    const char *top = pressed ? menu_top() : NULL;
    int paused = top && !strcmp(top, "PauseMenu");
    /* another in-race menu on the screen (the pause menu's Options): Options does nothing there, a
       tap at the pause icon's place would hit that menu's back arrow */
    if (pressed && pause_up_at < 0 && (paused || (steer_racing() && !top))) {
        pause_x = paused ? RESUME_X : PAUSE_X, pause_y = paused ? RESUME_Y : PAUSE_Y;
        ui_touch_event(8, 0, pause_x, pause_y);
        pause_up_at = now + 3;
        rt_log("[pad] %s (tap at %.0f, %.0f)\n", paused ? "resume" : "pause", pause_x, pause_y);
    }
    pad_start_was = pad_start;
}

static void test_pedals(void)
{
    /* GT_PEDALS=from:to:keys;... holds keys on those frames (testing): g gas, b brake, u up, d down,
       r rear view */
    static const char *ped_env;
    if (!ped_env)
        ped_env = dev_getenv("GT_PEDALS") ? dev_getenv("GT_PEDALS") : "";
    if (*ped_env) {
        int now = (int)eagl_frames();
        for (const char *p = ped_env; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
            int from, to;
            char keys[8];
            if (sscanf(p, "%d:%d:%7[gbudr]", &from, &to, keys) == 3 && now >= from && now <= to) {
                ui_gas |= strchr(keys, 'g') != NULL;
                ui_brake |= strchr(keys, 'b') != NULL;
                ui_gear_up |= strchr(keys, 'u') != NULL;
                ui_gear_down |= strchr(keys, 'd') != NULL;
                ui_rear |= strchr(keys, 'r') != NULL;
            }
        }
    }
}

static void start_tap(int pad_busy)
{
    /* PC_CONTROLS, keyboard / gamepad mode (Touch Screen off, pcctl.c): before the start the game waits
       for a touch ("Touch to continue": the starting grid, then the fly-by, race state 1); a press of
       gas (Up / W / R2) or gear up (Shift / Cross, Xbox A) gives it that touch, in the middle of the
       screen */
    static int start_was, start_up_at = -1;
    int now = (int)eagl_frames();
    int start = ui_gas || ui_gear_up || key_enter;   /* Enter too (KEY_MENU, as the pad's Cross) */
    /* and the pad's Cross / A whatever gear up is bound to (KEY_MAP): NO_TOUCH_TEXT's "Press A /
       Cross To Continue" (notouch.c) names it */
    if (pad && !pad_busy && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH))
        start = 1;
    if (start_up_at >= 0 && now >= start_up_at) {
        ui_touch_event(9, 3, 512, 400);
        start_up_at = -1;
    }
    if (start && !start_was && start_up_at < 0 && pcctl_keys_mode() && steer_race_waiting()) {
        ui_touch_event(9, 0, 512, 400);
        start_up_at = now + 3;
        rt_log("[pcctl] start: \"Touch to continue\" tapped\n");
    }
    start_was = start;
}

static void gears_and_camera(int pad_busy)
{
    /* a gear shifts once per press (keys also latch taps shorter than a frame in the key event) */
    static int gear_up_was, gear_down_was;
    if (ui_gear_up && !gear_up_was)
        ui_gear_tap |= 1;
    if (ui_gear_down && !gear_down_was)
        ui_gear_tap |= 2;
    gear_up_was = ui_gear_up, gear_down_was = ui_gear_down;
    /* next camera: C (key event above), the pad's R1, or GT_CAM=frame;frame... (testing) */
    static int pad_r1_was;
    int pad_r1 = pad && !pad_busy && keymap_pad(pad, KM_CAMERA) > 0.5f;
    if (pad_r1 && !pad_r1_was)
        ui_cam_tap = 1;
    pad_r1_was = pad_r1;
    static const char *cam_env;
    if (!cam_env)
        cam_env = dev_getenv("GT_CAM") ? dev_getenv("GT_CAM") : "";
    if (*cam_env) {
        int f;
        if (sscanf(cam_env, "%d", &f) == 1 && (int)eagl_frames() >= f) {
            ui_cam_tap = 1;
            const char *n = strchr(cam_env, ';');
            cam_env = n ? n + 1 : "";
        }
    }
}

/* Android: the accelerometer as the iPad's (in g, the device's own axes; SDL gives m/s^2 with the opposite sign).
   The game is drawn in landscape: held the other way round (SDL_ORIENTATION_LANDSCAPE_FLIPPED) x and y turn
   with it, so either way up the game sees the iPad held as it expects.  GT_TILT_INVERT=1 turns the steering's
   sign if a device's sensor axes differ. */
static void sensor_frame(void)
{
#if GT_ANDROID
    static SDL_Sensor *acc;
    static int tried, invert;
    if (!tried) {
        tried = 1;
        invert = rt_setting_int("GT_TILT_INVERT", 0, 0, 1);
        int n = 0;
        SDL_SensorID *ids = SDL_GetSensors(&n);
        for (int i = 0; ids && i < n && !acc; i++)
            if (SDL_GetSensorTypeForID(ids[i]) == SDL_SENSOR_ACCEL)
                acc = SDL_OpenSensor(ids[i]);
        SDL_free(ids);
        rt_log("[input] accelerometer: %s\n", acc ? SDL_GetSensorName(acc) : "none");
    }
    float v[3];
    if (!acc || !SDL_GetSensorData(acc, v, 3)) {
        ui_sensor_ok = 0;
        return;
    }
    double g[3] = {-v[0] / SDL_STANDARD_GRAVITY, -v[1] / SDL_STANDARD_GRAVITY, -v[2] / SDL_STANDARD_GRAVITY};
    if ((SDL_GetCurrentDisplayOrientation(SDL_GetPrimaryDisplay()) == SDL_ORIENTATION_LANDSCAPE_FLIPPED) != !!invert)
        g[0] = -g[0], g[1] = -g[1];
    memcpy(ui_sensor_g, g, sizeof g);
    ui_sensor_ok = 1;
#endif
}

void ui_pump(void)
{
    display_fullscreen_pump();          /* FULL_SCREEN: the Display page's row */
    board_tick();                       /* RED_BULL_BOARD: after a Red Bull race, the leaderboard (board.c) */
    gt_crash_test_tick(eagl_frames());  /* CRASH_DUMP's test (GT_CRASH_TEST, crashdump.c) */
    text_input_follow_forms();
    cursor_idle();
    pointer_glide();
    test_keynav();
    events();
    test_taps();
    int pad_busy = keymap_frame(pad);   /* the Keyboard / Gamepad page has the pad (a cell waits) */
    float want = steer_wanted(pad_busy);
    ui_steer_active = want != 0.0f;
    ui_steer_dir = want <= -0.5f ? -1 : want >= 0.5f ? 1 : 0;
    int key_pause, menus = driving_controls(pad_busy, &key_pause);
    menu_nav(pad_busy, menus);
    look_inputs(pad_busy);
    pause_button(pad_busy, key_pause, menus);
    test_pedals();
    start_tap(pad_busy);
    gears_and_camera(pad_busy);
    ui_tilt += (want - ui_tilt) * 0.2f;
    sensor_frame();
    ui_accel_tick();
    steer_frame();
}

/* The mouse pointer: a gold arrow drawn here (the iPad has none; Windows' white arrow looked out of place
   on the game's menus; a pointer like a console racing game's suits them).  A notched
   triangle, tip at the hot spot, filled light gold at the tip to deep amber at the base, with a dark brown
   outline and a soft shadow; drawn 4 x 4 supersampled, sized to the display's scale.  GT_CURSOR=0 keeps the
   system arrow. */
static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    dx = px - (ax + t * dx);
    dy = py - (ay + t * dy);
    return sqrtf(dx * dx + dy * dy);
}

static int in_poly(const float *p, int n, float x, float y)
{
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((p[2 * i + 1] > y) != (p[2 * j + 1] > y) &&
            x < (p[2 * j] - p[2 * i]) * (y - p[2 * i + 1]) / (p[2 * j + 1] - p[2 * i + 1]) + p[2 * i])
            in = !in;
    return in;
}

void ui_make_cursor(void)
{
    const char *env = getenv("GT_CURSOR");
    if (env && *env == '0')
        return;
    float sc = SDL_GetWindowDisplayScale(ui_window);
    if (sc < 1)
        sc = 1;
    /* GT_CURSOR_SIZE: a multiple of that size (1 .. 6).  A missing or blank key means 3 (at 1 the pointer is hard
       to see on a big screen).  The shape is drawn at
       the new size, not stretched, so it stays sharp; Windows takes colour cursors up to 256 px, so the size
       is capped there. */
    float mul = rt_setting_float("GT_CURSOR_SIZE", 3, 1, 6);
    int size = (int)(32 * sc * mul + 0.5f);
    if (size > 256)
        size = 256;
    float u = size / 32.0f;                     /* design units: a 32 x 32 grid */
    /* tip, down the left edge, the notch, out to the right */
    const float shape[] = {2, 1.5f, 2, 25, 8.5f, 18.5f, 19, 18};
    float poly[8], shadow[8];
    for (int i = 0; i < 8; i++) {
        poly[i] = shape[i] * u;
        shadow[i] = poly[i] + 1.5f * u;
    }
    float edge = 1.6f * u;                      /* outline width */
    SDL_Surface *s = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    if (!s)
        return;
    for (int y = 0; y < size; y++) {
        Uint8 *row = (Uint8 *)s->pixels + y * s->pitch;
        for (int x = 0; x < size; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    float d = 1e9f;
                    for (int i = 0, j = 3; i < 4; j = i++)
                        d = fminf(d, seg_dist(px, py, poly[2 * j], poly[2 * j + 1], poly[2 * i], poly[2 * i + 1]));
                    int in = in_poly(poly, 4, px, py);
                    if (in && d > edge) {
                        float t = (px + py) / (size * 1.2f);       /* tip light, base deep */
                        r += 1.0f;
                        g += 0.86f - 0.36f * t;
                        b += 0.42f - 0.38f * t;
                        a += 1;
                    } else if (in || d <= edge * 0.5f) {
                        r += 0.16f;
                        g += 0.09f;
                        b += 0.02f;
                        a += 1;
                    } else if (in_poly(shadow, 4, px, py)) {
                        a += 0.35f;                                  /* black shadow */
                    }
                }
            Uint8 *o = row + 4 * x;
            float ia = a > 0 ? 1 / a : 0;
            o[0] = (Uint8)(r * ia * 255 + 0.5f);
            o[1] = (Uint8)(g * ia * 255 + 0.5f);
            o[2] = (Uint8)(b * ia * 255 + 0.5f);
            o[3] = (Uint8)(a / 16 * 255 + 0.5f);
        }
    }
    const char *dump = dev_getenv("GT_CURSOR_DUMP");    /* testing: the pointer's picture as a .bmp */
    if (dump && *dump)
        SDL_SaveBMP(s, dump);
    SDL_Cursor *c = SDL_CreateColorCursor(s, (int)poly[0], (int)poly[1]);
    SDL_DestroySurface(s);
    if (c)
        SDL_SetCursor(c);
}

/* in a race (not a replay, whose buttons need it) the pointer hides after 2.5 s without mouse movement */
static Uint64 mouse_moved;

static void cursor_idle(void)
{
    if (!mouse_moved || !SDL_CursorVisible())
        return;
    if (steer_racing() && SDL_GetTicks() - mouse_moved > 2500)
        SDL_HideCursor();
}

static void cursor_wake(void)
{
    mouse_moved = SDL_GetTicks();
    if (!SDL_CursorVisible())
        SDL_ShowCursor();
}
