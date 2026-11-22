/* hudhide.c -- TOUCH_HUD_HIDE (port addition): with Touch Screen Off, the pause, pedal and gear touch icons are
   hidden.

   With Options > Controls > Touch Screen Off (PC_CONTROLS, pcctl.c) the race is driven by the keyboard or
   the pad, yet the race HUD (HUD SWF LHB_lN_QZMi*.FAK, the HUD menu: vtable 0x788c48) still drew its touch
   controls: the pause icon top left, the gear arrows bottom left, the pedals bottom right.  Now, while
   Touch Screen is Off, they are not drawn:
   - the pedals: HUD.AccelBrake.btnAccel / btnBrake / btnBrakeLeft / btnBrakeRight (which ones exist
     depends on the throttle setting: the HUD's layout 0x27dc40 picks the AccelBrake label PedalsNear /
     PedalsFar / OneBrake / BrakesOnly), and the gear arrows HUD.Gears (label LeftGears; "None" for an
     automatic gearbox).  Their visible byte (+0xab) is cleared right before each HUD draw (slot +0x20,
     0x27bf84, the HUD's own per-frame draw; its slot +0x1c, the menus' common update 0x258654, is never
     called for the HUD, so a clear there does nothing), as the game shows them again itself (0x50bde0)
     after a pause or the start.  Hidden is not hit, which is fine: the keys and the pad write the HUD's
     held bytes directly (steer.c), not through these buttons.  The tutorial inside AccelBrake is left alone.
   - the pause icon HUD.MovingElements.Home (its button btnHome; HUD.btnBack top left is another button,
     hidden by the game during a race): made fully transparent every frame (alpha 0 with the game's own
     0x50bdfc), not hidden, because Esc and the pad's Options pause the race by a tap on it (pcinput.c
     PAUSE_X / PAUSE_Y), which a hidden button does not take (with visible 0 Esc does not
     pause).  The game fades it itself (after a resume), so it is set again whenever its alpha is not 0;
     Touch Screen On gives it alpha 1, as the game leaves it.
   Touch Screen On again: what was hidden is shown once the race is driven again with no menu over it
   (not while the pause menu is up, where the game keeps the HUD's controls hidden itself).
   Tested on a save copy (Test Center, keys mode manual + pedals): the three are gone, Esc
   pauses and resumes, they stay gone after the resume; with Touch Screen On the HUD is the game's own.
   GT_HIDE_TOUCH_HUD=0: the touch controls stay on the screen in both modes.
   The same per-frame hook moves the HUD's parts to a wide screen's edges (widescreen.c wide_hud), so it
   is installed in both cases. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "features.h"
#include "game.h"

#define HUD_VTBL 0x788c48u            /* the race HUD menu's vtable */
#define HUD_DRAW_FN 0x27bf84u         /* slot +0x20: the HUD's own per-frame draw */
#define FN_SET_ALPHA 0x50bdfcu        /* (swf, path, float alpha) */
#define CH_VISIBLE 0xab               /* character byte: drawn and hit */
#define CH_CXFORM 0x58                /* -> its colour transform (8 floats, set by 0x50df1c) */
#define CX_ALPHA_MULT 0x18
#define PAUSE_ICON "HUD.MovingElements.Home"

/* under HUD (looked up once a frame, the parts under it: swffind.c swf_find_under, pass 4) */
static const char *const parts[] = {
    "AccelBrake.btnAccel", "AccelBrake.btnBrake", "AccelBrake.btnBrakeLeft", "AccelBrake.btnBrakeRight", "Gears", NULL};
static u32 hidden_by_us[8];           /* the character hidden, per parts entry */
static u32 faded;                     /* the pause icon made transparent (its character) */
static int on = -1;
static void (*hud_draw_fn)(CPU *);

static u32 find(u32 pl, const char *path) { return rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path)); }
u32 swf_find_under(u32 pl, u32 parent, const char *rest);

static void hide_parts(u32 pl)
{
    int hide = pcctl_keys_mode();
    /* back on only in the race itself (the pause menu keeps the HUD's controls hidden) */
    int show = !hide && steer_racing() && !menu_top();
    u32 hud = find(pl, "HUD");
    for (int i = 0; parts[i]; i++) {
        u32 ch = swf_find_under(pl, hud, parts[i]);
        volatile u8 *v = ch ? (volatile u8 *)GP(ch + CH_VISIBLE) : NULL;
        if (hide) {
            if (v && *v) {
                *v = 0;
                hidden_by_us[i] = ch;
            }
        } else if (show && hidden_by_us[i]) {
            if (v && hidden_by_us[i] == ch)
                *v = 1;
            hidden_by_us[i] = 0;
        }
    }
    u32 icon = swf_find_under(pl, hud, PAUSE_ICON + 4);     /* "MovingElements.Home" under HUD */
    if (hide && icon) {
        /* every frame: the HUD sets its colour transform again (seen: a single alpha 0 did not hold) */
        u32 cx = rd32(icon + CH_CXFORM);
        if (!cx || *(volatile float *)GP(cx + CX_ALPHA_MULT) != 0.0f)
            rt_call3(FN_SET_ALPHA, pl, GA((void *)PAUSE_ICON), 0);          /* 0.0f */
        if (icon != faded)
            rt_log("[hudhide] Touch Screen off: pause icon, pedals and gear arrows not drawn\n");
        faded = icon;
    } else if (!hide && faded) {
        if (icon == faded)
            rt_call3(FN_SET_ALPHA, pl, GA((void *)PAUSE_ICON), 0x3f800000u);  /* 1.0f */
        faded = 0;
        rt_log("[hudhide] Touch Screen on: the HUD's touch controls are drawn again\n");
    }
}

static void hud_draw_hook(CPU *c)
{
    u32 pl = rd32(c->r[0] + 4);
    static int logged;
    if (!logged++)
        rt_log("[hudhide] HUD drawn (player 0x%x)\n", pl);
    if (on > 0 && pl)
        hide_parts(pl);
    if (pl)
        wide_hud(pl);                 /* WIDE_UI: the HUD's parts to the screen's edges (widescreen.c) */
    hud_draw_fn(c);
}

void hudhide_init(void)
{
    const char *e = getenv("GT_HIDE_TOUCH_HUD");
    on = !(e && *e == '0');
    if (!on)
        rt_log("[hudhide] off (GT_HIDE_TOUCH_HUD=0): the touch controls stay on the race HUD\n");
    if (!gt_hook_slot(HUD_VTBL + 0x20, HUD_DRAW_FN, &hud_draw_fn, hud_draw_hook, "hudhide hud_draw_hook"))
        rt_log("[hudhide] touch controls stay, HUD not widened\n");
}
