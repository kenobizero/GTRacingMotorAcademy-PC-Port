/* exitbtn.c -- EXIT_BUTTON (port addition): an Exit button right of the settings gear on the main menu.

   The original has no way out of the game: on the iPad the Home button did that.  On the PC only closing
   the window did.  The main menu (NavigationMenu) now has an Exit button right of the gear (Options) in
   its bottom-left bar: btnPCExit, added to the menu SWF by tools/swfpatch_exit.py at build time (a copy of
   the gear button with a power symbol on the same yellow tile; the original shows btnCashbonus there only
   when Gameloft's ad server offers free cash, never offline).
   - A click on it asks first, in NavigationMenu's own yes / no popup, FlurryPopup (in the original it
     offers a Flurry video for free cash; Flurry is gone, so it never opens otherwise).  Texts as in
     shop.c: 0x50c038(swf, path, utf8, 0); it opens and closes with the labels "Show" / "Hided"
     (0x50bc94).  Its buttons reach NavigationMenu's event handler as releases (type 6) named btnYes /
     btnNo; the handler (vtable 0x786ed8 slot +0x2c = 0x1eb5ac) has its own case for FlurryPopup's btnYes
     (the video), so while our question is open the handler gets them nameless.
   - YES ends the game the way closing the window does (ui_quit: the app delegate hears it will resign
     active, enter the background and terminate, so the game saves as on the iPad), NO closes the popup.
   The pad and the keyboard reach the button and the popup like any other (padmenu.c: Circle / Esc on the
   popup is NO).  GT_EXIT_BUTTON=0 hides the button. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "features.h"
#include "game.h"

#define NAV_VTBL 0x786ed8u            /* NavigationMenu's vtable */
#define NAV_UPDATE_FN 0x1e95c8u       /* slot +0x1c */
#define NAV_EVENT_FN 0x1eb5acu        /* slot +0x2c */
#define CH_VISIBLE 0xab               /* character byte: drawn and hit */

#define BUTTON "btnPCExit"
#define POPUP "NavigationMenu.FlurryPopup"

static int on = -1;
static int asking;                    /* our question is open */
static void (*nav_event_fn)(CPU *), (*nav_update_fn)(CPU *);

static void set_text(u32 pl, const char *field, const char *s)
{
    char p[96];
    snprintf(p, sizeof p, POPUP ".Texts.%s", field);
    if (rt_call2(GT_FN_SWF_FIND, pl, GA(p)))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA(p), GA((void *)s), 0);
}

static void ask(u32 pl)
{
    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)POPUP), GA((void *)"Show"), 1);
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA((void *)POPUP ".btnYes"), 1);
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA((void *)POPUP ".Texts.txtYes"), 1);
    set_text(pl, "field", "EXIT THE GAME?");
    set_text(pl, "txtYes.Field", "YES");
    set_text(pl, "txtNo.Field", "NO");
    asking = 1;
    rt_log("[exit] Exit pressed: asking\n");
}

/* [ev] control, [ev+4] its name, [ev+8] kind (6 = released on it) */
static void nav_event_hook(CPU *c)
{
    u32 self = c->r[0], ev = c->r[1], pl = rd32(self + 4);
    int kind = (int)rd32(ev + 8);
    const char *name = rd32(ev + 4) ? (const char *)GP(rd32(ev + 4)) : "";
    board_nav_event(name, kind);        /* RED_BULL_BOARD (board.c): the Red Bull page's Leaderboard tile */
    if (topcars_event(c))
        return;
    if (p2p_nav_event(c)) {
        /* FIRST_NAME (p2p.c): the first start's name form, in the same popup */
        static const char none[] = "";
        u32 old = rd32(ev + 4);
        wr32(ev + 4, GA((void *)none));
        nav_event_fn(c);
        wr32(ev + 4, old);
        return;
    }
    if (!strcmp(name, BUTTON)) {
        /* ours: only the menus' common handler (NavigationMenu's knows no such button) */
        rt_call2(GT_FN_MENU_BASE_EVENT, self, ev);
        if (on > 0 && kind == 6 && pl && !asking && !p2p_first_open())
            ask(pl);
        return;
    }
    int yes = !strcmp(name, "btnYes"), no = !strcmp(name, "btnNo");
    if (asking && (yes || no)) {
        u32 path = rt_call1(GT_FN_EVENT_PATH, ev);
        if (path && strstr((const char *)GP(path), "FlurryPopup")) {
            if (kind == 6) {
                asking = 0;
                if (pl)
                    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)POPUP), GA((void *)"Hided"), 0);
                if (yes) {
                    rt_log("[exit] YES: the game ends (as when the window is closed)\n");
                    ui_quit = 1;
                } else {
                    rt_log("[exit] NO: back to the menu\n");
                }
            }
            /* the handler's own FlurryPopup case (the video offer) must not see it */
            static const char none[] = "";
            u32 old = rd32(ev + 4);
            wr32(ev + 4, GA((void *)none));
            nav_event_fn(c);
            wr32(ev + 4, old);
            return;
        }
    }
    nav_event_fn(c);
}

/* GT_EXIT_BUTTON=0: the button stays hidden (the menu file has it either way) */
static void nav_update_hook(CPU *c)
{
    u32 self = c->r[0];
    nav_update_fn(c);
    u32 pl = rd32(self + 4);
    menu3d_nav_update(pl);              /* MENU_3D: the 3D scene behind it (menu3d.c) */
    if (pl)
        wide_nav(pl);                   /* WIDE_UI: its left bar at a wide screen's left edge (widescreen.c) */
    p2p_nav_update(pl);                 /* FIRST_NAME: a first start asks the name (p2p.c) */
    maxmoney_nav_update(self);          /* MAX_STATS cheat code: applied, title bar redrawn (maxmoney.c) */
    if (on == 0 && pl) {
        u32 ch = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)"NavigationMenu." BUTTON));
        if (ch && *(volatile u8 *)GP(ch + CH_VISIBLE))
            *(volatile u8 *)GP(ch + CH_VISIBLE) = 0;
    }
}

void exitbtn_init(void)
{
    const char *e = getenv("GT_EXIT_BUTTON");
    on = !(e && *e == '0');
    if (gt_hook_slot(NAV_VTBL + 0x2c, NAV_EVENT_FN, &nav_event_fn, nav_event_hook, "exitbtn nav_event_hook") &&
        gt_hook_slot(NAV_VTBL + 0x1c, NAV_UPDATE_FN, &nav_update_fn, nav_update_hook, "exitbtn nav_update_hook"))
        rt_log("[exit] %s\n", on ? "Exit button on the main menu" : "off (GT_EXIT_BUTTON=0): the button is hidden");
}
