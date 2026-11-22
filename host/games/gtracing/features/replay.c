/* replay.c -- two replay fixes: REPLAY_UNDIM (the dim replay picture) and REPLAY_ZOOM (the mouse wheel
   as the iPad's pinch).  Both run once a frame from steer_frame.

   REPLAY_UNDIM (original-game fault, default on): the 3D picture and the HUD (Pos / Lap, speedometer) of a
   replay were dimmed, the replay bar and the top icons were not.
   What dims it is LowBarMenu.btnGreyOverlay of the menu SWF (LHBMiNHFKXiiMlHM.FAK, sprite 8: label
   "Show" fades a grey rectangle in, "Hide" moves it off the stage).  At the end of a race the
   FinishLinePopup's "Show" plays "Show" on it, as the dark backing of the end-of-race menu; EndMenu is
   pushed and the LowBarMenu goes to "Focus_Out", which keeps the overlay.  The Replay button (sprite
   79) only pushes ReplayMenu: the menu player (push 0x50b0a0) hides the old top (EndMenu) and shows
   the new one, and nothing - no SWF action, nor the native ReplayMenu (vtable 0x789670: 0x29c2b0,
   0x29c3fc, 0x29c440, 0x29c558) - puts the overlay to "Hide".  Leaving the replay (sprite 935:
   InReplay = true, LowBarMenu "Show" -> overlay "Show", PushMenu EndMenu) shows it again, so the
   designers took it as hidden during the replay.  No port fault was found (label lookup, the tolower
   shim, the menu stack and the visibility all follow the code), so this is taken as the original's
   oversight; no iPad was at hand to compare.  Found with staged tests on a save copy:
   hiding only this overlay makes the replay bright, and "Hide" on it at the replay brightens the
   picture and the HUD.
   Fix: when a replay runs (race state 13) and ReplayMenu is the top menu, play "Hide" on the overlay
   once (0x50bc94, gotoAndPlay by label); the game's own exit path shows it again for the end menu.
   GT_REPLAY_UNDIM=0 keeps the original.

   REPLAY_ZOOM (port addition: the iPad's pinch-to-zoom in the replay, on the mouse wheel).  The game
   takes a pinch from its own touch queue (0x17c788, once a frame): with two fingers
   down (pinch active, byte [0x70c06c] = 0x7a273c) and moved, the change of the distance between them
   (previous - now, so spreading the fingers is negative) goes to [0x70c070] = 0x7a2748.  The replay
   update (0x139c94) hands minus that to 0x13706c, which adds it to the zoom race +0xac4, clamped to
   0 .. 0.8 x [0x7711c0], in the replay views 4 .. 6 and (both ways) in view 7; the other views do not
   zoom (0x13706c still adds to the value there, unclamped).  A mouse has one pointer, so the wheel
   gives that value itself, in views 4 .. 7 only: up spreads (zooms in), each notch a tenth of the
   range, eased over a few updates.  It is set for one call of the replay update (vtable slot
   0x782edc) and put back after, since the game may run that update more or less than once per host
   frame (set once per host frame it would zoom 1x or 2x the amount).  A real pinch (two
   touch-screen fingers) keeps the game's own values.
   GT_REPLAY_ZOOM=0 leaves the wheel unused. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "features.h"
#include "game.h"

#define MENU_HOST 0x7a2564u            /* -> the object whose +0x1b2c is the menu SWF player */
#define REPLAY_STATE 13                /* race +0x1d84 while a replay runs */

#define PINCH_ACTIVE 0x7a273cu         /* byte: two fingers are pinching */
#define PINCH_DELTA 0x7a2748u          /* int: previous distance - distance, read by the replay update */

float ui_zoom_wheel;                   /* wheel notches since the last frame (pcinput.c), + = up */

static int option(const char *env)
{
    const char *e = getenv(env);
    return !(e && *e == '0');
}


/* the race, while a replay runs */
static u32 replay_race(void)
{
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    return race && (int)rd32(race + 0x1d84) == REPLAY_STATE ? race : 0;
}

/* the menus of a MenuFX player are on the screen: Resume does not pop PauseMenu (the stack stays
   LowBarMenu, PauseMenu, both in state 1 "shown", the clip unchanged); what changes is bit 0x80 of
   the player's flags at +0xf8: 0x90 while the pause menu shows, 0x10 while the race is driven (also
   before the first pause, stack empty).  (+0x58 / +0x5c go to -480 at the first Resume and stay
   there through the next pause: not it.)  Found from a test log: after Resume the pad
   still took PauseMenu as open, so Cross tapped its hidden buttons instead of shifting up, the
   pointer stayed, and Options tapped Resume instead of pausing again.
   The bit is not all of it: the game clears it too when it runs on under a menu that stays on the
   screen and takes touches - a replay (race state 13, ReplayMenu on top: 0x90 -> 0x10 the frame the
   replay starts) and the end menu once Next is pressed after an event (state 12, EndMenu on top, its
   EventsPopup "Congratulations!" / XP / prize / Next).  So once the race is over (state 12 or 13; no
   one drives it) the race player's menus count as shown when the top menu is in state 1 (shown) and
   is not a pause menu (one left in the stack from a pause during the race).  Port fault (of
   the bit test above): the replay menu and the menu after winning an event could not be
   used with a pad; the log had "pad off the menus" at both.  It had also left
   REPLAY_UNDIM a one-frame chance to see ReplayMenu (state 13 and the bit set together): a test
   replay stayed dim.
   The race may be gone too: after a licence's last test, Next in EndMenu frees the race (G + 0x17ac
   = 0) and the licence's "Congratulations! / A LICENSE ACQUIRED / Prize / Total / Next" (the event
   prize screen 0x29f4b0 pays its 9,000 cr there) shows until the main menus are loaded, with the main
   menus' player G + 0x19d4 not up yet, so the pad and the keys had no menu (log: "pad off the menus
   ... race off state -1 ... flags 0 / 10").  Port fault (as above): the screen after winning an event could
   not be used with the pad or the keyboard.  First taken for the race player; the next event still had no pad
   (flags 0 / 0 after Next) and the code of 0x29f4b0 (0x29f4e8) shows its own player G + 0x1b38.  So
   with no race the race player's and that player's menus count as shown by the same top-menu test. */
int menu_shown(u32 pl)
{
    if (rd32(pl + 0xf8) & 0x80)
        return 1;
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    int st = race ? (int)rd32(race + 0x1d84) : -1;
    /* with no race, the event prize player G + 0x1b38 as well (see padmenu.c player_now) */
    if (!g || (race && st != 12 && st != REPLAY_STATE)
        || (pl != rd32(g + G_RACE_MENUS) && (race || pl != rd32(g + G_PRIZE_MENUS))))
        return 0;
    int n = (int)rd32(pl + 0x118);
    u32 arr = rd32(pl + 0x114), e = n > 0 && arr ? rd32(arr + 4 * (n - 1)) : 0;
    return e && rd32(e + 0x58) == 1 && strncmp((const char *)GP(e + 8), "PauseMenu", 9);
}

/* the menu SWF player when it is set up and shown, and its top menu's name */
static u32 menu_player(const char **top)
{
    *top = NULL;
    u32 host = rd32(MENU_HOST), pl = host ? rd32(host + 0x1b2c) : 0;
    if (!pl || !rd32(pl + 0x3c) || !rd32(pl + 0x40) || !menu_shown(pl))
        return 0;
    int n = (int)rd32(pl + 0x118);
    u32 arr = rd32(pl + 0x114);
    if (n > 0 && arr) {
        u32 e = rd32(arr + 4 * (n - 1));
        if (e)
            *top = (const char *)GP(e + 8);
    }
    return pl;
}

/* the menu SWF's top menu ("PauseMenu", "ReplayMenu", ...) or NULL (pcinput.c: the pad's pause) */
const char *menu_top(void)
{
    const char *top;
    return menu_player(&top) ? top : NULL;
}

static void replay_undim(u32 race)
{
    static int on = -1, done;
    if (on < 0) {
        on = option("GT_REPLAY_UNDIM");
        rt_log("[replay] grey overlay %s during replays\n", on ? "hidden" : "kept (original)");
    }
    if (!on)
        return;
    const char *top;
    u32 pl = race ? menu_player(&top) : 0;
    if (!pl || !top || strcmp(top, "ReplayMenu")) {
        done = 0;
        return;
    }
    if (done)
        return;
    done = 1;
    static const char path[] = "LowBarMenu.btnGreyOverlay", label[] = "Hide";
    if (!rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path)))
        return;
    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)path), GA((void *)label), 1);
    rt_log("[replay] %s -> %s\n", path, label);
}

/* the wheel's zoom still to give, in the pinch's units (+ = in) */
static float zoom_pending;
static int zoom_logs;                          /* the first 40 wheel turns are logged */
static int hint_turn_frames;                   /* REPLAY_HINT_PAD: the swipe hint's turn, frames left */
static float hint_turn_from;                   /* its start yaw */

static void replay_zoom(u32 race)
{
    static int on = -1;
    if (on < 0)
        on = option("GT_REPLAY_ZOOM");
    float notches = ui_zoom_wheel;
    ui_zoom_wheel = 0;
    u32 g = gt_game();
    int view = g ? (int)rd32(g + G_REPLAY_VIEW) : -1;
    /* 0x13706c adds to the value in every view but clamps and uses it only in views 4 .. 7: the wheel
       is left out of the others, so the value does not run off there */
    if (!on || !race || view < 4 || view > 7 || *(volatile u8 *)GP(PINCH_ACTIVE)) {
        if (fabsf(notches) >= 1 && on && race && zoom_logs++ < 40)
            rt_log("[replay] wheel %+.0f ignored: replay view %d does not zoom (4 .. 7 do)\n", notches, view);
        zoom_pending = 0;
        return;
    }
    if (notches != 0) {
        int screen = (int)rd32(GT_SCREEN_PX_H);
        float range = 0.8f * (float)(screen > 0 ? screen : 1024);
        zoom_pending += notches * range / 10.0f;
        if (fabsf(notches) >= 1 && zoom_logs++ < 40)  /* the pad's buttons give a little each frame */
            rt_log("[replay] wheel %+.0f: zoom %d (range %.0f), replay view %d\n", notches,
                   (int)rd32(race + 0xac4), range, view);
    }
}

/* the replay update (0x139c94, vtable slot 0x782edc) reads the pinch value once: a part of what the
   wheel still has to give is put there for that one call, and the value before it put back after */
#define REPLAY_UPDATE_SLOT 0x782edcu
#define REPLAY_UPDATE_FN 0x139c94u
static void (*replay_update_fn)(CPU *);

static void replay_update_hook(CPU *c)
{
    if (zoom_pending == 0 || *(volatile u8 *)GP(PINCH_ACTIVE)) {
        replay_update_fn(c);
        return;
    }
    float step = zoom_pending * 0.25f;         /* eased: a quarter of what is left each update */
    if (fabsf(step) < 2.0f)
        step = fabsf(zoom_pending) < 2.0f ? zoom_pending : (zoom_pending > 0 ? 2.0f : -2.0f);
    zoom_pending -= step;
    u32 saved = rd32(PINCH_DELTA);
    wr32(PINCH_DELTA, (u32)(int)lrintf(-step)); /* spreading the fingers is negative */
    replay_update_fn(c);
    wr32(PINCH_DELTA, saved);
    u32 race = zoom_pending == 0 ? replay_race() : 0;
    if (race && zoom_logs <= 40)
        rt_log("[replay] zoom now %d\n", (int)rd32(race + 0xac4));
}

void replay_init(void)
{
    if (!gt_hook_slot(REPLAY_UPDATE_SLOT, REPLAY_UPDATE_FN, &replay_update_fn, replay_update_hook,
                      "replay replay_update_hook"))
        rt_log("[replay] no wheel zoom\n");
}

/* GT_WHEEL=frame:notches;... turns the wheel on those frames (testing) */
static void test_wheel(void)
{
    static const char *list;
    if (!list)
        list = dev_getenv("GT_WHEEL") ? dev_getenv("GT_WHEEL") : "";
    int f;
    float n;
    while (*list && sscanf(list, "%d:%f", &f, &n) == 2 && (int)eagl_frames() >= f) {
        ui_zoom_wheel += n;
        dev_log("[replay] test wheel %+.0f at frame %u\n", n, eagl_frames());
        const char *s = strchr(list, ';');
        list = s ? s + 1 : "";
    }
}

/* REPLAY_TOUCH_ICONS (port addition): with Touch Screen = Off, the replay's three top icons (camera, pinch,
   swipe) are hidden.  The replay's top icons are the menu SWF's
   ReplayMenu.btnCamera (the camera, top left: a tap cycles the replay cameras, the C key and the pad do
   that too) and an unnamed clip of ReplayMenu (sprite 949, depth 7: the pinch-zoom and swipe-turn hints,
   touch gestures; the wheel and the mouse drag do those, REPLAY_ZOOM).  While Touch Screen is Off
   (PC_CONTROLS) their visible byte (+0xab) is cleared every frame of a replay (the ReplayMenu's labels
   place its children again); a hidden clip takes no tap, and the pad's menu pointer skips it.  Touch
   Screen On: shown again (the same characters only).  GT_REPLAY_TOUCH_ICONS=0 keeps them. */
#define CH_VISIBLE 0xab
#define SPR_LIST 0xb8                  /* a sprite's display list: array, count at +0xbc */

static u32 icons_hidden[8];            /* characters hidden by us */
static int nicons;

static int unnamed(u32 ch)
{
    u8 tag = *(u8 *)GP(ch + 0x44);
    const char *n = tag == 0xff ? (const char *)GP(rd32(ch + 0x50)) : (const char *)GP(ch + 0x45);
    return !*n;
}

static void hide_icon(u32 ch)
{
    volatile u8 *v = (volatile u8 *)GP(ch + CH_VISIBLE);
    if (!*v)
        return;
    *v = 0;
    for (int i = 0; i < nicons; i++)
        if (icons_hidden[i] == ch)
            return;
    if (nicons < 8)
        icons_hidden[nicons++] = ch;
}

static void replay_touch_icons(u32 race)
{
    static int on = -1;
    if (on < 0)
        on = option("GT_REPLAY_TOUCH_ICONS");
    if (!on)
        return;
    const char *top;
    u32 pl = race ? menu_player(&top) : 0;
    if (pcctl_keys_mode()) {
        if (!pl || !top || strcmp(top, "ReplayMenu"))
            return;
        static const char cam[] = "ReplayMenu.btnCamera", menu[] = "ReplayMenu";
        u32 ch = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)cam)), rm = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)menu));
        int had = nicons;
        if (ch)
            hide_icon(ch);
        u32 arr = rm ? rd32(rm + SPR_LIST) : 0;
        int n = rm ? (int)rd32(rm + SPR_LIST + 4) : 0;
        for (int i = 0; arr && i < n && i < 64; i++) {
            u32 k = rd32(arr + 4 * i);
            if (k && unnamed(k))
                hide_icon(k);
        }
        if (nicons != had)
            rt_log("[replay] Touch Screen off: the replay's camera, pinch and swipe icons hidden\n");
        return;
    }
    if (!nicons)
        return;
    /* Touch Screen On: back, if they are still the ReplayMenu's (else the menu SWF was made anew) */
    static const char menu[] = "ReplayMenu";
    u32 host = rd32(MENU_HOST), mp = host ? rd32(host + 0x1b2c) : 0;
    u32 rm = mp && rd32(mp + 0x3c) && rd32(mp + 0x40) ? rt_call2(GT_FN_SWF_FIND, mp, GA((void *)menu)) : 0;
    for (int i = 0; i < nicons; i++) {
        u32 ch = icons_hidden[i];
        if (rm && rd32(ch + 0x40) == rm)
            *(volatile u8 *)GP(ch + CH_VISIBLE) = 1;
    }
    nicons = 0;
    rt_log("[replay] Touch Screen on: the replay's touch icons shown again\n");
}

/* REPLAY_PAD (port addition): the replay's 360-degree camera, zoom and time slider on the pad.  While ReplayMenu
   is on top in a replay:
   - the right stick turns the camera: a finger held in the middle of the screen and moved with the stick
     (pcinput.c ui_pad_drag), as the mouse's drag and the iPad's swipe do;
   - Y / Triangle zooms in, X / Square out, held: what the wheel gives (REPLAY_ZOOM), a notch in 7 frames;
     the game zooms only in the replay views 4 .. 7 (R1 / C changes the view);
   - RT / R2 and LT / L2 go 10 s on and back (held: again every quarter second after half a second), by
     a tap on the replay bar's time slider where that time is, so the game's own slider code seeks
     (ReplayMenu's event handler 0x29c558: btnTimeSlider pauses the replay while touched, seeks with
     0x1af678(G + 0x1be4, x / the slider's width) and plays on when let go).  The replay at G + 0x1be4
     counts in quarter seconds (0x1afcc8 prints (now - start) / 4 as m:ss): start +0x2580, now +0x2584,
     length +0x2588.
   The numpad does the same: its digits are the stick's directions (8 up, 2 down, 4 left, 6 right, 7 9 1 3 between, at
   full tilt), + / - zoom in / out, * / / go 10 s on / back.
   GT_REPLAY_PAD=0 leaves the pad and the numpad as they were.  Testing: GT_REPLAY_PADTEST=from:to:keys;... holds keys on
   those frames (L R U D the stick, i o zoom, f b on / back). */
#define SEEK_STEP 40                           /* 10 s in quarter seconds */

void ui_pad_drag(float dx, float dy);
int ui_pad_dragging(void);
void ui_pad_tap(float x, float y);

/* a character's box on the stage (twips), from its own bound and its parent's world matrix */
static int stage_box(u32 ch, float b[4])
{
    static float rect[4];
    rect[0] = 1, rect[1] = 0;
    rt_call2(rd32(rd32(ch) + 0x12c), ch, GA(rect));
    if (!(rect[0] < rect[1] && rect[2] <= rect[3]))
        return 0;
    float m[6] = {1, 0, 0, 0, 1, 0};
    u32 parent = rd32(ch + 0x40), w = parent ? rt_call1(GT_FN_WORLD_MATRIX, parent) : 0;
    if (w)
        memcpy(m, GP(w), sizeof m);
    b[0] = b[2] = 1e30f, b[1] = b[3] = -1e30f;
    for (int k = 0; k < 4; k++) {
        float u = rect[k & 1], v = rect[2 + (k >> 1)];
        float x = m[0] * u + m[1] * v + m[2], y = m[3] * u + m[4] * v + m[5];
        b[0] = x < b[0] ? x : b[0], b[1] = x > b[1] ? x : b[1];
        b[2] = y < b[2] ? y : b[2], b[3] = y > b[3] ? y : b[3];
    }
    return 1;
}

static void replay_seek(u32 pl, int dir)
{
    u32 g = gt_game(), r = g ? g + G_REPLAY : 0;
    if (!r)
        return;
    int start = (int)rd32(r + 0x2580), now = (int)rd32(r + 0x2584), len = (int)rd32(r + 0x2588);
    static const char path[] = "ReplayMenu.ReplayBar.ReplayBarFull.btnTimeSlider";
    u32 sl = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path));
    float b[4];
    if (len <= 0 || !sl || !stage_box(sl, b) || b[1] < 0 || b[0] > 20480 || b[2] > 15360) {
        rt_log("[replay] pad %s 10 s: no slider on the screen (length %d)\n", dir > 0 ? "on" : "back", len);
        return;
    }
    if (ui_pad_dragging())
        return;
    int t = now - start + dir * SEEK_STEP;
    t = t < 0 ? 0 : t > len ? len : t;
    float frac = ((float)t + 0.5f) / (float)len;   /* the game truncates len x frac */
    /* a pixel inside the slider's ends: a tap at its very end is past its width for the game (in a
       test, on from 0:20 of 0:29 tapped at the end and did nothing) */
    float xt = b[0] + frac * (b[1] - b[0]);
    xt = xt < b[0] + 20 ? b[0] + 20 : xt > b[1] - 20 ? b[1] - 20 : xt;
    float x = xt / 20, y = (b[2] + b[3]) / 2 / 20;
    ui_pad_tap(x, y);
    rt_log("[replay] pad %s 10 s: %d:%02d -> %d:%02d of %d:%02d (slider tap at %.0f, %.0f)\n", dir > 0 ? "on" : "back",
           (now - start) / 240, (now - start) / 4 % 60, t / 240, t / 4 % 60, len / 240, len / 4 % 60, x, y);
}

/* GT_REPLAY_PADTEST: the keys held on this frame */
static const char *pad_test(void)
{
    static const char *env;
    static char keys[16];
    if (!env)
        env = dev_getenv("GT_REPLAY_PADTEST") ? dev_getenv("GT_REPLAY_PADTEST") : "";
    int now = (int)eagl_frames();
    keys[0] = 0;
    for (const char *p = env; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        int from, to;
        char k[8];
        if (sscanf(p, "%d:%d:%7[LRUDiofb]", &from, &to, k) == 3 && now >= from && now <= to &&
            strlen(keys) + strlen(k) < sizeof keys)
            strcat(keys, k);
    }
    return keys;
}

void replay_pad(float rx, float ry, int zoom_in, int zoom_out, int on, int back)
{
    static int opt = -1;
    static float speed;
    if (opt < 0) {
        opt = option("GT_REPLAY_PAD");
        speed = rt_setting_float("GT_REPLAY_PAD_SPEED", 14, 1, 100);   /* stage points a frame at full tilt */
    }
    const char *k = pad_test();
    if (*k) {
        rx += (strchr(k, 'R') != NULL) - (strchr(k, 'L') != NULL);
        ry += (strchr(k, 'D') != NULL) - (strchr(k, 'U') != NULL);
        zoom_in |= strchr(k, 'i') != NULL, zoom_out |= strchr(k, 'o') != NULL;
        on |= strchr(k, 'f') != NULL, back |= strchr(k, 'b') != NULL;
    }
    /* the numpad as the pad: the digits turn (8 up, 2 down, 4 left, 6 right, 7 9 1 3 between), + / - zoom,
       * / / go 10 s on / back */
    {
        float kx, ky;
        if (numpad_dir(&kx, &ky)) {
            float n = sqrtf(kx * kx + ky * ky);
            rx += kx / n, ry -= ky / n;
        }
        const bool *ks = SDL_GetKeyboardState(NULL);
        if (ks) {
            zoom_in |= ks[SDL_SCANCODE_KP_PLUS], zoom_out |= ks[SDL_SCANCODE_KP_MINUS];
            on |= ks[SDL_SCANCODE_KP_MULTIPLY], back |= ks[SDL_SCANCODE_KP_DIVIDE];
        }
    }
    const char *top = NULL;
    u32 pl = opt && replay_race() ? menu_player(&top) : 0;
    int in_replay = pl && top && !strcmp(top, "ReplayMenu");
    /* the stick: past a fifth of its travel, faster further out */
    float m = sqrtf(rx * rx + ry * ry);
    static int turning, turn_logs;
    if (!in_replay || m < 0.2f) {
        if (!hint_turn_frames)                 /* the swipe hint's own drag (replay_hint) goes on */
            ui_pad_drag(0, 0);
        turning = 0;
    } else {
        float a = ((m > 1 ? 1 : m) - 0.2f) / 0.8f, v = speed * a * a / m;
        ui_pad_drag(rx * v, ry * v);
        if (!turning++ && turn_logs++ < 20) {
            u32 g = gt_game();
            rt_log("[replay] pad: right stick turns the camera (replay view %d)\n", g ? (int)rd32(g + G_REPLAY_VIEW) : -1);
        }
    }
    static int zoom_was;
    int z = in_replay ? (zoom_in && !zoom_out ? 1 : zoom_out && !zoom_in ? -1 : 0) : 0;
    if (z) {
        ui_zoom_wheel += 0.15f * (float)z;
        if (z != zoom_was && zoom_logs++ < 40) {
            u32 g = gt_game();
            rt_log("[replay] pad zoom %s (replay view %d)\n", z > 0 ? "in" : "out", g ? (int)rd32(g + G_REPLAY_VIEW) : -1);
        }
    }
    zoom_was = z;
    static int seek_was, next_at;
    int now = (int)eagl_frames();
    int dir = in_replay ? (on && !back ? 1 : back && !on ? -1 : 0) : 0;
    if (dir && (dir != seek_was || now >= next_at)) {
        next_at = now + (dir != seek_was ? 30 : 15);
        replay_seek(pl, dir);
    }
    seek_was = dir;
}

/* REPLAY_HINT_PAD (padmenu.c): Cross on the replay's pinch or swipe hint does that gesture.
   - The pinch (which 1): a step of a quarter of the zoom range (race +0xac4, 0 .. 0.8 x [0x7711c0]) inwards, given
     as the wheel's zoom is (REPLAY_ZOOM, eased over a few updates); from the last step (the closest view) back to
     0, the normal view.  The game zooms in the replay views 4 .. 7 only; in the others the press is logged.
   - The swipe (which 2): the free camera turned by 90 degrees, by a finger held in the middle of the screen and
     moved sideways (pcinput.c ui_pad_drag), as the mouse's drag and the iPad's swipe do.  The turn is the replay
     camera's yaw, race +0xac0 (radians: a test drag of 120 frames moved it by 1.6), read each frame: the drag
     stops once it has turned a quarter turn (in a test in view 4: 90 degrees in 55 frames, the car seen from its
     side), or after 2 s whatever it reached.  As with the zoom, only the views 4 .. 7 turn; in the others the
     press is logged.  A change of view sets the yaw back, as after a mouse drag. */
void replay_hint(int which)
{
    u32 race = replay_race(), g = gt_game();
    int view = g ? (int)rd32(g + G_REPLAY_VIEW) : -1;
    if (!race)
        return;
    if (which == 1) {
        int screen = (int)rd32(GT_SCREEN_PX_H);
        float range = 0.8f * (float)(screen > 0 ? screen : 1024), now = (float)(int)rd32(race + 0xac4) + zoom_pending;
        if (view < 4 || view > 7) {
            rt_log("[replay] pinch hint: replay view %d does not zoom (4 .. 7 do)\n", view);
            return;
        }
        /* four steps: the step nearest the zoom now, then the next (the zoom is kept in whole units) */
        float step = range / 4, level = floorf(now / step + 0.5f);
        float to = level >= 4 ? 0 : fminf((level + 1) * step, range);
        zoom_pending += to - now;
        rt_log("[replay] pinch hint: zoom %.0f -> %.0f of %.0f (replay view %d)\n", now, to, range, view);
    } else {
        if (view < 4 || view > 7) {     /* the views 1 .. 3 take no turn (tested in 2 and 3: the yaw stayed 0) */
            rt_log("[replay] swipe hint: replay view %d does not turn (4 .. 7 do)\n", view);
            return;
        }
        float yaw;
        memcpy(&yaw, GP(race + 0xac0), 4);
        hint_turn_from = yaw;
        hint_turn_frames = 120;
        rt_log("[replay] swipe hint: turning the camera 90 degrees (yaw %.2f, replay view %d)\n", yaw, view);
    }
}

/* the swipe hint's turn, once a frame while it lasts */
static void replay_hint_turn(u32 race)
{
    if (!hint_turn_frames)
        return;
    float yaw;
    memcpy(&yaw, GP(race + 0xac0), 4);
    const float quarter = 1.5707963f, full = 6.2831853f;
    float d = fmodf(fabsf(yaw - hint_turn_from), full);
    if (d > full / 2)
        d = full - d;
    if (d >= quarter || !--hint_turn_frames) {
        ui_pad_drag(0, 0);
        rt_log("[replay] swipe hint: turned %.0f degrees in %d frames (yaw %.2f)\n", d * 57.29578f,
               120 - hint_turn_frames, yaw);
        hint_turn_frames = 0;
        return;
    }
    ui_pad_drag(d > 0.8f * quarter ? 6.0f : 14.0f, 0);   /* slower near the end, so it stops close to 90 */
}

void replay_frame(void)
{
    test_wheel();
    u32 race = replay_race();
    replay_undim(race);
    replay_zoom(race);
    replay_touch_icons(race);
    if (race)
        replay_hint_turn(race);
    else
        hint_turn_frames = 0;
}
