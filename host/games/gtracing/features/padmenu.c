/* padmenu.c -- PAD_MENU_SNAP (port addition): with a pad, the pointer snaps to the menu choices by itself, as in
   console racing games.

   The iPad game has no pointer and no pad: its menus (gameswf, with Gameloft's MenuFX on top) take
   touches only.  With a pad, the D-pad or a flick of the left stick now jumps the gold pointer from
   one menu button to the next in that direction (held: repeats), Cross (Xbox A) taps the button
   under it and Circle (Xbox B) taps the menu's back button (else a popup's Close / Cancel / No, or
   the pause menu's Resume).  The first press only puts the pointer on the button nearest it.  While
   the pad holds the pointer it stays on its button: it follows a sliding button, lands on the nearest
   one when its button goes (a new menu, a popup over it), and goes back to the button a popup took it
   from when the popup closes.  NavigationMenu's wheel (COMMUNITY / TOP 10 / QUEST / RACE ...) turns
   by a swipe only, so on it up / down swipe it; at its end the press leaves it.  EventsMenu's cards
   are a carousel slid by a sideways swipe over its btnDrag: left / right on its middle card swipe
   it; on the card's other buttons they go to Garage / Enter
   Event, see find_card).  The mouse works unchanged; moving it ends the
   pad's hold on the pointer.  On while a menu is on the screen (menu_shown: the in-race player's
   menus are hidden, not popped, while the race is driven); off while a race waits for its start.
   Checked on a test save (GT_PADNAV): main menu, the shop (packs, the buy
   confirm, No, back), NavigationMenu's wheel (down twice to RACE, up to QUEST), a popup's OK, the
   pause menu (twice, with Resume between, and its Options), EventsMenu (slide, Enter Event).  A
   look over the buttons takes ~1 ms (0.3 .. 3.7 ms) every 8 frames, 60 fps kept.

   How the buttons are found (static reading of the shipped binary, gameswf symbols are in it):
   - the menus' MenuFX player (G + 0x19d4 main menus, G + 0x1b2c in a race; player_now) holds the
     gameswf player at +0x3c, whose +0x10 is the root movie (0x4ed8d8);
   - a sprite (vtable 0x78dac8, is(2) true) keeps its display list at +0xb8 (array) / +0xbc (count),
     a character its visible byte at +0xab, its parent at +0x40, its name (tu_string) at +0x44 (0xff:
     the text at [+0x50], else inline at +0x45);
   - vtable +0x15c is can_handle_mouse_event (true for a clip with button events or one MenuFX
     treats as a button), +0x12c get_bound (x min, x max, y min, y max in the PARENT's space),
     +0x68 get_topmost_mouse_entity(x, y) (the stage point, landscape points x 20 twips); 0x4ca618
     gives a character's world matrix {a, b, tx, c, d, ty};
   - gameswf's hit test (sprite 0x4f62b4) hands a touch to the OUTERMOST clip that takes mouse
     events, top child first, so a button is kept only where that same hit test finds it: at its
     centre, or else at the reachable part of a grid over it (the shop's pack rows: an unnamed newBG
     clip lies over their middle, only the price buttons take a touch; the shop's btnBackTop reaches
     0,-768 .. 1024,80 under the top bar, only its arrow at the left is free).  That drops buttons
     under a popup's click blocker, inside another button, or off the stage; btnBlock / ClickBlocker
     catchers and full-screen clips are no choice.
   The pointer glides there rather than jumping (PAD_GLIDE, ui_pointer_to in pcinput.c, GT_PAD_GLIDE ms).
   GT_PAD_MENU=0 switches it off (the pad's Cross then stays gear up only).  Testing: GT_PADNAV=
   frame:keys;... presses u d l r (D-pad), a (Cross), b (Circle) on those frames, GT_PADMENU_LOG=1
   logs each scan's buttons. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "features.h"
#include "game.h"

#define ROOT_FN 0x4ed8d8u               /* gameswf player -> root movie */
#define VT_IS 0x08u                     /* is(type): 2 = sprite_instance */
#define VT_HIT 0x68u                    /* get_topmost_mouse_entity(x, y) */
#define VT_BOUND 0x12cu                 /* get_bound(rect *) */
#define VT_MOUSE 0x15cu                 /* can_handle_mouse_event() */
#define TWIPS 20.0f                     /* stage units per landscape point */
#define STAGE_W 1024.0f                 /* the stage's width: the host's points are the stage's (also on a
                                           wide screen, WIDE_UI: pcinput.c turns them into the screen's) */

#define MAX_BUTTONS 96

typedef struct {
    u32 ch;                             /* the clip that takes the touch */
    float x0, y0, x1, y1;               /* landscape points, the part a touch reaches */
    float sx, sy;                       /* where the pointer goes and the tap lands */
    int wheel;                          /* a wheel menu, turned by up / down; wy: its middle row */
    float wy;
    int card;                           /* the middle card of a carousel with no button on it (its drag area) */
    int hint;                           /* the replay's pinch (1) or swipe (2) hint (REPLAY_HINT_PAD) */
    char name[32];
} Button;

static Button btn[MAX_BUTTONS];
static int nbtn;

/* the drag areas (clips named btnDrag*) on the screen: EventsMenu's card carousel turns by a sideways
   swipe over one (-41,97 .. 983,647; the cards EventFile_N, the middle one's btnChange), AftermarketMenu's
   (the upgrade parts, 3,78 .. 1024,628; the cards MarketFile_N, no button on them) too */
typedef struct {
    float x0, y0, x1, y1;
    u32 ch;
    int files;                          /* cards (clips named *File_N) beside it: a card carousel */
    float mx0, my0, mx1, my1;           /* the card in its middle (files > 0 and mx1 > mx0) */
} Area;
static Area drag[8];
static int ndrag;
static int dbg = -1;

static u32 f2u(float f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

static const char *char_name(u32 ch)
{
    u8 tag = *(u8 *)GP(ch + 0x44);
    return tag == 0xff ? (const char *)GP(rd32(ch + 0x50)) : (const char *)GP(ch + 0x45);
}

static u32 vcall(u32 ch, u32 slot) { return rd32(rd32(ch) + slot); }

/* the MenuFX player of the menus on screen (vtable 0x78dfc8; found by a scan of the game object):
   G + 0x19d4 runs the main menus (MainMenu, NavigationMenu, TestCenterMenu ...),
   G + 0x1b44 the Paddocks (LotteryMenu, Lottery_truck), G + 0x1b2c the in-race ones (PauseMenu,
   EndMenu, ReplayMenu); *top = its top menu's name */
static u32 player_now(const char **top)
{
    *top = NULL;
    u32 g = gt_game();
    if (!g)
        return 0;
    u32 pl = rd32(g + (rd32(g + G_RACE) ? 0x1b2c : 0x19d4));
    /* the Paddocks (the trucks Free / Silver / Gold, Rewards): G + 0x19d4 is 0 there and LotteryMenu runs
       in a third MenuFX player at G + 0x1b44 (a scan of the game object at that screen), so
       the pad had no buttons there (port fault: the Paddocks could not be used with a pad) */
    /* after an event's or a licence's last race Next frees the race and the event's prize screen
       (0x29f4b0, "Congratulations! / Prize / Total / Next") shows in a fourth MenuFX player, G + 0x1b38
       (0x29f4e8 loads it), before the main menus' player is up (see menu_shown).  G + 0x1b2c alone
       leaves the next event with no pad (log: flags 0 / 0 after Next); the code of 0x29f4b0
       shows 0x1b38 */
#define PLAYER_UP(p) ((p) && rd32(p) == 0x78dfc8u && rd32((p) + 0x3c) && rd32((p) + 0x40) && menu_shown(p))
    if (!rd32(g + G_RACE) && !PLAYER_UP(pl))
        pl = PLAYER_UP(rd32(g + G_PRIZE_MENUS)) ? rd32(g + G_PRIZE_MENUS)
           : PLAYER_UP(rd32(g + G_RACE_MENUS)) ? rd32(g + G_RACE_MENUS) : rd32(g + G_PADDOCK_MENUS);
#undef PLAYER_UP
    if (!pl || rd32(pl) != 0x78dfc8u || !rd32(pl + 0x3c) || !rd32(pl + 0x40) || !menu_shown(pl))
        return 0;
    int n = (int)rd32(pl + 0x118);
    u32 arr = rd32(pl + 0x114), e = n > 0 && arr ? rd32(arr + 4 * (n - 1)) : 0;
    *top = e ? (const char *)GP(e + 8) : NULL;
    return pl;
}

static void world_of(u32 ch, float m[6])
{
    static const float id6[6] = {1, 0, 0, 0, 1, 0};
    u32 w = ch ? rt_call1(GT_FN_WORLD_MATRIX, ch) : 0;
    memcpy(m, w ? GP(w) : id6, sizeof(float) * 6);
}

/* a character's box on the stage, in landscape points (0 when nothing is drawn in it) */
static int stage_box(u32 ch, float *x0, float *y0, float *x1, float *y1)
{
    static float rect[4];
    rt_call2(vcall(ch, VT_BOUND), ch, GA(rect));
    if (!(rect[0] <= rect[1] && rect[2] <= rect[3]))
        return 0;
    float m[6];
    world_of(rd32(ch + 0x40), m);
    *x0 = 1e9f, *y0 = 1e9f, *x1 = -1e9f, *y1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float x = rect[k & 1], y = rect[2 + (k >> 1)];
        float sx = (m[0] * x + m[1] * y + m[2]) / TWIPS, sy = (m[3] * x + m[4] * y + m[5]) / TWIPS;
        *x0 = fminf(*x0, sx), *x1 = fmaxf(*x1, sx), *y0 = fminf(*y0, sy), *y1 = fmaxf(*y1, sy);
    }
    return 1;
}

static void push(u32 ch, float x0, float y0, float x1, float y1, const char *name)
{
    if (nbtn >= MAX_BUTTONS)
        return;
    Button *b = &btn[nbtn++];
    memset(b, 0, sizeof *b);
    b->ch = ch;
    b->x0 = x0, b->y0 = y0, b->x1 = x1, b->y1 = y1;
    snprintf(b->name, sizeof b->name, "%s", name);
}

/* the rows' place in a wheel (a sum of the y of its txt* items): it changes when the wheel turns */
static float wheel_sig(u32 ch, int depth)
{
    float sum = 0, x0, y0, x1, y1;
    u32 arr = rd32(ch + 0xb8);
    int n = (int)rd32(ch + 0xbc);
    for (int i = 0; arr && i < n && i < 128; i++) {
        u32 k = rd32(arr + 4 * i);
        if (!k)
            continue;
        if (!strncmp(char_name(k), "txt", 3) && stage_box(k, &x0, &y0, &x1, &y1))
            sum += y0 * (float)(i + 1);
        else if (depth < 3 && rt_call2(vcall(k, VT_IS), k, 2))
            sum += wheel_sig(k, depth + 1);
    }
    return sum;
}

/* the cards beside a drag area (its parent's clips named *File_N: EventFile_N, MarketFile_N; the only
   such names in the menu SWFs) and the one across the area's middle */
static void find_files(Area *a, u32 parent)
{
    u32 arr = parent ? rd32(parent + 0xb8) : 0;
    int n = parent ? (int)rd32(parent + 0xbc) : 0;
    float mx = (a->x0 + a->x1) / 2, best = 0, x0, y0, x1, y1;
    for (int i = 0; arr && i < n && i < 128; i++) {
        u32 k = rd32(arr + 4 * i);
        if (!k || !*(u8 *)GP(k + 0xab) || !strstr(char_name(k), "File_") || !stage_box(k, &x0, &y0, &x1, &y1))
            continue;
        a->files++;
        if (x0 <= mx && x1 >= mx && (x1 - x0) * (y1 - y0) > best)
            best = (x1 - x0) * (y1 - y0), a->mx0 = x0, a->my0 = y0, a->mx1 = x1, a->my1 = y1;
    }
}

static void add_button(u32 ch)
{
    float x0, y0, x1, y1;
    if (!stage_box(ch, &x0, &y0, &x1, &y1))
        return;                                         /* nothing drawn in it */
    /* the part on the stage (a clip may reach far off it, as the shop's btnBackTop: 0,-768 .. 1024,80) */
    x0 = fmaxf(x0, 0), y0 = fmaxf(y0, 0), x1 = fminf(x1, STAGE_W), y1 = fminf(y1, 768);
    const char *name = char_name(ch);
    if (!strncmp(name, "btnDrag", 7) && ndrag < 8 && x1 - x0 > 8 && y1 - y0 > 8) {
        Area *a = &drag[ndrag++];
        memset(a, 0, sizeof *a);
        a->x0 = x0, a->y0 = y0, a->x1 = x1, a->y1 = y1, a->ch = ch;
        find_files(a, rd32(ch + 0x40));
    }
    /* a full-screen button with a small hitzone: QR_ClassMenu's btnBackTop on a second visit (Back,
       then RACE again) has its btnBlock moved onto the stage (0,0 .. 1023,768); its arrow's hitzone
       (2,0 .. 52,80) is the part to touch (Circle found no back button there without it).
       A hitzone well off the box's middle too: the event prize screen's Next (FirstEventsScreen's
       btnAwards) is the right-aligned word NEXT (its text field 714 .. 839) and the arrow (855 .. 965);
       its box's middle, 846,690, fell in the gap between them, the hitzone (831 .. 981) is on the
       arrow (port fault: the pointer sat off the Next button; the tap
       there still worked).  A hitzone near the middle leaves the box as it was */
    if (rt_call2(vcall(ch, VT_IS), ch, 2)) {
        int full = (x1 - x0) * (y1 - y0) >= 0.5f * STAGE_W * 768;
        u32 arr = rd32(ch + 0xb8);
        int n = (int)rd32(ch + 0xbc);
        float hx0, hy0, hx1, hy1;
        for (int i = 0; arr && i < n && i < 128; i++) {
            u32 k = rd32(arr + 4 * i);
            if (k && *(u8 *)GP(k + 0xab) && !strcmp(char_name(k), "hitzone") && stage_box(k, &hx0, &hy0, &hx1, &hy1)) {
                hx0 = fmaxf(hx0, 0), hy0 = fmaxf(hy0, 0), hx1 = fminf(hx1, STAGE_W), hy1 = fminf(hy1, 768);
                float off = fmaxf(fabsf((hx0 + hx1) - (x0 + x1)), fabsf((hy0 + hy1) - (y0 + y1))) / 2;
                if (hx1 > hx0 && hy1 > hy0 && (hx1 - hx0) * (hy1 - hy0) < 0.5f * STAGE_W * 768 &&
                    (full || (off > 24 && hx1 - hx0 >= 8 && hy1 - hy0 >= 8))) {
                    if (!full && GT_DEV && dbg > 0)
                        rt_log("[padmenu] %s: its hitzone %.0f,%.0f .. %.0f,%.0f (box %.0f,%.0f .. %.0f,%.0f)\n",
                               char_name(ch), hx0, hy0, hx1, hy1, x0, y0, x1, y1);
                    x0 = hx0, y0 = hy0, x1 = hx1, y1 = hy1;
                }
                break;
            }
        }
        /* the Paddocks' trucks (LotteryMenu's btnSelection01..03: Gold, Silver, Free): a hitzone over the
           whole truck and its sign; the pointer goes to the sign, its "bar" (Free, Silver, Gold) */
        if (!strncmp(name, "btnSelection", 12))
            for (int i = 0; arr && i < n && i < 128; i++) {
                u32 k = rd32(arr + 4 * i);
                if (k && *(u8 *)GP(k + 0xab) && !strcmp(char_name(k), "bar") && stage_box(k, &hx0, &hy0, &hx1, &hy1)) {
                    x0 = hx0, y0 = hy0, x1 = hx1, y1 = hy1;
                    break;
                }
            }
    }
    if (strstr(name, "Block") ||                        /* btnBlock, btnClickBlocker: touch catchers */
        x1 - x0 < 4 || y1 - y0 < 4 ||
        (x1 - x0) * (y1 - y0) >= 0.5f * STAGE_W * 768)  /* full-screen catchers are no choice */
        return;
    /* a wheel (NavigationMenu's btnLeftMenu: COMMUNITY / TOP 10 / QUEST / RACE ... between arrowup and
       arrowdown): the game turns it by a swipe only (a test tap on a row or an arrow did nothing, a
       swipe up of 120 points turned TOP 10 to QUEST), so it is one choice, its spot the middle row */
    float wy0 = -1, wy1 = -1, ax0, ay0, ax1, ay1;
    if (rt_call2(vcall(ch, VT_IS), ch, 2)) {
        u32 arr = rd32(ch + 0xb8);
        int n = (int)rd32(ch + 0xbc);
        for (int i = 0; arr && i < n && i < 128; i++) {
            u32 k = rd32(arr + 4 * i);
            if (k && *(u8 *)GP(k + 0xab) && stage_box(k, &ax0, &ay0, &ax1, &ay1)) {
                if (!strcmp(char_name(k), "arrowup"))
                    wy0 = ay1;
                else if (!strcmp(char_name(k), "arrowdown"))
                    wy1 = ay0;
            }
        }
    }
    push(ch, x0, y0, x1, y1, name);
    if (wy0 >= 0 && wy1 > wy0) {
        Button *b = &btn[nbtn - 1];
        b->wheel = 1;
        b->wy = (wy0 + wy1) / 2;
    }
}

static void dump_tree(u32 ch, int depth)   /* testing (GT_PADMENU_TREE=1 with GT_PADMENU_LOG=1): the display list at each press */
{
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int box = stage_box(ch, &x0, &y0, &x1, &y1), spr = rt_call2(vcall(ch, VT_IS), ch, 2);
    int mouse = rt_call1(vcall(ch, VT_MOUSE), ch);
    rt_log("[tree] %*s%s%s%s %4.0f,%4.0f .. %4.0f,%4.0f%s\n", depth * 2, "", char_name(ch), spr ? " [s]" : "",
           mouse ? " [btn]" : "", x0, y0, x1, y1, box ? "" : " (empty)");
    if (!spr || depth > 7)
        return;
    u32 arr = rd32(ch + 0xb8);
    int n = (int)rd32(ch + 0xbc);
    for (int i = 0; arr && i < n && i < 128; i++) {
        u32 k = rd32(arr + 4 * i);
        if (k && *(u8 *)GP(k + 0xab))
            dump_tree(k, depth + 1);
    }
}

/* a closed popup: MenuFX moves a popup off the stage (its btnClickBlocker at 1024 .. 2048) rather than
   hiding it, and a part of it may stay on the stage: EventsMenu's RequirementsPopup leaves its
   AllRestrictions icons at 235,349 over the middle card (nothing drawn there, but the hit test finds
   it: left from the Restrictions bar went there, not to Garage).  Closed only when
   none of its blockers is on the stage: FlurryPopup (the shop's "BUY 15 CASH FOR 5,250 COINS?", the
   Paddocks offer) keeps btnClickBlocker on the stage while open and a second btnClickBlocker2 off it,
   so one blocker off the stage took the open popup's Yes / No out (port fault: Yes, No and OK could not be
   picked; the log said "no button found") */
static int closed_popup(u32 ch)
{
    u32 arr = rd32(ch + 0xb8);
    int n = (int)rd32(ch + 0xbc), off = 0;
    float x0, y0, x1, y1;
    for (int i = 0; arr && i < n && i < 512; i++) {
        u32 k = rd32(arr + 4 * i);
        if (k && !strncmp(char_name(k), "btnClickBlocker", 15) && stage_box(k, &x0, &y0, &x1, &y1)) {
            if (x0 >= STAGE_W - 1 || x1 <= 1 || y0 >= 767 || y1 <= 1)
                off = 1;
            else
                return 0;                       /* one on the stage: the popup is open */
        }
    }
    return off;
}

static void walk(u32 ch, int depth)
{
    if (!ch || depth > 24 || !*(u8 *)GP(ch + 0xab))
        return;
    if (rt_call1(vcall(ch, VT_MOUSE), ch)) {
        add_button(ch);                                 /* the outermost one takes the touch */
        return;
    }
    if (!rt_call2(vcall(ch, VT_IS), ch, 2) || closed_popup(ch))
        return;
    u32 arr = rd32(ch + 0xb8);
    int n = (int)rd32(ch + 0xbc);
    for (int i = 0; arr && i < n && i < 512; i++)
        walk(rd32(arr + 4 * i), depth + 1);
}

static u32 root_now, hit_fn;

/* does a touch at (x, y) reach button b (the game's own hit test finds it there)?  Anything on top
   counts: a popup's button or click blocker, the top bar over the shop's btnBackTop, and the shop's
   newBG, an unnamed clip over the middle of its pack rows (a tap there does nothing; a test tap at
   523,244 bought nothing) */
static int reaches(const Button *b, float x, float y, u32 *by)
{
    u32 h = rt_call3(hit_fn, root_now, f2u(x * TWIPS), f2u(y * TWIPS));
    *by = h;
    return h == b->ch;
}

/* a covered centre: which part of the button a grid of touches reaches, and the spot in it */
typedef struct {
    u32 ch, center_hit;
    float rx0, ry0, rx1, ry1;           /* the button's box when looked at */
    int ok;
    float x0, y0, x1, y1, sx, sy;       /* the reachable part and its spot */
    int age;                            /* scans since this answer was found */
} Grid;
static Grid grid[MAX_BUTTONS], grid_old[MAX_BUTTONS];
static int ngrid, ngrid_old;

static Grid *grid_search(const Button *b, u32 center_hit)
{
    static Grid g;
    memset(&g, 0, sizeof g);
    g.ch = b->ch, g.center_hit = center_hit;
    g.rx0 = b->x0, g.ry0 = b->y0, g.rx1 = b->x1, g.ry1 = b->y1;
    int cols = (int)((b->x1 - b->x0) / 40), rows = (int)((b->y1 - b->y0) / 40);
    cols = cols < 3 ? 3 : cols > 16 ? 16 : cols, rows = rows < 3 ? 3 : rows > 8 ? 8 : rows;
    float cw = (b->x1 - b->x0) / cols, rh = (b->y1 - b->y0) / rows;
    static float hx[16 * 8], hy[16 * 8];
    int nh = 0;
    float nx0 = 1e9f, ny0 = 1e9f, nx1 = -1e9f, ny1 = -1e9f;
    for (int r = 0; r < rows; r++)
        for (int k = 0; k < cols; k++) {
            float x = b->x0 + (k + 0.5f) * cw, y = b->y0 + (r + 0.5f) * rh;
            u32 h;
            if (reaches(b, x, y, &h)) {
                hx[nh] = x, hy[nh++] = y;
                nx0 = fminf(nx0, x - cw / 2), nx1 = fmaxf(nx1, x + cw / 2);
                ny0 = fminf(ny0, y - rh / 2), ny1 = fmaxf(ny1, y + rh / 2);
            }
        }
    if (!nh)
        return &g;
    g.ok = 1;
    g.x0 = nx0, g.y0 = ny0, g.x1 = nx1, g.y1 = ny1;
    float mx = (nx0 + nx1) / 2, my = (ny0 + ny1) / 2, best = 1e18f;
    for (int j = 0; j < nh; j++) {                      /* the reachable sample nearest that part's middle */
        float d = (hx[j] - mx) * (hx[j] - mx) + (hy[j] - my) * (hy[j] - my);
        if (d < best)
            best = d, g.sx = hx[j], g.sy = hy[j];
    }
    return &g;
}

/* the last reachable part found for each button, kept while its box stays (see scan) */
static Grid good[MAX_BUTTONS];
static int ngood, good_next;

static Grid *good_of(const Button *b)
{
    for (int j = 0; j < ngood; j++)
        if (good[j].ch == b->ch && fabsf(good[j].rx0 - b->x0) < 0.5f && fabsf(good[j].ry0 - b->y0) < 0.5f &&
            fabsf(good[j].rx1 - b->x1) < 0.5f && fabsf(good[j].ry1 - b->y1) < 0.5f)
            return &good[j];
    return NULL;
}

static void keep_good(const Grid *e)
{
    int j = 0;
    while (j < ngood && good[j].ch != e->ch)
        j++;
    if (j == ngood) {
        if (ngood < MAX_BUTTONS)
            ngood++;
        else
            j = good_next++ % MAX_BUTTONS;
    }
    good[j] = *e;
}

/* EventsMenu's carousel (a wide btnDrag area): the middle card is the largest button whose spot is in
   the area and whose box spans the area's middle (btnChange, or btnLockedOverlay on a locked event).
   Left / right on it slide the carousel; the other buttons of the area keep their place on the
   screen: the middle card's Restrictions bar, an Unlock button under it.  The side cards' buttons
   (btnfilemask, a sliver of a side card's btnChange) are no choice: they come by a slide.  Before (port
   fault), left / right anywhere in the area slid it, so from Restrictions
   Garage and Enter Event (the Red Bull event's Start) were out of reach. */
static int carousel = -1, card = -1;    /* the wide drag area and its middle card, or -1 */
static float card_x0, card_x1, card_y0, card_y1, card_mx;   /* the middle card's box, the area's middle */

static int in_area(int d, float x, float y)
{
    return d >= 0 && x >= drag[d].x0 && x <= drag[d].x1 && y >= drag[d].y0 && y <= drag[d].y1;
}

static int reaches(const Button *b, float x, float y, u32 *by);

static void find_card(const char *top)
{
    (void)top;
    carousel = card = -1;
    /* a drag area with cards (*File_N) beside it: EventsMenu, SubEventsMenu (EventFile_N) and
       AftermarketMenu (MarketFile_N).  Not by a drag area alone, since TrackSelectionMenu
       (a btnDrag over the track, -2,132 .. 1022,593) and BrandSelectionMenu (over the brand grid) have
       one too, and there the carousel rule would make their other buttons side cards: the track's next arrow
       and four of the six brands out of reach (port fault: only the 2 middle brands could be picked,
       and no track in Practice or Simple Race).  Those have no cards beside their drag area, so their
       buttons stay plain choices.
       By the cards, not by the menu's name, so that AftermarketMenu (the car upgrade screen) is in the rule: its
       cards have no button (the drag area, too big to be a choice, takes their touch), so the pad had
       only the top bar and Select there */
    for (int d = 0; d < ndrag && carousel < 0; d++) {
        if (GT_DEV && dbg > 0)
            rt_log("[padmenu]   drag area %s %4.0f,%4.0f .. %4.0f,%4.0f, %d cards\n", char_name(drag[d].ch), drag[d].x0, drag[d].y0,
                   drag[d].x1, drag[d].y1, drag[d].files);
        if (drag[d].files > 0 && drag[d].x1 - drag[d].x0 > drag[d].y1 - drag[d].y0)
            carousel = d;
    }
    if (carousel < 0)
        return;
    float mx = (fmaxf(drag[carousel].x0, 0) + fminf(drag[carousel].x1, STAGE_W)) / 2, best = 0;
    for (int i = 0; i < nbtn; i++) {
        Button *b = &btn[i];
        float area = (b->x1 - b->x0) * (b->y1 - b->y0);
        if (in_area(carousel, b->sx, b->sy) && b->x0 <= mx && b->x1 >= mx && area > best)
            best = area, card = i;
    }
    /* no button on the middle card (AftermarketMenu: a tap on it does nothing, Select opens its part):
       the card itself is the choice, where a touch reaches the drag area; Cross on it presses Select */
    Area *a = &drag[carousel];
    u32 by = 0;
    if (card < 0 && a->mx1 > a->mx0 && nbtn < MAX_BUTTONS) {
        Button *b = &btn[nbtn];
        memset(b, 0, sizeof *b);
        b->ch = a->ch, b->card = 1;
        b->x0 = fmaxf(a->mx0, a->x0), b->y0 = fmaxf(a->my0, a->y0), b->x1 = fminf(a->mx1, a->x1), b->y1 = fminf(a->my1, a->y1);
        b->sx = (b->x0 + b->x1) / 2, b->sy = (b->y0 + b->y1) / 2;
        snprintf(b->name, sizeof b->name, "card (%s)", char_name(a->ch));
        int ok = b->x1 > b->x0 && b->y1 > b->y0 && reaches(b, b->sx, b->sy, &by);
        if (GT_DEV && dbg > 0)
            rt_log("[padmenu]   carousel %s: %d cards, middle %4.0f,%4.0f .. %4.0f,%4.0f, %s%s\n", char_name(a->ch), a->files,
                   b->x0, b->y0, b->x1, b->y1, ok ? "the card is a choice" : "covered by ", ok ? "" : by ? char_name(by) : "?");
        if (ok)
            card = nbtn++;
    }
    card_x0 = card >= 0 ? btn[card].x0 : mx - 290;     /* a card is ~570 points wide */
    card_x1 = card >= 0 ? btn[card].x1 : mx + 290;
    card_y0 = card >= 0 ? btn[card].y0 : drag[carousel].y0;
    card_y1 = card >= 0 ? btn[card].y1 : drag[carousel].y1;
    card_mx = mx;
}

/* a button on the middle card (spans the area's middle: on a locked event both btnLockedOverlay and
   btnChange do) */
static int on_card(const Button *b)
{
    return in_area(carousel, b->sx, b->sy) && b->x0 <= card_mx && b->x1 >= card_mx && b->sy >= card_y0 && b->sy <= card_y1;
}

/* a button in the middle card's body (its Restrictions bar; not an Unlock button under it) */
static int in_card(const Button *b)
{
    return in_area(carousel, b->sx, b->sy) && b->sy >= card_y0 && b->sy <= card_y1;
}

/* a side card's button (reached by a slide, not by a jump) */
static int side_card(const Button *b)
{
    return in_area(carousel, b->sx, b->sy) && (b->sx < card_x0 || b->sx > card_x1);
}

/* REPLAY_HINT_PAD (port addition): the replay's two touch hints as pointer stops with Touch Screen On.  ReplayMenu's
   top icons are btnCamera (a button: the pointer already reaches it) and an unnamed clip (sprite 949, depth 7):
   one picture with the pinch-zoom hint on its left half and the swipe-turn hint on its right (267,17 .. 475,84 on
   the stage), no button, since on the iPad they only tell of the two-finger gestures.  Here each half is a pointer
   stop; Cross on it does the gesture (replay.c replay_hint): the pinch zooms in a step at a time and back out to
   the normal view after the closest one, the swipe turns the camera by 90 degrees.  With Touch Screen Off the clip
   is hidden (REPLAY_TOUCH_ICONS) and the pad has its own replay buttons (REPLAY_PAD).  GT_REPLAY_HINT_PAD=0: no
   stops. */
static void replay_hints(const char *top)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT_REPLAY_HINT_PAD");
        on = !(e && *e == '0');
    }
    if (!on || !top || strcmp(top, "ReplayMenu") || pcctl_keys_mode() || nbtn + 2 > MAX_BUTTONS)
        return;
    const char *t;
    u32 pl = player_now(&t), gp = pl ? rd32(pl + 0x3c) : 0, root = gp ? rt_call1(ROOT_FN, gp) : 0;
    u32 arr = root ? rd32(root + 0xb8) : 0, rm = 0;
    int n = root ? (int)rd32(root + 0xbc) : 0;
    for (int i = 0; arr && i < n && i < 128 && !rm; i++) {
        u32 k = rd32(arr + 4 * i);
        if (k && !strcmp(char_name(k), "ReplayMenu"))
            rm = k;
    }
    arr = rm ? rd32(rm + 0xb8) : 0;
    n = rm ? (int)rd32(rm + 0xbc) : 0;
    for (int i = 0; arr && i < n && i < 128; i++) {
        u32 k = rd32(arr + 4 * i);
        float x0, y0, x1, y1;
        if (!k || !*(u8 *)GP(k + 0xab) || *char_name(k) || !rt_call2(vcall(k, VT_IS), k, 2) ||
            !stage_box(k, &x0, &y0, &x1, &y1) || y1 > 160 || x1 - x0 < 100)
            continue;                   /* the hint clip: unnamed, at the top, two icons wide */
        float mid = (x0 + x1) / 2, shift = wide_draw_shift(k);
        for (int h = 1; h <= 2; h++) {
            Button *b = &btn[nbtn++];
            memset(b, 0, sizeof *b);
            b->ch = k, b->hint = h;
            b->x0 = (h == 1 ? x0 : mid) + shift, b->x1 = (h == 1 ? mid : x1) + shift, b->y0 = y0, b->y1 = y1;
            b->sx = (b->x0 + b->x1) / 2, b->sy = (y0 + y1) / 2;
            snprintf(b->name, sizeof b->name, h == 1 ? "hint: pinch (zoom)" : "hint: swipe (turn)");
            if (GT_DEV && dbg > 0)
                rt_log("[padmenu]   %-24s %4.0f,%4.0f .. %4.0f,%4.0f\n", b->name, b->x0, b->y0, b->x1, b->y1);
        }
        return;
    }
}

/* the buttons a touch can reach now, each with the spot the pointer goes to: its centre, or when that
   is covered the reachable part of a grid over it (its box shrinks to that part) */
static int scan(void)
{
    nbtn = 0;
    ndrag = 0;
    const char *top;
    u32 pl = player_now(&top), gp = pl ? rd32(pl + 0x3c) : 0, root = gp ? rt_call1(ROOT_FN, gp) : 0;
    if (!root)
        return 0;
    if (GT_DEV && dbg > 0 && dev_getenv("GT_PADMENU_TREE") && (int)eagl_frames() >= atoi(dev_getenv("GT_PADMENU_TREE")))
        dump_tree(root, 0);                     /* GT_PADMENU_TREE=frame: from that frame on */
    walk(root, 0);
    root_now = root, hit_fn = vcall(root, VT_HIT);
    memcpy(grid_old, grid, sizeof(Grid) * ngrid);
    ngrid_old = ngrid, ngrid = 0;
    int n = 0, retried = 0;
    for (int i = 0; i < nbtn; i++) {
        Button *b = &btn[i];
        float cx = (b->x0 + b->x1) / 2, cy = b->wheel ? b->wy : (b->y0 + b->y1) / 2;
        u32 by;
        int ok = reaches(b, cx, cy, &by);
        if (!ok) {
            /* the grid costs a hit test a cell (the shop: ~20 ms in all), so its answer is kept while the
               button's box and what covers its centre stay the same (and its spot still reaches it) */
            Grid *e = NULL;
            for (int j = 0; j < ngrid_old; j++)
                if (grid_old[j].ch == b->ch && grid_old[j].center_hit == by && fabsf(grid_old[j].rx0 - b->x0) < 0.5f &&
                    fabsf(grid_old[j].ry0 - b->y0) < 0.5f && fabsf(grid_old[j].rx1 - b->x1) < 0.5f &&
                    fabsf(grid_old[j].ry1 - b->y1) < 0.5f) {
                    e = &grid_old[j];
                    break;
                }
            /* a "none reachable" answer was kept while nothing changed, but it may be taken while something
               passes over the button: the shop's "NOT ENOUGH COINS" popup slides out after its X (and the
               shop covers the whole screen for ~0.1 s after it), with newBG (unnamed) over the rows' middle,
               so the price buttons stayed out of the pad's reach until the menu was left and
               entered again (port fault: after OK, the cash price list did not take the pad pointer
               again right away).  So the
               spot last found for that button (same box) is tried each look, one hit
               test, and a "none" answer is searched again after 4 looks (one button a look, ~3 ms) */
            u32 h2;
            Grid *k = good_of(b);
            if ((!e || !e->ok) && k && reaches(b, k->sx, k->sy, &h2)) {
                k->center_hit = by, k->age = 0;
                e = k;
            } else if (!e || (e->ok && !reaches(b, e->sx, e->sy, &h2)) || (!e->ok && e->age >= 4 && !retried++)) {
                e = grid_search(b, by);
                if (e->ok)
                    keep_good(e);
            } else
                e->age++;
            if (ngrid < MAX_BUTTONS)
                grid[ngrid++] = *e;
            ok = e->ok;
            if (ok) {
                b->x0 = e->x0, b->y0 = e->y0, b->x1 = e->x1, b->y1 = e->y1;
                cx = e->sx, cy = e->sy;
            }
        }
        if (GT_DEV && dbg > 0)
            rt_log("[padmenu]   %-24s %4.0f,%4.0f .. %4.0f,%4.0f %s%s\n", b->name, b->x0, b->y0, b->x1, b->y1,
                   ok ? "" : "covered by ", ok ? "" : by ? char_name(by) : "?");
        if (!ok)
            continue;
        b->sx = cx, b->sy = cy;
        /* WIDE_UI: a button drawn at a wide screen's edge (the main menu's left bar, widescreen.c): the
           pointer goes where it is drawn; the tap there is handed to the game at its own place (pcinput.c) */
        float shift = wide_draw_shift(b->ch);
        b->x0 += shift, b->x1 += shift, b->sx += shift;
        btn[n++] = *b;
    }
    nbtn = n;
    /* the Paddocks: under the Free / Silver / Gold signs (btnSelection01..03, their picture over the whole
       stage) the truck yard's buttons stay in the hit test, and two (btnTruck04 / 05) are reached in the
       gap between Silver's and Gold's hitzones (646 .. 657): the pointer went there, between the signs
       (port fault, seen at 21:9).  While a sign is on the stage the trucks are no
       choice. */
    int signs = 0;
    for (int i = 0; i < nbtn; i++)
        signs |= !strncmp(btn[i].name, "btnSelection", 12);
    if (signs) {
        n = 0;
        for (int i = 0; i < nbtn; i++)
            if (strncmp(btn[i].name, "btnTruck", 8))
                btn[n++] = btn[i];
        nbtn = n;
    }
    replay_hints(top);
    find_card(top);
    return nbtn;
}

/* navigation */

static u32 focus;                       /* the button the pointer sits on (pad mode), or 0 */
static float focus_x, focus_y;          /* where the pointer was put */
static int pad_mode;                    /* the pad moved the pointer last (the mouse has not since) */
static int focus_card;                  /* the pointer was put on the carousel's middle card */
static int focus_hint;                  /* ... on the replay's pinch (1) or swipe (2) hint: both are one clip */

static int find(u32 ch)
{
    int first = -1;
    for (int i = 0; i < nbtn; i++)
        if (btn[i].ch == ch) {
            if (btn[i].hint == focus_hint)
                return i;
            if (first < 0)
                first = i;
        }
    return first;
}

static int nearest(float x, float y)
{
    int best = -1;
    float bd = 1e18f;
    for (int i = 0; i < nbtn; i++) {
        if (side_card(&btn[i]))
            continue;
        float dx = fmaxf(fmaxf(btn[i].x0 - x, x - btn[i].x1), 0), dy = fmaxf(fmaxf(btn[i].y0 - y, y - btn[i].y1), 0);
        float cx = btn[i].sx - x, cy = btn[i].sy - y;
        float d = (dx * dx + dy * dy) * 4 + cx * cx + cy * cy;   /* inside a button beats near its centre */
        if (d < bd)
            bd = d, best = i;
    }
    return best;
}

/* the next button from button f in direction (ux, uy): ahead of it, close along the way, and lined up */
static int step(int f, int ux, int uy, int wide)
{
    Button *a = &btn[f];
    float ax = a->sx, ay = a->sy;
    int best = -1;
    float bs = 1e18f;
    for (int i = 0; i < nbtn; i++) {
        if (i == f)
            continue;
        Button *b = &btn[i];
        float bx = b->sx, by = b->sy;
        /* the carousel: its side cards come by a slide; sideways from outside it (Enter Event, Garage)
           the move goes past it, to the button across (it is entered by up / down) */
        if (side_card(b) || (ux && in_card(b) && !in_area(carousel, ax, ay)))
            continue;
        /* a drag area around the pointer is no step from a button in it: Top 10's list (NavigationMenu's
           btnDrag, 847,73 .. 1018,767, over the Buy column and the scroll arrows) took down from a row's
           Buy (no edge to cross, the score 8 against the next row's 14), the pointer went between rows 4
           and 5 and from there to the arrow (port fault: the pointer, moved down, stuck between rows 4
           and 5) */
        if (!b->card && !strncmp(b->name, "btnDrag", 7) && ax >= b->x0 && ax <= b->x1 && ay >= b->y0 && ay <= b->y1)
            continue;
        float along = ux ? (bx - ax) * ux : (by - ay) * uy;
        /* ahead: its spot past this button's far edge, or its near edge past this one's spot (so down
           from the cash + does not go to the level bar beside it, 6 px lower) */
        float a_far = ux > 0 ? a->x1 : ux < 0 ? -a->x0 : uy > 0 ? a->y1 : -a->y0;
        float b_near = ux > 0 ? b->x0 : ux < 0 ? -b->x1 : uy > 0 ? b->y0 : -b->y1;
        float b_spot = ux ? bx * ux : by * uy, a_spot = ux ? ax * ux : ay * uy;
        if (along < 4 || (b_spot <= a_far && b_near <= a_spot))
            continue;
        /* the gap between the two buttons across the move (0 when they overlap there) */
        float gap = ux ? fmaxf(fmaxf(b->y0 - a->y1, a->y0 - b->y1), 0) : fmaxf(fmaxf(b->x0 - a->x1, a->x0 - b->x1), 0);
        float off = ux ? fabsf(by - ay) : fabsf(bx - ax);
        if (gap > 0 && wide < 2 && off > along * (wide ? 2.0f : 0.7f))
            continue;                   /* not lined up and too far aside (strict: under 35 degrees; wide, when the strict finds none: 63;
                                           at last anything ahead: the Red Bull card's Restrictions -> down -> Enter Event) */
        /* the score, as a browser's spatial navigation: the way from this edge to its edge, plus twice
           how far it lies aside from the pointer's line (from the spot to its box), plus a little of the
           centres' offset to part ties.  So down from Play goes to the right card under it (not the small
           Next arrow), and left from NavigationMenu's tall btnDrag to the wheel (not the cash bar) */
        float edge = fmaxf(b_near - a_far, 0);
        float aside = ux ? fmaxf(fmaxf(b->y0 - ay, ay - b->y1), 0) : fmaxf(fmaxf(b->x0 - ax, ax - b->x1), 0);
        float s = edge + aside * 2 + off * 0.1f;
        if (s < bs)
            bs = s, best = i;
    }
    return best;
}

static void put(int i, const char *why)
{
    if (i < 0)
        return;
    Button *b = &btn[i];
    focus = b->ch;
    focus_hint = b->hint;
    focus_x = b->sx, focus_y = b->sy;
    focus_card = i == card || on_card(b);
    pad_mode = 1;
    ui_pointer_to(focus_x, focus_y);
    rt_log("[padmenu] %s: %s (%.0f, %.0f)\n", why, b->name, focus_x, focus_y);
}

static int option_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT_PAD_MENU");
        on = !(e && *e == '0');
        dbg = dev_getenv("GT_PADMENU_LOG") && *dev_getenv("GT_PADMENU_LOG") == '1';
        rt_log("[padmenu] pad menu pointer %s\n", on ? "on (D-pad / stick jump between buttons)" : "off");
    }
    return on;
}

/* the top menu's name on the screen now, or NULL (maxmoney.c: the cheat code only in the main menu) */
const char *padmenu_top(void)
{
    const char *top;
    return player_now(&top) ? top : NULL;
}

/* the menus take the pad now: when a menu is on the screen (the in-race ones are moved off while the
   race is driven, menu_shown; after the race also the replay's ReplayMenu and the event's
   "Congratulations!" in EndMenu, which run with the player's flag bit clear, see replay.c), not while a
   race waits for its start */
int padmenu_active(void)
{
    if (!option_on())
        return 0;
    const char *top;
    u32 pl = player_now(&top);
    int racing = steer_racing(), waiting = steer_race_waiting();
    /* in a race the in-race menus take it while they are on the screen (the pause menu, its Options,
       the end menus); not at the start's "Touch to continue", where Cross gives that touch */
    int on = pl && top && !waiting;
    (void)racing;
    static int was = -1;
    static char was_st[160];
    u32 arr = pl ? rd32(pl + 0x114) : 0;
    int nst = pl ? (int)rd32(pl + 0x118) : 0;
    char st[160] = "";
    for (int i = 0; arr && i < nst && i < 8; i++) {
        u32 e = rd32(arr + 4 * i);
        if (e)
            snprintf(st + strlen(st), sizeof st - strlen(st), " %s:%d", (const char *)GP(e + 8), (int)rd32(e + 0x58));
    }
    if (on != was || strcmp(st, was_st)) {
        u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
        rt_log("[padmenu] %s at frame %u: race %s state %d, racing %d waiting %d, flags %x / %x / %x, menus%s\n",
               on ? "menus take the pad" : "pad off the menus", eagl_frames(), race ? "on" : "off",
               race ? (int)rd32(race + 0x1d84) : -1, racing, waiting,
               g && rd32(g + G_MENUS) ? rd32(rd32(g + G_MENUS) + 0xf8) : 0,
               g && rd32(g + G_RACE_MENUS) ? rd32(rd32(g + G_RACE_MENUS) + 0xf8) : 0,
               g && rd32(g + G_PRIZE_MENUS) ? rd32(rd32(g + G_PRIZE_MENUS) + 0xf8) : 0, st);
        was = on;
        snprintf(was_st, sizeof was_st, "%s", st);
    }
    return on;
}

/* the button a popup (or a slide) took the pointer from: it goes back there once that button can be
   touched again, as a console menu returns to the item that opened a popup; a press of the pad's
   direction (a choice of the player's) forgets it */
static u32 return_to;
static int slid;                        /* a carousel was slid: land on what comes to the pointer */

void padmenu_mouse_moved(void)
{
    pad_mode = 0;
    focus = 0;
    return_to = 0;
}

/* testing: GT_POKE=frame:addr:value;... writes a guest word on that frame; with GT_PADMENU_LOG=1 every
   MenuFX player in the game object (vtable 0x78dfc8) is logged with its flags and menus when they change */
static void test_probe(void)
{
    static const char *poke;
    if (!poke)
        poke = dev_getenv("GT_POKE") ? dev_getenv("GT_POKE") : "";
    int now = (int)eagl_frames(), f;
    unsigned a, v;
    while (*poke && sscanf(poke, "%d:%x:%x", &f, &a, &v) == 3 && now >= f) {
        dev_log("[test] poke %08x = %x (was %x)\n", a, v, rd32(a));
        wr32(a, v);
        const char *n = strchr(poke, ';');
        poke = n ? n + 1 : "";
    }
    if (!GT_DEV || dbg <= 0)
        return;
    u32 g = gt_game();
    static char was[512];
    char s[512] = "";
    /* every pointer is checked: while a race loads, a player's menu list can name freed memory (a test with
       GT_PADMENU_LOG faulted here reading a menu at 0x51957308; the player never runs this) */
    #define MAPPED(p, n) ((p) >= 0x10000 && !((p) & 3) && VirtualQuery(GP(p), &mb, sizeof mb) && mb.State == MEM_COMMIT \
                          && !(mb.Protect & (PAGE_NOACCESS | PAGE_GUARD)) \
                          && (uintptr_t)GP(p) + (n) <= (uintptr_t)mb.BaseAddress + mb.RegionSize)
    MEMORY_BASIC_INFORMATION mb;
    for (u32 o = 0; g && o < 0x8000; o += 4) {
        if ((!o || !((g + o) & 0xfff)) && !MAPPED(g + o, 4))
            break;                          /* the scan ran off the object's pages (it faulted once) */
        u32 pl = rd32(g + o);
        if (!MAPPED(pl, 0x11c) || rd32(pl) != 0x78dfc8u)
            continue;
        snprintf(s + strlen(s), sizeof s - strlen(s), " +%x[%x]", o, rd32(pl + 0xf8));
        u32 arr = rd32(pl + 0x114);
        for (int i = 0, n = (int)rd32(pl + 0x118); arr && i < n && i < 6 && MAPPED(arr + 4 * i, 4); i++) {
            u32 e = rd32(arr + 4 * i);
            if (e && MAPPED(e, 0x5c))
                snprintf(s + strlen(s), sizeof s - strlen(s), " %.40s:%d", (const char *)GP(e + 8), (int)rd32(e + 0x58));
        }
    }
    #undef MAPPED
    if (strcmp(s, was)) {
        dev_log("[test] players at frame %d:%s\n", now, s);
        snprintf(was, sizeof was, "%s", s);
    }
}

/* the look padmenu_frame makes with nothing pressed (padmenu_frame: every 8 frames) */
static unsigned look_tick, look_sig;
static int look_same;

/* while the pad holds the pointer, every 8 frames: once the buttons have stopped changing (three
   looks alike: a menu or popup has slid in), the pointer follows its button if that moved, goes
   back to the button a popup took it from, or lands on the nearest one if its button is gone or
   covered (a new menu, a popup over it) */
static void follow_look(int pad_present)
{
    if (!pad_mode || !pad_present || ++look_tick % 8)
        return;
    int quiet = dbg;
    dbg = 0;                                /* the per-button lines only for presses */
    Uint64 t0 = SDL_GetPerformanceCounter();
    int n = scan();
    dbg = quiet;
    if (GT_DEV && dbg > 0 && look_tick % 256 == 0)
        rt_log("[padmenu] look: %d buttons in %.2f ms\n", n,
               (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    unsigned sig = (unsigned)n * 2654435761u;
    for (int i = 0; i < n; i++)
        sig = sig * 31 + btn[i].ch + (unsigned)lrintf(btn[i].sx) * 7 + (unsigned)lrintf(btn[i].sy) * 13;
    look_same = sig == look_sig ? look_same + 1 : 0;
    look_sig = sig;
    if (look_same < 2 || n == 0)
        return;
    int f = find(focus), r = return_to && return_to != focus ? find(return_to) : -1;
    static u32 known[MAX_BUTTONS];          /* the buttons on the screen with the pointer's own */
    static int nknown, held;
    if (f >= 0) {
        for (nknown = 0; nknown < n; nknown++)
            known[nknown] = btn[nknown].ch;
        held = 0;
    }
    if (slid) {
        slid = 0;                           /* a carousel turned: the card now in the middle */
        put(card >= 0 ? card : nearest(focus_x, focus_y), card >= 0 ? "after the slide, the card" : "after the slide, nearest");
    } else if (focus_card && card >= 0 && f != card && (f < 0 || !on_card(&btn[f]))) {
        /* the card under the pointer changed after it settled (a locked event's overlay fades in
           over its btnChange, leaving a sliver of it): on to the card */
        put(card, "the card changed");
    } else if (r >= 0) {
        return_to = 0;
        put(r, "back to");
    } else if (f < 0) {
        /* gone with no new button on the screen (only ones that were there with it): something passes
           over it, so the pointer waits up to 12 looks (~1.6 s).  After the shop's "NOT ENOUGH COINS"
           popup closed, the screen was covered for ~0.5 s with the top bar coming back first, and the
           pointer went from 30 cash up to the coins bar and back down (found while testing the
           fix above).  A popup or a new menu brings new buttons: the pointer goes at once */
        int fresh = 0;
        for (int i = 0; i < n && !fresh; i++) {
            int j = 0;
            while (j < nknown && known[j] != btn[i].ch)
                j++;
            fresh = j == nknown;
        }
        if (!fresh && ++held <= 12)
            return;
        if (!return_to)
            return_to = focus;
        put(nearest(focus_x, focus_y), "button gone, nearest");
    } else if (fabsf(btn[f].sx - focus_x) > 3 || fabsf(btn[f].sy - focus_y) > 3) {
        focus_x = btn[f].sx, focus_y = btn[f].sy;
        ui_pointer_to(focus_x, focus_y);    /* it slid: follow it, quietly */
    }
}

/* on a wheel, up / down turn it with a swipe (1: done, or waiting for the last one) */
static int wheel_turn(int f, int uy, int now)
{
    /* down = the finger up, to the next row; when the last swipe that way did not turn it (its end), the
       press leaves it, to the button past it.
       A turn takes a while: presses in the first 30 frames after a swipe wait for it */
    static u32 wheel_ch;
    static int wheel_dir, wheel_at;
    static float wheel_before;
    if (btn[f].wheel && uy) {
        float sig = wheel_sig(btn[f].ch, 0);
        if (wheel_ch == btn[f].ch && now - wheel_at < 30)
            return 1;
        if (!(wheel_ch == btn[f].ch && wheel_dir == uy && fabsf(sig - wheel_before) < 0.5f)) {
            wheel_ch = btn[f].ch, wheel_dir = uy, wheel_before = sig, wheel_at = now;
            float x = btn[f].sx, y = btn[f].sy;
            rt_log("[padmenu] turn %s %s\n", btn[f].name, uy > 0 ? "down" : "up");
            ui_pad_swipe(x, y + 60 * uy, x, y - 60 * uy, 12);
            return 1;
        }
    }
    wheel_ch = 0;
    return 0;
}

/* a list scrolled by arrows (Top 10's rows: btnTopTenBuy x 7 of 10 cars, btnPrev / btnNext beside
   them, a tap moves it one row): up / down past its first / last row shown taps the arrow that way
   and the pointer stays on its row, the next car coming under it, as a console's list scrolls
   (otherwise down goes to the arrow, and the cars past row 7 are out of the pad's reach).  A row: a button
   whose name three or more buttons share */
static int list_scroll(int f, int uy, int now)
{
    int same = 0, further = 0, arrow = -1;
    for (int i = 0; i < nbtn; i++) {
        if (strcmp(btn[i].name, btn[f].name))
            continue;
        same++;
        further |= i != f && fabsf(btn[i].sx - btn[f].sx) < 8 && (btn[i].sy - btn[f].sy) * uy > 4;
    }
    int back = -1;                      /* the other arrow: the list is at its end this way */
    for (int i = 0; i < nbtn && same >= 3 && !further; i++)
        if (!strcmp(btn[i].name, uy > 0 ? "btnNext" : "btnPrev"))
            arrow = i;
        else if (!strcmp(btn[i].name, uy > 0 ? "btnPrev" : "btnNext"))
            back = i;
    if (arrow < 0 && back >= 0) {
        /* its end (the tenth car on the last row): the pointer stays, as at a console list's end (not on to the
           Exit tile at the bottom left) */
        put(f, "end of the list, stays");
        return 1;
    }
    static int scroll_at = -1000;
    if (arrow >= 0) {
        if (now - scroll_at >= 12) {
            scroll_at = now;
            rt_log("[padmenu] scroll %s: %s (tap at %.0f, %.0f)\n", uy > 0 ? "down" : "up", btn[arrow].name,
                   btn[arrow].sx, btn[arrow].sy);
            ui_pad_tap(btn[arrow].sx, btn[arrow].sy);
        }
        return 1;
    }

    return 0;
}

/* in a wide drag area (EventsMenu's cards), left / right slide it with a swipe (right = the finger to the left,
   the next card in); the pointer then lands on what comes to its place.  1: slid (or the last slide still moves) */
static int carousel_slide(int f, int ux, int n, int now)
{
    static int slide_at = -1000;
    /* on the carousel's middle card (or in its area with nothing that way) left / right slide it */
    if (!(ux && in_area(carousel, btn[f].sx, btn[f].sy) && (f == card || on_card(&btn[f]) || n < 0)))
        return 0;
    if (now - slide_at < 30)
        return 1;                       /* the last slide is still moving */
    slide_at = now;
    Area *a = &drag[carousel];
    float cx = (fmaxf(a->x0, 0) + fminf(a->x1, STAGE_W)) / 2, w = 160, y = btn[f].sy;
    /* the swipe starts where a touch reaches the drag area itself: LicenseMenu's middle card has
       its Buy / Skip button (btnBuy1, 601,598) where the swipe began, at the pointer's height, so
       gameswf gave the touch to that button and the cards never turned (port fault: the licence
       cards did not turn with the pad; a test swipe
       by hand at y 330 turned them).  The pointer's height when the drag area is there (as on
       EventsMenu), else the first row over the middle card, from its
       middle out, where both ends of the swipe reach the drag area */
    u32 by;
    Button probe;
    memset(&probe, 0, sizeof probe);
    probe.ch = a->ch;
    if (!reaches(&probe, cx + w * ux, y, &by)) {
        /* over the middle card (its *File_N clip; else the whole area) */
        int file = a->mx1 > a->mx0 && a->my1 > a->my0;
        float y0 = fmaxf(file ? a->my0 : a->y0, a->y0), y1 = fminf(file ? a->my1 : a->y1, a->y1);
        float my = (y0 + y1) / 2, to = -1;
        for (float d = 0; d <= (y1 - y0) / 2 && to < 0; d += 20)
            for (int s = -1; s <= 1 && to < 0; s += 2) {
                float ty = my + d * s;
                if (reaches(&probe, cx + w * ux, ty, &by) && reaches(&probe, cx - w * ux, ty, &by))
                    to = ty;
            }
        if (to >= 0)
            y = to;
        else if (GT_DEV && dbg > 0)
            rt_log("[padmenu]   no row over the card reaches %s; the swipe stays at y %.0f\n", char_name(a->ch), y);
    }
    rt_log("[padmenu] slide %s (swipe at y %.0f)\n", ux > 0 ? "to the next" : "to the previous", y);
    ui_pad_swipe(cx + w * ux, y, cx - w * ux, y, 10);
    slid = 1;
    return 1;
}

/* up / down / left / right: the pointer to the next button that way (or a wheel, list or carousel moved) */
static void press_direction(int pressed, int f, float px, float py)
{
    if (f < 0) {
        /* the first press only takes the pointer over; so does one after its button went away */
        put(pad_mode ? nearest(focus_x, focus_y) : nearest(px, py), pad_mode ? "button gone, nearest" : "first press, nearest");
        return;
    }
    return_to = 0;
    int ux = pressed & 4 ? -1 : pressed & 8 ? 1 : 0, uy = ux ? 0 : pressed & 1 ? -1 : 1;
    int now = (int)eagl_frames();
    if (wheel_turn(f, uy, now))
        return;
    if (uy && list_scroll(f, uy, now))
        return;
    int n = step(f, ux, uy, 0);
    if (n < 0)          /* nothing lined up that way: something aslant (EventsMenu: Restrictions -> Enter Event) */
        n = step(f, ux, uy, 1);
    if (n < 0 && uy)    /* still none, up / down: anything that way (sideways that jumped from Ready to the top bar) */
        n = step(f, ux, uy, 2);
    if (carousel_slide(f, ux, n, now))
        return;
    put(n >= 0 ? n : f, n >= 0 ? "move" : "edge, stays");
}

/* Cross: a tap on the pointer's button */
static void press_select(int f, float px, float py)
{
    if (f < 0 && !pad_mode) {
        /* the mouse left the pointer somewhere: a tap only when it is on a button, else Cross just
           takes the pointer to the nearest one (a test's first Cross had tapped a Final Settings slider) */
        int on = -1;
        for (int i = 0; i < nbtn && on < 0; i++)
            if (px >= btn[i].x0 && px <= btn[i].x1 && py >= btn[i].y0 && py <= btn[i].y1)
                on = i;
        if (on < 0) {
            put(nearest(px, py), "first press, nearest");
            return;
        }
        f = on;
    }
    if (f < 0)
        f = nearest(focus_x, focus_y);
    /* the tap goes where the pointer is when it sits on a button, so a mouse-placed pointer works too */
    Button *b = &btn[f];
    if (b->card)        /* a card with no button (the upgrade parts): its menu's Select opens it */
        for (int i = 0; i < nbtn; i++)
            if (!strcmp(btn[i].name, "btnSelect")) {
                rt_log("[padmenu] select: %s -> %s (tap at %.0f, %.0f)\n", b->name, btn[i].name, btn[i].sx, btn[i].sy);
                ui_pad_tap(btn[i].sx, btn[i].sy);
                return;
            }
    if (b->hint) {      /* the replay's pinch / swipe hint: its gesture (REPLAY_HINT_PAD) */
        rt_log("[padmenu] select: %s\n", b->name);
        replay_hint(b->hint);
        return;
    }
    int on_it = px >= b->x0 && px <= b->x1 && py >= b->y0 && py <= b->y1;
    float x = pad_mode || !on_it ? b->sx : px, y = pad_mode || !on_it ? b->sy : py;
    rt_log("[padmenu] select: %s (tap at %.0f, %.0f)\n", b->name, x, y);
    ui_pad_tap(x, y);
}

static void press_back(const char *top)
{
    /* Circle: the menu's back button (btnBack, the shop's btnBackTop ...; not btnBackstage), else a
       popup's close / cancel / no, or the pause menu's Resume, or the replay's eject (ReplayMenu's
       btnStop, back to the end menu), as a console's Circle */
    static const char *const order[] = {"btnBack", "btnClose", "btnCancel", "btnNo", "btnResume", "btnStop", NULL};
    for (int k = 0; order[k]; k++)
        for (int i = 0; i < nbtn; i++) {
            size_t l = strlen(order[k]);
            char next = btn[i].name[l];             /* btnBackTop yes, btnBackstage no */
            if (!strncmp(btn[i].name, order[k], l) && !(next >= 'a' && next <= 'z')) {
                Button *b = &btn[i];
                rt_log("[padmenu] back: %s (tap at %.0f, %.0f)\n", b->name, b->sx, b->sy);
                ui_pad_tap(b->sx, b->sy);
                return;
            }
        }
    rt_log("[padmenu] back: no back button on %s\n", top ? top : "-");
}

/* buttons: bit 0 up, 1 down, 2 left, 3 right, 4 select, 5 back (presses this frame) */
void padmenu_frame(int pressed, int pad_present)
{
    test_probe();
    if (!padmenu_active()) {
        focus = 0;
        return_to = 0;
        look_sig = 0;
        return;
    }
    const char *top;
    player_now(&top);
    if (!pressed) {
        follow_look(pad_present);
        return;
    }
    if (GT_DEV && dbg > 0)
        rt_log("[padmenu] scan (top menu %s)\n", top ? top : "-");
    if (scan() == 0) {
        rt_log("[padmenu] no button found (top menu %s)\n", top ? top : "-");
        return;
    }
    float px, py;
    ui_pointer_at(&px, &py);
    if ((dev_getenv("GT_PADNAV") || dev_getenv("GT_KEYNAV")) && !pad_mode)
        px = 512, py = 384;                 /* testing: the real mouse may be anywhere */
    int f = pad_mode ? find(focus) : -1;
    if (pressed & 15)
        press_direction(pressed, f, px, py);
    else if (pressed & 16)
        press_select(f, px, py);
    else if (pressed & 32)
        press_back(top);
}
