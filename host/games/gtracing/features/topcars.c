/* topcars.c -- TOP_CARS: the main menu's "Top Ten Cars" list made here, offline.

   Ten cars drawn from the 10 best of each class (C, B, A, S: 40 in all).  Not a port fault: the
   original list comes from Gameloft's server, which is gone (and the port is offline), so the page only ever
   said "No network found."

   The original (read from the code):
   - G = [[0x70c048]]; the TopCars manager is G+0xf4 (made by 0x12bc3c, 0x18 bytes, vtable 0x78a328):
     +0xc a state (0 .. 5 the server request's steps, 6 idle), +0x10 an array of 10 ints, +0x14 "has the list".
     The array starts as a built-in list (0x773b80) and is replaced by the server's answer "v|id|id|..."
     (0x2c2988), which then sets +0x14 = 1 and G+0xe8 = 1.
   - GS_LoadMainMenu (0x1a7260) and, on a retry flag G+0xe9, the main menu's update (0x251ed0) call 0x12bc00,
     which starts the request once (G+0xf8).  The main menu's update pushes "dataTopTen" to the SWF when
     G+0xe8 is set (0x251e94), and clears it.
   - The SWF asks NativeHasTopCars (0x2bf8cc: +0x14), then NativeGetTopCars (0x2bf8fc) for the 10 cars: each
     number is looked up in the car table's field 0x22 (an online id), first row that matches.  NativeGoToTopCar
     (0x2c2620) opens CarDealerMenu on that car, if its brand's dealer list (0x2b6924) has it.

   Here (0x12bc00 wrapped: its two callers are in other generated files): after it has run, the array gets 10
   cars drawn at random from the 40 best, the 10 with the most horsepower (field 0x10, then torque, field
   0x11) in each class (field 0x12: 0 C, 1 B, 2 A, 3 S), the 10 shown best first (class S to C, then hp);
   the state is set to 6 (no server request), +0x14 = 1 and G+0xe8 = 1, as the server's answer would.  It is
   drawn again each time the main menu loads (the start and after every race).  Left out: car 0x13a (the
   dealer's lists leave it out, 0x2b6924), the Red Bull event cars 411 .. 413, and a car whose online id is 0
   or found first on another row (the Ferrari 288 GTO add-on has the 458's 88).  The add-on cars are in the
   table, so they take part.  GT_TOP_CARS=0: the original (the server's list, "No network found."). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "rt.h"
#include "features.h"
#include "game.h"

#define CAR_REC 0x128              /* a row: +4 + 4 * field */
#define F_ID 0x00
#define F_HP 0x10
#define F_TORQUE 0x11
#define F_CLASS 0x12
#define F_ONLINE 0x22
#define TC_STATE 0x0c
#define TC_LIST 0x10
#define TC_HAS 0x14
#define PER_CLASS 10
#define SHOWN 10

void __real_F_12bc00(CPU *c);

static int on = -1;

static u32 field(u32 rows, int i, int f) { return rd32(rows + (u32)i * CAR_REC + 4 + 4 * (u32)f); }

static u64 seed;
static u32 rnd(u32 n)
{
    if (!seed) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        seed = (u64)t.QuadPart ^ ((u64)GetCurrentProcessId() << 32) ^ 0x9e3779b97f4a7c15ull;
    }
    seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;     /* xorshift64 */
    return (u32)((seed >> 11) % n);
}

static u32 g_rows;
static int better(int a, int b)                 /* row a before row b: class S first, then hp, torque */
{
    int ca = (int)field(g_rows, a, F_CLASS), cb = (int)field(g_rows, b, F_CLASS);
    if (ca != cb)
        return ca > cb;
    int ha = (int)field(g_rows, a, F_HP), hb = (int)field(g_rows, b, F_HP);
    if (ha != hb)
        return ha > hb;
    return (int)field(g_rows, a, F_TORQUE) > (int)field(g_rows, b, F_TORQUE);
}

static void sort_rows(int *r, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && better(r[j], r[j - 1]); j--) {
            int t = r[j];
            r[j] = r[j - 1], r[j - 1] = t;
        }
}

static void fill(void)
{
    u32 g = rd32(GT_G_SLOT) ? gt_game() : 0, tc = g ? rd32(g + 0xf4) : 0;
    u32 tbl = gt_car_table();
    u32 rows = tbl ? rd32(tbl) : 0, list = tc ? rd32(tc + TC_LIST) : 0;
    int n = tbl ? (int)rd32(tbl + 8) : 0;
    if (!tc || !rows || !list || n <= 0 || n > 4096)
        return;
    g_rows = rows;
    /* each class's rows, best first */
    static int cls[4][4096], pool[4 * PER_CLASS];
    int cn[4] = {0}, k = 0;
    for (int i = 0; i < n; i++) {
        u32 id = field(rows, i, F_ID), on_id = field(rows, i, F_ONLINE), c = field(rows, i, F_CLASS);
        if (c > 3 || id == 0x13a || (id >= 411 && id <= 413) || !on_id || on_id == 0xffffffffu)
            continue;
        int first = i;                          /* the row NativeGetTopCars would find for this id */
        for (int j = 0; j < i; j++)
            if (field(rows, j, F_ONLINE) == on_id) {
                first = j;
                break;
            }
        if (first != i)
            continue;
        cls[c][cn[c]++] = i;
    }
    for (int c = 0; c < 4; c++) {
        sort_rows(cls[c], cn[c]);
        for (int j = 0; j < cn[c] && j < PER_CLASS; j++)
            pool[k++] = cls[c][j];
    }
    if (k == 0)
        return;
    /* SHOWN of them at random (a partial Fisher-Yates), shown best first */
    int m = k < SHOWN ? k : SHOWN;
    for (int i = 0; i < m; i++) {
        int j = i + (int)rnd((u32)(k - i)), t = pool[i];
        pool[i] = pool[j], pool[j] = t;
    }
    sort_rows(pool, m);
    const char *test = dev_getenv("GT_TOP_CARS_TEST");   /* testing: these car ids, in this order */
    if (test && !strcmp(test, "rent")) {          /* testing: a car the rent manager offers now, first */
        u32 pm = gt_profile(), pp = pm ? rd32(pm + 4) : 0;
        u32 gn = pp ? rd32(pp + 0x30) : 0, gbase = pp ? rd32(pp + 0x34) : 0;   /* the garage (menu3d.c) */
        for (int i = 0; pp && i < n; i++) {   /* offered (0x2c4150), not in the garage, not a Red Bull car */
            u32 id = field(rows, i, F_ID);
            int owned = 0;
            for (u32 j = 0; gbase && j < gn && j < 1024; j++)
                owned |= (int)rd32(gbase + 0x28 + j * 0x84) == i;
            if (owned || (id >= 411 && id <= 413) || !rt_call2(0x2c4150u, pp + 0x38, id))
                continue;
            pool[0] = i;
            break;
        }
    } else if (test && *test) {
        m = 0;
        for (const char *p = test; *p && m < SHOWN; p += strcspn(p, ",") + (p[strcspn(p, ",")] == ',')) {
            u32 want = (u32)atoi(p);
            for (int i = 0; i < n; i++)
                if (field(rows, i, F_ID) == want) {
                    pool[m++] = i;
                    break;
                }
        }
    }
    char line[256];
    int o = 0;
    for (int i = 0; i < SHOWN; i++) {
        u32 v = i < m ? field(rows, pool[i], F_ONLINE) : 0xffffffffu;
        wr32(list + 4 * (u32)i, v);
        if (i < m && o < (int)sizeof line - 24)
            o += snprintf(line + o, sizeof line - (size_t)o, " %c%u",
                          "CBAS"[field(rows, pool[i], F_CLASS)], field(rows, pool[i], F_ID));
    }
    wr32(tc + TC_STATE, 6);                     /* idle: no server request */
    *(u8 *)GP(tc + TC_HAS) = 1;
    *(u8 *)GP(g + 0xe8) = 1;                    /* the main menu tells the SWF ("dataTopTen") */
    rt_log("[topcars] %d of the %d best (C %d, B %d, A %d, S %d; up to %d per class):%s\n", m, k,
           cn[0] < PER_CLASS ? cn[0] : PER_CLASS, cn[1] < PER_CLASS ? cn[1] : PER_CLASS,
           cn[2] < PER_CLASS ? cn[2] : PER_CLASS, cn[3] < PER_CLASS ? cn[3] : PER_CLASS, PER_CLASS, line);
}

/* Buy / Rent: to the Car Dealer */
/* Buy / Rent in the list go straight to the Car Dealer at the chosen car, with its Buy or Rent popup open (the
   original opened the list's own popup).  A change of the original flow.
   The original: a row's Buy / RENT calls NativeRentOrBuyTopCar (0x2bf78c: G+0xfc = the car's row), then the
   menu's event handler (NavigationMenu 0x1eb5ac, MainMenu 0x1e2cb8; vtable slot +0x2c) gets "btnTopTenBuy" /
   "btnTopTenRent" (event: +4 name, +8 type, 6 = click) and opens its own BuyRentPopup.
   The event kinds come 0 8 10 4 6 1 2; the list's ActionScript calls NativeRentOrBuyTopCar between 6 and 2, and
   the original acts on 2.
   Here those two clicks are taken from the handler (NavigationMenu's through exitbtn.c's hook, which asks
   topcars_event first; MainMenu's slot hooked here), on kind 2:
   - the car's place in its brand's dealer list (0x2b6924, sorted by 0x2c2230, as NativeGoToTopCar 0x2c2620 finds
     it) goes to [0x70be0c], the brand to [0x70be34], [0x70be10] = 0 (Vehicle Dealer, not Rent Cars), and the
     Car Dealer is opened through the menu scene's slot +0x34 (scene, "CarDealerMenu", 1, 0), as the brand page
     does it: the camera flies to the dealer's camera first.  (NativeGoToTopCar's way, the menu player's vtable
     +0x28, opens the menu with no camera, so the car was seen from the main menu's camera, not the dealer's.)
   - when the dealer shows that car (its update, vtable 0x787b78 slot +0x1c; the row shown is
     [[0x70be84]+0x24]+0x28, as its buttons read it) with no popup open (+0xaf2), a tap is given on the middle of
     CarDealerMenu.btnBuy1 (the price button) or CarDealerMenu.btnRent (ui_tap, pcinput.c): the button's own
     ActionScript opens the popup (the native handler only fills its texts), the dealer's own checks included.
     (A click event sent to the handler 0x206bd4 alone filled the texts but showed no popup; "btnBuy" there is
     the popup's OK.)
   Given up after 10 s if the dealer never shows the car.  A car in no dealer list: the original popup.
   The list's RENT is pressable only for a car the save's rent manager offers now and the player lacks
   (NativeGetTopCars' IsCanRent: 0x2c4150 on player+4+0x38, 0x2681f4); the offer changes from run to run, as
   the Rent Cars page shows.
   Testing: GT_TOP_CARS_LOG=1 logs the events and the rentable cars; GT_TOP_CARS_TEST=id,id,... shows those cars,
   GT_TOP_CARS_TEST=rent puts a car offered for rent first (drawn again at the wheel's first turn, when the
   profile is in). */
#define MAIN_EVENT_SLOT (GT_VT_MAIN_MENU + GT_VT_EVENT)
#define MAIN_EVENT_FN 0x1e2cb8u
#define DEALER_UPDATE_SLOT (0x787b78u + 0x1c)
#define DEALER_UPDATE_FN 0x206664u
#define VT_BOUND 0x12cu               /* character vtable: get_bound(rect *) */
#define FN_BRAND_LIST 0x2b6924u       /* (table, brand, &count, 0) -> rows (new[]) */
#define FN_SORT_LIST 0x2c2230u        /* (rows, count) */
#define DEALER_PLACE_SLOT 0x70be0cu   /* -> the car's place in its brand's list */
#define DEALER_BRAND_SLOT 0x70be34u   /* -> the brand */
#define DEALER_CAR_SLOT 0x70be84u     /* -> [+0x24] -> +0x28 the row shown */
#define STR_CAR_DEALER 0x67c911u      /* "CarDealerMenu" */
#define DEALER_MODE_SLOT 0x70be10u    /* -> byte: 0 Vehicle Dealer, 1 Rent Cars */
#define SCENE_OPEN 0x34               /* menu scene vtable slot: (scene, name, 1, 0) camera, then the menu */
#define G_TOP_ROW 0xfc
#define F_BRAND 0x01

static void (*main_event_fn)(CPU *), (*dealer_update_fn)(CPU *);
static int pending_row = -1, pending_rent, pending_frames;

static int to_dealer(CPU *c, int row)
{
    u32 g = gt_game(), tbl = gt_car_table();
    u32 rows = tbl ? rd32(tbl) : 0;
    if (!g || !rows || row < 0 || row >= (int)rd32(tbl + 8))
        return 0;
    u32 brand = field(rows, row, F_BRAND);
    static u32 cnt;
    cnt = 0;
    u32 args[4] = {tbl, brand, GA(&cnt), 0};
    u32 list = rt_guest_call(c, FN_BRAND_LIST, 4, args);
    if (!list)
        return 0;
    args[0] = list, args[1] = cnt;
    rt_guest_call(c, FN_SORT_LIST, 2, args);
    int at = -1;
    for (u32 i = 0; i < cnt && i < 4096; i++)
        if ((int)rd32(list + 4 * i) == row) {
            at = (int)i;
            break;
        }
    if (at < 0)
        return 0;
    u32 scene = menu3d_scene();
    if (!scene)
        return 0;
    wr32(rd32(DEALER_PLACE_SLOT), (u32)at);
    wr32(rd32(DEALER_BRAND_SLOT), brand);
    *(u8 *)GP(rd32(DEALER_MODE_SLOT)) = 0;     /* the Vehicle Dealer, not Rent Cars (as the brand page sets it) */
    args[0] = scene, args[1] = STR_CAR_DEALER, args[2] = 1, args[3] = 0;
    rt_guest_call(c, rd32(rd32(scene) + SCENE_OPEN), 4, args);
    rt_log("[topcars] %s: car %u (row %d) -> Car Dealer, brand %u, place %d of %u\n", pending_rent ? "Rent" : "Buy",
           field(rows, row, F_ID), row, brand, at, cnt);
    return 1;
}

static int top_event(CPU *c)
{
    u32 ev = c->r[1];
    const char *name = rd32(ev + 4) ? (const char *)GP(rd32(ev + 4)) : "";
    int buy = strstr(name, "btnTopTenBuy") != NULL, rent = strstr(name, "btnTopTenRent") != NULL;
    if (dev_getenv("GT_TOP_CARS_LOG") && !buy && !rent && rd32(ev + 8) == 2)
        rt_log("[topcars] other event %s\n", name);
    const char *test = dev_getenv("GT_TOP_CARS_TEST");   /* testing "rent": drawn again once the profile is in */
    if (on && test && !strcmp(test, "rent") && !strcmp(name, "btnLeftMenu") && rd32(ev + 8) == 2)
        fill();
    if (!on || (!buy && !rent))
        return 0;
    if (dev_getenv("GT_TOP_CARS_LOG")) {
        u32 g0 = gt_game();
        rt_log("[topcars] event %s type %u, G+0xfc %d\n", name, rd32(ev + 8), g0 ? (int)rd32(g0 + G_TOP_ROW) : -9);
    }
    if (dev_getenv("GT_TOP_CARS_LOG") && rd32(ev + 8) == 0) {   /* the rows the rent manager can rent (0x2c4150) */
        u32 pm = gt_profile(), pp = pm ? rd32(pm + 4) : 0, tbl = gt_car_table();
        char line[512];
        int o = 0;
        for (int i = 0; pp && tbl && i < (int)rd32(tbl + 8) && o < 480; i++)
            if (rt_call2(0x2c4150u, pp + 0x38, field(rd32(tbl), i, F_ID)))   /* (manager, car id) */
                o += snprintf(line + o, sizeof line - (size_t)o, " %u", field(rd32(tbl), i, F_ID));
        line[o] = 0;
        rt_log("[topcars] rentable cars:%s\n", line);
    }
    if (rd32(ev + 8) != 2)
        return 1;           /* the other kinds (0 8 10 4 6 1 2 in that order): type 2 comes after the list's
                               NativeRentOrBuyTopCar has set G+0xfc, and the original acts on it too */
    u32 g = gt_game();
    int row = g ? (int)rd32(g + G_TOP_ROW) : -1;
    pending_rent = rent;
    CPU save = *c;
    int ok = to_dealer(c, row);
    *c = save;
    if (!ok) {
        rt_log("[topcars] %s: row %d is in no dealer list, the list's own popup\n", rent ? "Rent" : "Buy", row);
        return 0;
    }
    pending_row = row, pending_frames = 0;
    /* the list's own BuyRentPopup, which the button's ActionScript has just started to show (it showed blank,
       "50 cr", during the camera's flight), is put at its end of hiding */
    u32 pl = rd32(c->r[0] + 4);
    static const char *const popups[] = {"NavigationMenu.BuyRentPopup", "MainMenu.BuyRentPopup"};
    for (int i = 0; pl && i < 2; i++)
        if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)popups[i])))
            rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)popups[i]), GA((void *)"Hided"), 0);
    return 1;
}

/* NavigationMenu's handler slot is exitbtn.c's hook, which asks here first */
int topcars_event(CPU *c) { return top_event(c); }

static void main_event_hook(CPU *c)
{
    if (p2p_nav_event(c))
        return;   /* FIRST_NAME (p2p.c): the first start's name form, in MainMenu's FlurryPopup: not the handler's */
    if (!top_event(c))
        main_event_fn(c);
}

static void dealer_update_hook(CPU *c)
{
    u32 self = c->r[0];
    dealer_update_fn(c);
    if (pending_row < 0)
        return;
    if (++pending_frames > 600) {
        rt_log("[topcars] the Car Dealer did not show row %d: no popup\n", pending_row);
        pending_row = -1;
        return;
    }
    u32 car = rd32(DEALER_CAR_SLOT), sel = car ? rd32(car + 0x24) : 0;
    int shown = sel ? (int)rd32(sel + 0x28) : -1;
    if (dev_getenv("GT_TOP_CARS_LOG") && pending_frames % 10 == 1)
        rt_log("[topcars] dealer frame %d: car slot %x sel %x shown %d popup %u\n", pending_frames, car, sel, shown,
               *(u8 *)GP(self + 0xaf2));
    if (pending_frames < 15 || shown != pending_row || *(u8 *)GP(self + 0xaf2))
        return;
    /* the button's middle on the stage (as padmenu.c's stage_box: its bound through its parent's world matrix) */
    u32 pl = rd32(self + 4);
    const char *path = pending_rent ? "CarDealerMenu.btnRent" : "CarDealerMenu.btnBuy1";
    u32 ch = pl ? rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path)) : 0;
    static float rect[4];
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    if (ch) {
        rt_call2(rd32(rd32(ch) + VT_BOUND), ch, GA(rect));
        u32 parent = rd32(ch + 0x40), w = parent ? rt_call1(GT_FN_WORLD_MATRIX, parent) : 0;
        static const float id6[6] = {1, 0, 0, 0, 1, 0};
        float m[6];
        memcpy(m, w ? GP(w) : id6, sizeof m);
        for (int k = 0; k < 4 && rect[0] <= rect[1] && rect[2] <= rect[3]; k++) {
            float x = rect[k & 1], y = rect[2 + (k >> 1)];
            float sx = (m[0] * x + m[1] * y + m[2]) / 20.0f, sy = (m[3] * x + m[4] * y + m[5]) / 20.0f;
            x0 = sx < x0 ? sx : x0, x1 = sx > x1 ? sx : x1, y0 = sy < y0 ? sy : y0, y1 = sy > y1 ? sy : y1;
        }
    }
    pending_row = -1;
    if (x0 > x1) {
        rt_log("[topcars] Car Dealer: no %s on the stage, no popup\n", path);
        return;
    }
    rt_log("[topcars] Car Dealer shows the car: a tap on %s at (%.0f, %.0f)\n", path, (x0 + x1) / 2, (y0 + y1) / 2);
    ui_tap((x0 + x1) / 2, (y0 + y1) / 2);
}

void topcars_init(void)
{
    const char *e = getenv("GT_TOP_CARS");
    on = !(e && *e == '0');
    if (!on)
        return;
    gt_hook_slot(MAIN_EVENT_SLOT, MAIN_EVENT_FN, &main_event_fn, main_event_hook, "topcars main_event_hook");
    gt_hook_slot(DEALER_UPDATE_SLOT, DEALER_UPDATE_FN, &dealer_update_fn, dealer_update_hook, "topcars dealer_update_hook");
}

/* GS_LoadMainMenu (and the main menu's retry): starts the server request the first time */
void __wrap_F_12bc00(CPU *c)
{
    if (on < 0) {
        const char *e = getenv("GT_TOP_CARS");
        on = !(e && *e == '0');
        if (!on)
            rt_log("[topcars] off (GT_TOP_CARS=0): the server's list\n");
    }
    __real_F_12bc00(c);
    if (on) {
        fill();
        c->r[0] = 1;
    }
}
