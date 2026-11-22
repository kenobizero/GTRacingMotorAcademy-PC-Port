/* widescreen.c -- WIDE_UI: on a wide screen (GT_WIDE) the menus and the race HUD keep their shape.

   A port addition: the iPad's screen is 4:3, and without it a wider GT_WIDE screen stretches the UI sideways.

   gameswf's begin_display (F_517608, render_handler_irrlicht) gets the viewport (the drawable, portrait
   pixels: 1536 x 2732 for a 1366-point-wide iPad 3 screen; it turns the picture itself) and the stage's
   frame in twips (0 .. 20480 x 0 .. 15360 = 1024 x 768) and maps one onto the other, so a wider screen
   stretched every menu and the HUD sideways.  Here the frame handed on is widened to the viewport's shape
   around the stage's middle: the stage keeps its scale and sits in the middle, and what the SWFs draw past
   its edges (the menus' backgrounds reach well past them) fills the sides.  The cull, is_visible
   (F_515980: a shape's box against 0 .. the frame's width), then takes the widened range, or the sides
   would lose whatever lies wholly left of the stage.  Touches: uikit.c (ui_stage_dx).  Both functions are
   reached through the dispatch table only (--wrap).  GT_WIDE_UI=0: the stretched picture.
   The parts below (checked at 1920 x 1080 on ten screens, and at 21:9):
   - the game's own drawing over SWF placeholders (GetBounds), black side bands under menus-only frames, plain
     covers drawn across, slivers and display callbacks off the stage, the cockpit picture enlarged;
   - title bars and backdrops across the screen, the track list's backdrop enlarged, carousel cards more than
     half off the stage hidden (cover_kind, card_off, backdrop_scale); the upgrade menu's 3D background is
     menu3d.c's (UPGRADE_3D);
   - the menus' 3D pictures placed as on the iPad (gt_wide_fit), pictures and framed bars kept in shape with their
     edges drawn into the sides (EXTEND, extend_sides);
   - the race HUD's parts at the screen's edges, as on the iPad (wide_hud). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "gl.h"
#include "rt.h"
#include "features.h"
#include "game.h"

void __real_F_517608(CPU *c);
void __real_F_515980(CPU *c);

static float ext_x0, ext_x1;              /* the frame's x range handed on (twips); 0, 0 = the original's */
static float stage_w;                     /* the stage's own width (twips) */
static int in_anchored;                   /* drawing a race HUD part moved to the screen's edge (wide_hud) */

/* render_handler_irrlicht::is_visible(rect): x min, x max, y min, y max against 0 .. the frame's size.
   On a wide screen still against the STAGE's width, not the widened frame's: MenuFX parks a closed menu
   or popup just off the stage (x 1024 .. 2048) rather than hiding it, and the sides would show it (the
   first build did: Options > Controls showed a black panel at the right edge).  A shape
   reaching onto the stage (the menus' backgrounds) is still drawn whole, into the sides. */
void __wrap_F_515980(CPU *c)
{
    if (ext_x0 == 0 && ext_x1 == 0) {
        __real_F_515980(c);
        return;
    }
    float r[4], h;
    memcpy(r, GP(c->r[1]), sizeof r);
    memcpy(&h, GP(c->r[0] + 0x34c), 4);
    if (in_anchored)                      /* a race HUD part at the screen's edge: the whole frame */
        c->r[0] = !(r[0] > ext_x1 || r[1] < ext_x0 || r[3] < 0 || r[2] > h);
    else
        c->r[0] = !(r[0] > stage_w || r[1] < 0 || r[3] < 0 || r[2] > h);
}

/* generic_character::display (F_4eb4ac: a shape, a bitmap) and edit_text_character::display (F_500b58),
   the display list's leaves (vtable +0x120; a sprite's own, 0x4f6450, only walks its children): on a wide
   screen a leaf wholly off the stage sideways is not drawn, so a menu, popup or HUD message parked off
   the stage does not show in the sides (seen: a black panel right of Options > Controls, a
   race message's "ed!" left of the HUD).  The renderer itself culls only text glyphs (is_visible above).
   A leaf's box: get_bound (vtable +0x12c, its parent's space) and the parent's world matrix (0x4ca618:
   a, b, tx, c, d, ty), as padmenu.c finds buttons. */
#define VT_BOUND 0x12cu

static float stage_h;                     /* the stage's height (twips) */

/* PASS_CACHE (optimisation pass 3: matrix and bounding box results cached for static elements): within one
   display pass (begin_display, __wrap_F_517608, to the next) gameswf
   only draws, nothing moves, so a parent's world matrix and a carousel card's "more than half off the stage" are
   the same for all its leaves.  They were asked again for each leaf: two calls into the game per leaf for the
   parent's matrix and its box, two more per leaf of a card.  Kept here per pass (a small table by character;
   a full table is simply asked again).  A leaf's own matrix (moved and put back by the draws below) is never kept.
   GT_WIDE_CACHE=0: asked every time. */
#define PC_SLOTS 512
typedef struct { u32 pass, ch; float m[6]; u8 has_m; s8 card; } PassEntry;
static PassEntry pc[PC_SLOTS];
static u32 pass_no = 1;
static int pc_on = -1;

static PassEntry *pass_entry(u32 ch)
{
    if (pc_on < 0) {
        const char *e = getenv("GT_WIDE_CACHE");
        pc_on = !(e && *e == '0');
    }
    if (!pc_on || !ch)
        return NULL;
    u32 h = (ch >> 2) * 0x9E3779B1u >> 23 & (PC_SLOTS - 1);
    for (int n = 0; n < 8; n++, h = (h + 1) & (PC_SLOTS - 1)) {
        PassEntry *e = &pc[h];
        if (e->pass != pass_no) {             /* free in this pass: take it */
            e->pass = pass_no, e->ch = ch, e->has_m = 0, e->card = -1;
            return e;
        }
        if (e->ch == ch)
            return e;
    }
    return NULL;
}

/* a parent's world matrix (a, b, tx, c, d, ty), identity when it has none */
static void parent_matrix(u32 parent, float m[6])
{
    PassEntry *e = pass_entry(parent);
    if (e && e->has_m) {
        memcpy(m, e->m, sizeof e->m);
        return;
    }
    static const float id[6] = {1, 0, 0, 0, 1, 0};
    u32 w = parent ? rt_call1(GT_FN_WORLD_MATRIX, parent) : 0;
    memcpy(m, w ? GP(w) : (const void *)id, 6 * sizeof(float));
    if (e) {
        memcpy(e->m, m, sizeof e->m);
        e->has_m = 1;
    }
}

/* a leaf's box on the stage (twips); 0 when it has none */
static int world_box(u32 ch, float b[4])
{
    static float rect[4];
    rect[0] = 1, rect[1] = 0;
    rt_call2(rd32(rd32(ch) + VT_BOUND), ch, GA(rect));
    if (!(rect[0] <= rect[1] && rect[2] <= rect[3]))
        return 0;
    float m[6];
    parent_matrix(rd32(ch + 0x40), m);
    b[0] = b[2] = 1e30f, b[1] = b[3] = -1e30f;
    for (int k = 0; k < 4; k++) {
        float u = rect[k & 1], v = rect[2 + (k >> 1)];
        float x = m[0] * u + m[1] * v + m[2], y = m[3] * u + m[4] * v + m[5];
        b[0] = x < b[0] ? x : b[0], b[1] = x > b[1] ? x : b[1];
        b[2] = y < b[2] ? y : b[2], b[3] = y > b[3] ? y : b[3];
    }
    return 1;
}

/* Off the stage sideways: wholly, or all but a sliver of a much wider leaf (a parked panel's drop shadow
   reaching a few pixels onto the stage drew its whole frame in the side: the race's end screen,
   a second panel's frame right of Rankings with its texts culled). */
static int off_stage(const float b[4])
{
    float on = (b[1] < stage_w ? b[1] : stage_w) - (b[0] > 0 ? b[0] : 0);
    if (on <= 0)
        return 1;
    return on < 0.03f * stage_w && on < 0.05f * (b[1] - b[0]);
}

/* A plain colour box that covers the stage, as the dimming behind the race's end and pause menus and the
   one-colour backgrounds are, is drawn across the whole screen: in the sides too, which were left
   undimmed.  Only a shape (shape_character_def, display 0x4f0d00 at its
   vtable + 0xc: 0x78d448 or 0x78da08) with one solid fill (fill styles [def + 0x24], count + 0x28; a
   style's type +4, 0 = solid) and no further than a little past the stage: a picture is not stretched,
   and a background already wider than the stage needs nothing.  Its cached world matrix (character +
   0x88, made current by 0x4ca618) is widened for the draw and put back. */
static const char *char_name(u32 ch);

/* Bars, backdrops and pictures across the screen (the car prize, the track list, the race's end and the upgrade
   menu at 1920 x 1080):
   - a bar across the stage's width (the menus' black title bar, 0 .. 1024 x 0 .. 80, and its 3-pixel line
     under it, a linear gradient) is drawn across the screen too: it ended at the stage's edges;
   - the menus' flat backdrop newBG (a picture of exactly the stage) is enlarged to the screen's width, a
     little off its top and bottom: it ended at the stage's edges, the sides black;
   - the Paddocks prize popup's picture (PopupCongrats: grey and white shading of exactly the stage) is drawn
     across: the container's walls showed at its sides, a messy picture.
   At 21:9 PopupCongrats is the Paddocks' truck picture
   too (Free / Silver / Gold), stretched flat, and the bottom panel's two fills are the
   bar and the Rewards frame, stretched with it (its word cut).  So a picture of the stage (not newBG), a
   bar of more than one plain fill and a bar of a picture keep their shape, their edges drawn on into the
   side bands (EXTEND, extend_sides below); the loading screen's picture (logo, panel and car in one) too:
   it was 4:3 with black sides.
   An event's prize screen (at 16:9, "Congratulations! /
   Classic Car Competition / You finished 1st / XP / Prize / Total / Next", still 4:3; port
   fault, the screen's shape): its SWF (XiAZXBFMiNHKXiiMlHM.FAK, player G + 0x1b38) has a background
   picture of the stage in two picture fills (root, ch3) and its yellow and black banners as one picture
   0 .. 1027 x 47 .. 551 (FirstEventsScreen; ThirdEventsScreen 47 .. 654, secondEventsScreen 46 .. 308 in
   three fills): the first is no one-fill picture, the second too tall for a bar, both kept in the 4:3
   middle.  So a picture of the stage in up to three picture fills, and a picture spanning the stage's
   width at any height, are EXTENDed.  Checked over every SWF: besides
   those, only the level-up screen's background (Screen_levelup.levelupScr, four SWFs) and the Paddocks'
   free truck animation (Lottery_animation.Free_lottery) take the new rule, both screens of the stage;
   the lobby's player list (36 fills) does not. */
enum { KEEP, STRETCH, ZOOM, EXTEND };

static int cover_kind(u32 ch, const float b[4])
{
    float tol = 0.02f * stage_w;
    if (b[0] > tol || b[1] < stage_w - tol)
        return KEEP;
    if (b[0] < -0.1f * stage_w || b[1] > 1.1f * stage_w)
        return KEEP;
    u32 def = rd32(ch + 0xb0), vt = def ? rd32(def) : 0;
    if (vt != 0x78d448u && vt != 0x78da08u)
        return KEEP;
    u32 fills = rd32(def + 0x24), n = rd32(def + 0x28), type = n && fills ? rd32(fills + 4) : 99;
    int picture = type >= 0x40 && type <= 0x43 && n <= 3;
    if (b[2] > tol || b[3] < stage_h - tol) {          /* a bar */
        if (!n)
            return KEEP;
        if (b[3] - b[2] > 0.25f * stage_h)
            return picture ? EXTEND : KEEP;             /* a picture across the stage (prize banners) */
        return n == 1 && (type == 0 || type == 0x10) ? STRETCH : EXTEND;
    }
    if (picture)                                        /* a picture of the stage */
        return n == 1 && !strcmp(char_name(rd32(ch + 0x40)), "newBG") ? ZOOM : EXTEND;
    if (n != 1)
        return KEEP;
    if (type == 0)                                      /* a plain colour over the stage */
        return STRETCH;
    return KEEP;
}

/* The carousels' cards (MarketFile_N in the upgrade menu, File1..3 in the track list, RankingsFile and the
   like at the race's end, EventFile_N): the cards next to the one shown sit beside it, reaching past the
   stage's edges, where the iPad's screen cut them; the sides showed them (Tires beside Engine, a second
   panel right of Rankings).  On a wide screen a card
   (the nearest parent named ..File.., not the ..Files container) more than half off the stage is not
   drawn, all its parts together.
   EVENT_SIDES: the event carousel's cards (EventFile_N, EventsMenu and SubEventsMenu) stay drawn beside the
   one shown, so the events before and after can be seen,
   cut by the screen's edges as the iPad's were by its own.
   GT_EVENT_SIDES=0 hides them.
   UPGRADE_SIDES: the same
   for AftermarketMenu's parts (MarketFile_N: Tires beside Engine ...).  GT_UPGRADE_SIDES=0 hides them. */
static int world_box(u32 ch, float b[4]);
static int event_sides = -1, upgrade_sides = -1;

static int card_off(u32 ch)
{
    u32 p = rd32(ch + 0x40);
    for (int d = 0; p && d < 8; d++, p = rd32(p + 0x40)) {
        const char *n = char_name(p);
        size_t len = strlen(n);
        if (!strstr(n, "File") || n[len - 1] == 's')
            continue;
        if (event_sides < 0) {
            const char *e = getenv("GT_EVENT_SIDES");
            event_sides = !(e && *e == '0');
            e = getenv("GT_UPGRADE_SIDES");
            upgrade_sides = !(e && *e == '0');
        }
        if ((event_sides && !strncmp(n, "EventFile", 9)) || (upgrade_sides && !strncmp(n, "MarketFile", 10)))
            return 0;
        PassEntry *e = pass_entry(p);     /* the card's answer, once per display pass (PASS_CACHE) */
        if (e && e->card >= 0)
            return e->card;
        float b[4];
        int off = 0;
        if (world_box(p, b) && b[1] > b[0]) {
            float on = (b[1] < stage_w ? b[1] : stage_w) - (b[0] > 0 ? b[0] : 0);
            off = on < 0.5f * (b[1] - b[0]);
        }
        if (e)
            e->card = (s8)off;
        return off;
    }
    return 0;
}

/* The cockpit view's car interior (dashboard, pillars, wheel, hands) is no 3D model but a SWF picture: the
   car's interior movie, copied into the race HUD's clip "RenderInterior" (HUD.RenderInterior; Android
   HUD::Create), its needles drawn by the game over placeholders in it (HUD::RenderDash, found with
   GetBounds; the wheel is a 3D model, gt_wheel_proj below).  Drawn to the stage's scale it ends at about 0.87 of a 16:9
   screen's half width, the 3D world showing past it (seen with the Pagani; the Ferrari
   288 GTO's the same): the pictures are made for the iPhone's 3:2 (they end at 1.54 .. 1.57; the clip's
   box reaches further, with parts not seen).  On a screen wider than 3:2 the clip is enlarged around the
   stage's middle to span it (at most 1.5x; 16:9 1.19x): a little closer in, a little off its top and
   bottom; the 3D view behind keeps its angle. */
static const char *char_name(u32 ch)
{
    u8 tag = *(u8 *)GP(ch + 0x44);
    return tag == 0xff ? (const char *)GP(rd32(ch + 0x50)) : (const char *)GP(ch + 0x45);
}

static u32 interior_of(u32 ch)
{
    for (int depth = 0; ch && depth < 8; depth++, ch = rd32(ch + 0x40))
        if (!strcmp(char_name(ch), "RenderInterior"))
            return ch;
    return 0;
}

#define INTERIOR_ASPECT 1.5f              /* the pictures' width: the iPhone's 3:2 (seen: 1.54 .. 1.57) */

static float interior_scale(void)
{
    float s = (ext_x1 - ext_x0) / stage_h / INTERIOR_ASPECT;
    s = s < 1 ? 1 : s > 1.5f ? 1.5f : s;
    static float told;
    if (s != told) {
        told = s;
        rt_log("[wide] cockpit picture enlarged %gx\n", s);
    }
    return s;
}

/* Testing: GT_WIDE_DUMP=frame,frame.. logs every leaf, display callback and GetBounds of those frames (names up
   the parents, box in stage pixels, the shape's fills). */
static int dump_frames[16], ndump, frame_no, dumping;

static void dump(const char *what, u32 ch, const float b[4])
{
    char path[256] = "";
    u32 p = ch;
    for (int d = 0; p && d < 6; d++, p = rd32(p + 0x40)) {
        size_t n = strlen(path);
        snprintf(path + n, sizeof path - n, "%s%s", d ? "<" : "", char_name(p));
    }
    u32 def = rd32(ch + 0xb0), vt = def ? rd32(def) : 0, nf = 0, t = 99;
    if (vt == 0x78d448u || vt == 0x78da08u) {
        nf = rd32(def + 0x28);
        t = nf ? rd32(rd32(def + 0x24) + 4) : 99;
    }
    dev_log("[wdump] %u %s %08x %s box %.0f..%.0f x %.0f..%.0f vt %x fills %u type %u\n", frame_no, what, ch, path,
           b[0] / 20, b[1] / 20, b[2] / 20, b[3] / 20, vt, nf, t);
}

/* EXTEND: after the leaf's own draw, each side band gets the leaf again, mirrored about its edge and drawn
   out sideways (k times), so that the band shows only the edge's last two pixels (0.2 % of the stage:
   at 0.4 % the Paddocks' bottom panel brought its Rewards frame's white line into the right edge),
   in streaks: a bar's colour, a picture's edge.  Mirrored, the whole copy lies outside the leaf's own
   edge and never over the stage.  (A GL scissor did not hold: gameswf's draws go out later.) */
static void extend_sides(CPU *c, void (*real)(CPU *), u32 ch, const float b[4])
{
    float band = -ext_x0, edge = 0.002f * stage_w;
    if (band <= 0)
        return;
    u32 wm = rt_call1(GT_FN_WORLD_MATRIX, ch);
    float keep[6], m[6], k = (band + fabsf(b[0])) / edge;
    memcpy(keep, GP(wm), sizeof keep);
    for (int side = 0; side < 2; side++) {
        float a = side ? b[1] : b[0];                   /* x' = a - (x - a) k */
        memcpy(m, keep, sizeof m);
        m[0] *= -k, m[1] *= -k, m[2] = a - (m[2] - a) * k;
        memcpy(GP(wm), m, sizeof m);
        c->r[0] = ch;
        real(c);
    }
    memcpy(GP(wm), keep, sizeof keep);
}

/* The race HUD (race and replay at 21:9: the speedometer, position, lap, time and map follow the aspect
   ratio).  Drawn as the stage is, the race HUD stays in the centred 4:3 stage, the
   track map and the speedometer a third of the way in from the screen's edges.  On the iPad they sit at
   the screen's edges, so on a wide screen each HUD part on the stage's left (the map, the pause icon, the
   gear arrows) is drawn further left by the side band and each on its right (Pos, Lap, the speedometer,
   the lap times, the pedals) further right; what is in the middle (rear view, start lights, messages)
   stays.  Only the drawing moves (the leaf's world matrix for its draw, the map's GetBounds box): the game
   turns a touch into a stage point by x * 1024 / screen width, which never leaves 0 .. 1024, so a part
   really moved off the stage (by its local matrix) cannot be touched (Esc's pause tap at -336 does
   nothing).  A touch on a part where it is drawn is handed to the game at the part's own place
   instead (wide_hud_touch, uikit.c ui_touch_event).  The off-stage cull and the text glyph cull (is_visible
   above) let the moved parts be.  Called from the HUD's per-frame draw (hudhide.c).
   GT_WIDE_HUD=0: the HUD stays in the 4:3 middle. */

/* The parts drawn at an edge, in two groups (0 the race HUD, 1 the main menu's left bar), each taken anew
   on every frame its menu is updated; a group not updated for 2 frames is off. */
enum { G_HUD, G_NAV, NGROUPS };
#define MAX_PARTS 32
typedef struct { u32 ch; float shift; float box[4]; } Part;  /* shift, box (where it is drawn): twips */
static Part parts[NGROUPS][MAX_PARTS];
static int nparts[NGROUPS], group_frame[NGROUPS] = {-9, -9};
static int hud_on = -1;

static int edges_on(void)
{
    if (hud_on < 0) {
        const char *e = getenv("GT_WIDE_HUD");
        hud_on = ui_wide_ui && !(e && *e == '0');
    }
    return hud_on > 0 && (ext_x0 != 0 || ext_x1 != 0);
}

static void add_part(int g, u32 ch, float shift, const float b[4])
{
    if (nparts[g] >= MAX_PARTS || shift == 0)
        return;
    Part *p = &parts[g][nparts[g]++];
    p->ch = ch, p->shift = shift;
    memcpy(p->box, b, sizeof p->box);
    p->box[0] += shift, p->box[1] += shift;
}

/* the shift (twips) a leaf or clip of a moved part is drawn with, or 0 */
static float hud_shift(u32 ch)
{
    if (hud_on <= 0)
        return 0;
    for (int d = 0; ch && d < 10; d++, ch = rd32(ch + 0x40))
        for (int g = 0; g < NGROUPS; g++)
            if (group_frame[g] + 2 >= frame_no)
                for (int i = 0; i < nparts[g]; i++)
                    if (parts[g][i].ch == ch)
                        return parts[g][i].shift;
    return 0;
}

/* stage points: padmenu.c puts the pad's pointer where the button is drawn */
float wide_draw_shift(u32 ch)
{
    return ext_x0 != 0 || ext_x1 != 0 ? hud_shift(ch) / 20 : 0;
}

static const char *const hud_parts[] = {
    "HUD.MovingElements.btnRenderTrack", "HUD.MovingElements.btnTimeStats", "HUD.MovingElements.btnSpeedOMeter",
    "HUD.MovingElements.Laps", "HUD.MovingElements.btnPosition", "HUD.MovingElements.btnLap",
    "HUD.MovingElements.btnTopTimes", "HUD.MovingElements.Home", "HUD.MovingElements.OpenBigPlayer",
    "HUD.MovingElements.btnOpenSong", "HUD.Gears", "HUD.AccelBrake.btnAccel", "HUD.AccelBrake.btnBrake",
    "HUD.AccelBrake.btnBrakeLeft", "HUD.AccelBrake.btnBrakeRight", "HUD.btnBack", "HUD.btnReset", NULL};

void wide_hud(u32 pl)
{
    if (!edges_on())
        return;
    float band = -ext_x0;
    group_frame[G_HUD] = frame_no, nparts[G_HUD] = 0;
    pass_no++;                            /* the update, not a display pass: nothing kept across (PASS_CACHE) */
    u32 hud = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)"HUD"));   /* once; the parts under it (pass 4, swffind.c) */
    for (int i = 0; hud_parts[i]; i++) {
        u32 ch = swf_find_under(pl, hud, hud_parts[i] + 4);   /* past "HUD." */
        float b[4];
        if (!ch || !*(u8 *)GP(ch + 0xab) || !world_box(ch, b) || b[1] <= b[0])
            continue;
        float mid = (b[0] + b[1]) / 2;
        add_part(G_HUD, ch, mid < 0.4f * stage_w ? -band : mid > 0.6f * stage_w ? band : 0, b);
    }
    pass_no++;
    static int told;
    if (!told++)
        rt_log("[wide] race HUD: its left and right parts drawn at the screen's edges (%.0f px)\n", band / 20);
}

/* The main menu's left bar at 21:9 (the blue menu sticks to the screen's left edge): the main menu's blue bar
   (NavigationMenu.btnLeftMenu: RACE / PRACTICE /
   MY CARS / RED BULL and its arrows) and the buttons under it (btnInfo = LIVE, btnSettings, btnPCExit and
   their unnamed backing) are drawn at the screen's left edge, the cards and the top bar staying in the
   middle: each child of NavigationMenu lying in the stage's left part (within its first 40 %) where the
   menu rests.  MenuFX slides a menu off the stage and back (NavigationMenu's own x): the shift shrinks as
   the menu moves away (none once it is a side band's width off), so the bar leaves and comes back
   without a jump and a parked menu stays off the screen.  Called from NavigationMenu's update
   (exitbtn.c). */
#define CH_LIST 0xb8                      /* a sprite's display list: array, count at +0xbc */

void wide_nav(u32 pl)
{
    if (!edges_on())
        return;
    float band = -ext_x0;
    group_frame[G_NAV] = frame_no, nparts[G_NAV] = 0;
    u32 nm = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)"NavigationMenu"));
    u32 wm = nm && *(u8 *)GP(nm + 0xab) ? rt_call1(GT_FN_WORLD_MATRIX, nm) : 0;
    if (!wm)
        return;
    float tx = *(float *)GP(wm + 8), away = fabsf(tx);
    float shift = away < band ? -(band - away) : 0;
    if (shift == 0)
        return;
    u32 arr = rd32(nm + CH_LIST);
    int n = (int)rd32(nm + CH_LIST + 4);
    pass_no++;                            /* the update, not a display pass (PASS_CACHE) */
    for (int i = 0; arr && i < n && i < 128; i++) {
        u32 k = rd32(arr + 4 * i);
        float b[4];
        if (!k || !*(u8 *)GP(k + 0xab) || !world_box(k, b) || b[1] <= b[0])
            continue;
        if (b[0] - tx >= -0.05f * stage_w && b[1] - tx <= 0.4f * stage_w)
            add_part(G_NAV, k, shift, b);
    }
    pass_no++;
    static int told;
    if (!told++)
        rt_log("[wide] main menu: its left bar drawn at the screen's left edge (%.0f px)\n", band / 20);
}

/* a touch at stage point x, y (points): on a moved part, the part's own place (x changed) */
void wide_hud_touch(float *x, float y)
{
    if (!edges_on())
        return;
    float tx = *x * 20, ty = y * 20;
    for (int g = 0; g < NGROUPS; g++) {
        if (group_frame[g] + 2 < frame_no)
            continue;
        for (int i = 0; i < nparts[g]; i++) {
            const float *b = parts[g][i].box;
            if (tx >= b[0] && tx <= b[1] && ty >= b[2] && ty <= b[3]) {
                *x -= parts[g][i].shift / 20;
                return;
            }
        }
    }
}

static void leaf(CPU *c, void (*real)(CPU *), int shape)
{
    u32 ch = c->r[0];
    float b[4], keep[6];
    u32 wm = 0;
    if (shape && menu3d_hide_backdrop(ch))
        return;
    /* DYNAMIC_PRICE (shop.c): the title bar's NEW on the cash (cashNew, tools/swfpatch_cashnew.py) is drawn
       only while the shop's rate is one the player has not seen */
    u32 par = rd32(ch + 0x40);
    if (par && !strcmp(char_name(par), "cashNew")) {
        if (!shop_cash_new())
            return;
    }
    float shift = ext_x0 != 0 || ext_x1 != 0 ? hud_shift(ch) : 0;
    if (shift != 0) {                     /* a part drawn at the screen's edge */
        if (world_box(ch, b) && (b[1] + shift < ext_x0 || b[0] + shift > ext_x1))
            return;
        wm = rt_call1(GT_FN_WORLD_MATRIX, ch);
        memcpy(keep, GP(wm), sizeof keep);
        float m[6];
        memcpy(m, keep, sizeof m);
        m[2] += shift;
        memcpy(GP(wm), m, sizeof m);
        c->r[0] = ch;
        in_anchored = 1;
        real(c);
        in_anchored = 0;
        memcpy(GP(wm), keep, sizeof keep);
        return;
    }
    if ((ext_x0 != 0 || ext_x1 != 0) && world_box(ch, b)) {
        if (dumping)
            dump(shape ? "shape" : "text", ch, b);
        if (off_stage(b) || card_off(ch))
            return;
        float m[6], s = 1;
        int interior = interior_of(ch) != 0;
        int kind = shape && !interior ? cover_kind(ch, b) : KEEP;
        if (kind == EXTEND) {
            c->r[0] = ch;
            real(c);
            extend_sides(c, real, ch, b);
            return;
        }
        if (kind == ZOOM)
            s = (ext_x1 - ext_x0) / (b[1] - b[0]);
        else if (interior)
            s = interior_scale();
        if (s > 1 && kind != STRETCH) {
            wm = rt_call1(GT_FN_WORLD_MATRIX, ch);
            memcpy(keep, GP(wm), sizeof keep);
            float cx = stage_w / 2, cy = stage_h / 2;
            memcpy(m, keep, sizeof m);
            m[0] *= s, m[1] *= s, m[2] = cx + (m[2] - cx) * s;
            m[3] *= s, m[4] *= s, m[5] = cy + (m[5] - cy) * s;
            memcpy(GP(wm), m, sizeof m);
        } else if (kind == STRETCH) {
            wm = rt_call1(GT_FN_WORLD_MATRIX, ch);
            memcpy(keep, GP(wm), sizeof keep);
            s = (ext_x1 - ext_x0) / (b[1] - b[0]);
            memcpy(m, keep, sizeof m);
            m[0] *= s, m[1] *= s, m[2] = ext_x0 + (m[2] - b[0]) * s;
            memcpy(GP(wm), m, sizeof m);
        }
    }
    c->r[0] = ch;
    real(c);
    if (wm)
        memcpy(GP(wm), keep, sizeof keep);
}

void __real_F_4eb4ac(CPU *c);
void __real_F_500b58(CPU *c);
void __wrap_F_4eb4ac(CPU *c) { leaf(c, __real_F_4eb4ac, 1); }
void __wrap_F_500b58(CPU *c) { leaf(c, __real_F_500b58, 0); }

/* character::do_display_callback (F_4ca684): the game's own drawing over a placeholder clip (the dealer's
   brand logos, maps, the cars' 3D boxes).  One on a clip off the stage sideways is skipped too: the
   dealer's next page of logos showed right of the stage).  Reached from the sprite's, the
   shape's and the text's display (other files: --wrap). */
void __real_F_4ca684(CPU *c);

void __wrap_F_4ca684(CPU *c)
{
    u32 ch = c->r[0];
    float b[4];
    if ((ext_x0 != 0 || ext_x1 != 0) && hud_shift(ch) == 0 && world_box(ch, b)) {
        if (dumping) {
            u32 cb = rd32(ch + 0x64);
            dump("callback", ch, b);
            dev_log("[wdump] %u   its function 0x%x (data 0x%x)\n", frame_no, cb ? rd32(cb + 0x4c) : 0,
                   cb ? rd32(cb + 0x50) : 0);
        }
        if (off_stage(b) || card_off(ch))
            return;
    }
    c->r[0] = ch;
    __real_F_4ca684(c);
}

/* The cockpit's steering wheel and the hands on it are not in the interior picture: they are a 3D model
   the game draws in the display callback of HUD.RenderWheel (0x276918: in the cockpit view, 0x1660f0)
   with a camera of its own, its projection made by 0x14d988(matrix, 55 degrees, height / width, near,
   far): the 55 degrees span the screen's height (the drawable is portrait), so the wheel keeps its size
   against the screen's height on any width.  Enlarging only the picture (above) left the wheel at the
   4:3 size and place, small and high against the dashboard, the gauges behind its hub (seen at 21:9 in
   the Dodge Challenger: the wheel out of proportion with the dashboard; port
   fault, the screen's shape).  The matrix (row-major, column vectors: m[11] = the depth term, m[14] =
   -1) is turned a quarter for the portrait drawable by 0x12dd00 (0x14d400), then loaded by the device.
   On a wide screen its first two rows, the drawable's x and y, are multiplied by the picture's
   enlargement between the two (recomp/codegen.py HOST_INSNS 0x166254), so that the wheel grows with the
   picture around the same middle (the screen's).  The renderer's own projection set just before
   (0x14a1c4) does not move the wheel, and a narrower angle handed to 0x14d988 makes the wheel vanish.  On a 4:3 screen nothing changes. */
void gt_wheel_proj(u32 m)
{
    if (ext_x0 == 0 && ext_x1 == 0)
        return;
    float s = interior_scale(), v[8];
    if (s <= 1)
        return;
    memcpy(v, GP(m), sizeof v);
    for (int i = 0; i < 8; i++)
        v[i] *= s;
    memcpy(GP(m), v, sizeof v);
    static int told;
    if (!told++)
        rt_log("[wide] cockpit wheel: enlarged %gx with the picture\n", s);
}

/* The menus' own drawing (track maps, brand logos, the loading wheel, the cars' 3D boxes) finds where a
   SWF placeholder is on the screen with GetBounds (0x2630d0: the box, world matrix, times the movie's
   pixel scale, gameswf root::get_pixel_scale = max(screen w / 1024, screen h / 768); twentieths of a
   pixel, which the callers divide by 20), then draws there
   in Begin2DRendering's 0 .. W x 0 .. H pixels (0x14a660, W, H = [0x7711c0], [0x7711bc]).  On a wide
   screen that scale is the width's (2732 / 1024 = 2.67 against 1536 / 768 = 2), so those pictures came
   out a third too big and from the screen's corner, not the centred stage's (Quick Race's and the track
   list's maps off their panels, the dealer's logos off their
   cells).  Hooked at its last store (recomp/codegen.py HOST_INSNS 0x26322c): the box scaled back to the
   height's scale and moved by the side band; in the cockpit picture enlarged with it (above). */
static float bx_sx = 1, bx_sy = 1, bx_dx;   /* the box is twips x sx; the fix: twips x sy + dx (x) */

/* The track list's backdrop (TrackSelectionMenu.RenderBackground, the game's own drawing of the track's
   picture over the whole stage) is a picture of about 5:3 (1270 x 768 pixels on a 1366 x 768 screen)
   drawn to the box's height and centred: the 3D scene showed in strips past its sides on 16:9.
   Its box is enlarged to make the picture span the screen. */
#define BACKDROP_ASPECT 1.64f

static float backdrop_scale(void)
{
    float s = (ext_x1 - ext_x0) / stage_h / BACKDROP_ASPECT;
    return s < 1 ? 1 : s;
}

void gt_wide_bounds(u32 ch, u32 rect)
{
    if (bx_sx == bx_sy)
        return;
    float r[4], s = 1, cx = stage_w / 2, cy = stage_h / 2;
    if (interior_of(ch))
        s = interior_scale();
    else if (!strcmp(char_name(ch), "RenderBackground"))
        s = backdrop_scale();
    memcpy(r, GP(rect), sizeof r);
    if (dumping) {
        float d[4] = {r[0] / bx_sx, r[1] / bx_sx, r[2] / bx_sx, r[3] / bx_sx};
        dump("bounds", ch, d);
    }
    float shift = hud_shift(ch);          /* the race HUD's track map, drawn at the screen's edge */
    for (int k = 0; k < 4; k++) {
        float t = r[k] / bx_sx, mid = k < 2 ? cx : cy;
        t = mid + (t - mid) * s;
        r[k] = (t + (k < 2 ? shift : 0)) * bx_sy + (k < 2 ? bx_dx : 0);
    }
    memcpy(GP(rect), r, sizeof r);
}

/* The menus' 3D pictures (the Paddocks prize's car or coin and its white studio GarageScene, the garage's
   and the dealer's cars: twelve callers) set their viewport from a GetBounds box with 0x257200: the box
   is made the screen's shape (Begin2D's W / H, [0x7711c0] / [0x7711bc]) around it, and the projection
   takes that shape.  The widening goes the wrong way: x0 += (w - new w) * -0.5, the box moved right by
   half the growth instead of left (y alike: up by half).  An original fault the iPad hardly shows (a
   prize box of 646 x 575 grew to 767 wide: the coin 60 pixels right of the box's middle, near the
   screen's; the art is placed around it).  On a wide screen the growth is large: the studio started a
   third of the way in and the car sat at the right edge (seen at 21:9; port fault, the screen's shape).
   Hooked at the box's load (recomp/codegen.py
   HOST_INSNS 0x257244): the iPad's own fit, with its 4:3 and its fault, gives the picture's middle and
   height; the box handed on is the screen's shape around that middle, so the game's fit leaves it be.
   The same as the iPad's picture, the studio across the screen.  On a 4:3 screen the box as it was. */
void gt_wide_fit(u32 rect, u32 *out)
{
    float r[4];
    memcpy(r, GP(rect), sizeof r);
    float W = (float)(int)rd32(GT_SCREEN_PX_H), H = (float)(int)rd32(GT_SCREEN_PX_W), w = r[1] - r[0], h = r[3] - r[2];
    if (bx_sx != bx_sy && W > 0 && H > 0 && W / H > 4.0f / 3 * 1.001f && w > 0 && h > 0) {
        float vx, vy, vw, vh;
        if (w / h > 4.0f / 3)
            vw = w, vh = w * 0.75f, vx = r[0], vy = r[2] - (h - vh) * 0.5f;
        else
            vh = h, vw = h * 4.0f / 3, vx = r[0] - (w - vw) * 0.5f, vy = r[2];
        float mid = vx + vw / 2, nw = vh * W / H;
        r[0] = mid - nw / 2, r[1] = mid + nw / 2, r[2] = vy, r[3] = vy + vh;
        if (dumping)
            dev_log("[wdump] %u fit %.0f..%.0f x %.0f..%.0f -> %.0f..%.0f x %.0f..%.0f (px)\n", frame_no,
                   (vx) / 20, (vx + vw) / 20, vy / 20, (vy + vh) / 20, r[0] / 20, r[1] / 20, r[2] / 20, r[3] / 20);
    }
    memcpy(out, r, sizeof r);
}

/* The menus' clip (the callback 0x263240 of the clips StartClipping / StopClipping: Car Collection's View,
   GarageCarSelectionMenu): GetBounds of the callback's object into a box on the stack, then a scissor
   there (0x14a290).  In View its object is none, GetBounds leaves the box as it was, and the box is the
   one before it on the stack: the car's 3D box (CarModel, the stage's width): the iPad cut nothing
   off.  On a wide screen that is the 4:3 stage: the car's studio, drawn across, was cut to it, grey
   beside (seen at 21:9 in Car Collection's View; port
   fault).  Hooked at its "add r0, sp, #8" (recomp/codegen.py HOST_INSNS 0x2632fc): a clip spanning the
   stage's width spans the screen's. */
void gt_wide_clip(u32 rect)
{
    if (bx_sx == bx_sy)
        return;
    float r[4], x0 = bx_dx, x1 = bx_dx + stage_w * bx_sy, tol = 0.02f * stage_w * bx_sy;
    memcpy(r, GP(rect), sizeof r);
    if (r[0] <= x0 + tol && r[1] >= x1 - tol) {
        if (dumping)
            dev_log("[wdump] %u clip %.0f..%.0f widened (px)\n", frame_no, r[0] / 20, r[1] / 20);
        r[0] = 0, r[1] = stage_w * bx_sx;
        memcpy(GP(rect), r, sizeof r);
    }
}

/* The side bands of a frame with nothing but menus.  A screen without the 3D scene behind it (the
   loading screens, the Paddocks' trucks) showed the game's clear colour there, white;
   the iPad never shows it.  When nothing was drawn yet this frame as the first menu starts,
   the bands are made black first, as a 4:3 picture on a wide screen is; what the SWFs draw past the
   stage still goes over them. */
static u32 draws_at_start;
static int bands_seen;

void wide_frame_start(void)
{
    draws_at_start = gles_draws();
    bands_seen = 0;
    if (!frame_no) {
        const char *s = dev_getenv("GT_WIDE_DUMP");
        while (s && *s && ndump < 16) {
            dump_frames[ndump++] = atoi(s);
            s = strchr(s, ',');
            if (s)
                s++;
        }
    }
    frame_no++;
    dumping = 0;
    for (int i = 0; i < ndump; i++)
        dumping |= dump_frames[i] == frame_no;
}

static void black_bands(int vw, int vh, float band)
{
    int b = (int)(band + 0.5f), sc = 0, box[4];
    float cc[4];
    if (b <= 0)
        return;
    /* GT_GL_STATS=1 (testing, pass 4): the time of these queries, every 600 frames */
    static int timed = -1;
    static LARGE_INTEGER fq, t0, t1;
    static double acc;
    static unsigned calls;
    if (timed < 0) {
        timed = dev_getenv("GT_GL_STATS") && *dev_getenv("GT_GL_STATS") == '1';
        QueryPerformanceFrequency(&fq);
    }
    if (GT_DEV && timed)
        QueryPerformanceCounter(&t0);
    /* QUERY_FREE (gles.c, pass 4): kept, not asked (these queries took 0.20 ms a frame, mid-frame) */
    gles_state(GL_SCISSOR_TEST, &sc);
    gles_state(GL_SCISSOR_BOX, box);
    gles_state(GL_COLOR_CLEAR_VALUE, cc);
    if (GT_DEV && timed) {
        QueryPerformanceCounter(&t1);
        acc += (double)(t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart;
        if (++calls == 600) {
            rt_log("[wide] side bands' state queries: %.3f ms a frame\n", acc * 1000 / 600);
            calls = 0, acc = 0;
        }
    }
    p_glEnable(GL_SCISSOR_TEST);
    p_glClearColor(0, 0, 0, 1);
    if (vh > vw) {                                      /* portrait drawable: the screen's x is its y */
        p_glScissor(0, 0, (u32)vw, (u32)b);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glScissor(0, (u32)(vh - b), (u32)vw, (u32)b);
    } else {
        p_glScissor(0, 0, (u32)b, (u32)vh);
        p_glClear(GL_COLOR_BUFFER_BIT);
        p_glScissor((u32)(vw - b), 0, (u32)b, (u32)vh);
    }
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glClearColor(cc[0], cc[1], cc[2], cc[3]);
    p_glScissor((u32)box[0], (u32)box[1], (u32)box[2], (u32)box[3]);
    if (!sc)
        p_glDisable(GL_SCISSOR_TEST);
}

/* begin_display(this, background, x0, y0, width, height, frame x0, x1, y0, y1) */
void __wrap_F_517608(CPU *c)
{
    u32 sp = c->r[13];
    int vw = (int)rd32(sp), vh = (int)rd32(sp + 4);
    float f[4];
    memcpy(f, GP(sp + 8), sizeof f);
    float lw = (float)(vw > vh ? vw : vh), lh = (float)(vw > vh ? vh : vw);   /* landscape pixels */
    float fw = f[1] - f[0], fh = f[3] - f[2];
    pass_no++;                            /* PASS_CACHE: a new display pass */
    int wide = ui_wide_ui && fh > 0 && fw > 0 && lh > 0 && lw / lh > fw / fh * 1.001f;
    ext_x0 = ext_x1 = 0;
    if (wide) {
        float nw = fh * lw / lh, ext = (nw - fw) / 2, g[2] = {f[0] - ext, f[1] + ext};
        static int told;
        if (!told) {
            told = 1;
            rt_log("[wide] gameswf: the %g x %g twips stage in a %g x %g frame (one scale, centred)\n", fw, fh, nw, fh);
        }
        memcpy(GP(sp + 8), g, sizeof g);
        ext_x0 = g[0], ext_x1 = g[1], stage_w = fw, stage_h = fh;
        float sx = lw * 20 / fw, sy = lh * 20 / fh;      /* pixels per stage pixel, by width / height */
        bx_sx = sx, bx_sy = sy, bx_dx = (lw * 20 - fw * sy) / 2;   /* the box: twentieths of a pixel */
        if (!bands_seen) {
            bands_seen = 1;
            if (gles_draws() == draws_at_start)
                black_bands(vw, vh, (lw - fw / 20 * sy) / 2);
        }
    } else {
        bx_sx = bx_sy = 1, bx_dx = 0;
    }
    __real_F_517608(c);
    if (wide)
        memcpy(GP(sp + 8), f, 2 * sizeof(float));
}
