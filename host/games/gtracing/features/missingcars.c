/* missingcars.c -- MISSING_CARS: a save holding cars the car table does not have loads, plays and saves safely.

   Loading a save whose active car is an add-on car while that add-on is off crashed.  A port fault of the add-on
   design (ns_misc.c): the car table is every car file in the
   bundle and the add-on folders, so a car of an add-on is in it only while the add-on is there.  Found while
   testing the release script (a save with an add-on car, GT_ADDONS=0); the iPad game never loses a car.

   What the game does (read from the code; seen on save copies):
   - the profile load 0x270ac0 picks the loader by the save's version (0x26bc08, 0x26d4f0, 0x26ee20); each reads
     the garage's cars as car ids and turns each into a row of the car table with 0x2b5fe8 (table, id): -1 when
     the table has no such car (return addresses 0x26beb8, 0x26d7a4, 0x26f0f0).  The garage ([player + 4]) keeps
     the row (entry + 0x28; entries of 0x84 bytes at [+0x34], count +0x30) and the chosen car's slot (+0x2c);
     the player keeps the entries the menus use in a vector at +8 .. +0xc (0x269084 makes it from the garage);
   - still inside the load (0x2709dc in 0x26ee20), and wherever a car is readied later (13 callers), the car
     file's loader 0x2b7e78 (table, entry, ...) reads table[entry's row].name without a check: for row -1 the
     words before the table, a bad read in msvcrt's sprintf (or in the game's formatter 0x1b147c) when they are
     no pointer.  So the crash came while the main menu loaded, on some runs and not others;
   - the Garage (My Cars) reads each car's name the same way (0x2b6f50, row -1: a crash), and it can
     sell a car (GarageMenu.btnSell);
   - the profile's writer 0x266820 (player): writes the chosen slot and the count, copies the vector over the
     garage's entries when the vector holds at least `count` of them, then writes each entry: the car id as the
     table's field 0 of its row (0x2b689c, return 0x266968; 0 for row -1, so one save without the add-on dropped
     its cars for good: seen on a save copy), then the entry's own words (+0x2c .. +0x7c).  No other part of the
     save is indexed by garage slot that the writer shows.
   Here:
   - the car file's loader, given a garage entry without a row, loads the garage's first car that has one, and
     that car becomes the chosen one if the entry was (logged); for any other entry without a row it answers as
     for a car file that is missing (0), as the original does when it cannot open one;
   - right after the load (optimisation pass 4; they go back to their original places when 0x266820 writes the
     save):
     the entries without a row are taken out of the garage and the player's list, kept here with their car ids
     and places.  So while the add-on is away its cars are not in the game at all: no menu reads them, none can
     be sold, raced or upgraded;
   - the writer gets the whole garage: the game's own copy of the vector is made first, then the kept entries go
     back to their places in a list made for the write (the vector shown empty to the writer meanwhile, so it
     copies nothing over that list), each written with its car id, and the chosen car the save had unless the
     player chose another one.  After the write the garage is the full list again.  So the cars come back with the add-on,
     the chosen one too.
   Tested on save copies.  GT_MISSING_CARS=0: (the crashes too). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "rt.h"
#include "features.h"
#include "game.h"

void __real_F_2b5fe8(CPU *c);
void __real_F_2b689c(CPU *c);
void __real_F_2b7e78(CPU *c);
void __real_F_270ac0(CPU *c);
void __real_F_266820(CPU *c);

#define P_GARAGE 4
#define P_VEC 8                         /* the player's list: begin +8, end +0xc */
#define G_CHOSEN 0x2c                   /* the chosen car's slot */
#define G_COUNT 0x30
#define G_ENTRIES 0x34
#define ENTRY 0x84
#define E_ROW 0x28
#define WRITE_SITE 0x266968u            /* the writer's "field 0 of the entry's row" */
#define MAX_KEPT 256

static const u32 load_sites[] = {0x26beb8u, 0x26d7a4u, 0x26f0f0u};

static int on = -1;
static u32 ld_garage, ld_n, ld_k;       /* the load under way: its garage, entries, entries read */
static u32 kept[MAX_KEPT];              /* the ids without a row, in their entries' order */
static int nkept, fresh;                /* fresh: a load read the garage since the last hiding */
static int chosen_was = -1, chosen_now = -1;   /* the save's chosen slot and the one put in its place */
static int writing, wr_missing;         /* inside the writer: entries without a row written so far */

/* the hidden entries: their place in the save's garage, their bytes and car id */
typedef struct { u32 place, id; u8 bytes[ENTRY]; } Hidden;
static Hidden hidden[MAX_KEPT];
static int nhidden;
static int hidden_chosen = -1;          /* the hidden entry that was the save's chosen car, or -1 */
static int chosen_vis = -1;             /* the visible slot chosen in its place (while the player keeps it) */

static int enabled(void)
{
    if (on < 0) {
        const char *e = getenv("GT_MISSING_CARS");
        on = !(e && *e == '0');
    }
    return on;
}

static u32 table_rows(void)
{
    u32 tbl = gt_car_table();
    return tbl ? rd32(tbl + 8) : 0;
}

static u32 player(void) { return gt_profile(); }

/* the first of n entries at base whose row is in a table of `rows`, or -1 */
static int first_with_row(u32 base, u32 n, u32 rows)
{
    for (u32 j = 0; base && j < n && j < 4096; j++)
        if (rd32(base + j * ENTRY + E_ROW) < rows)
            return (int)j;
    return -1;
}

/* the chosen slot moves from `was` to `now` (kept for the writer) */
static void choose(u32 garage, u32 was, u32 now, const char *where)
{
    wr32(garage + G_CHOSEN, now);
    if (chosen_now < 0)
        chosen_was = (int)was;
    chosen_now = (int)now;
    rt_log("[cars] the chosen car (garage slot %u) is not in the car table: slot %u chosen instead (%s)\n", was, now,
           where);
}

/* The order of the written garage: `nvis` visible entries with `nh` hidden ones put back at their places
   (place[k] ascending: the k-th hidden entry's place in the save's garage).  out[i] >= 0 is a visible entry,
   -1 - k the k-th hidden one.  With the visible list as it was loaded this is the save's order again; with cars
   bought or sold since, a hidden entry goes to its place or the end of the list, whichever comes first. */
void gt_missing_cars_order(const u32 *place, int nh, int nvis, int *out)
{
    int n = 0, v = 0;
    for (int k = 0; k < nh; k++) {
        while (v < nvis && (u32)n < place[k])
            out[n++] = v++;
        out[n++] = -1 - k;
    }
    while (v < nvis)
        out[n++] = v++;
}

/* (car table, car id) -> row: at the loaders' garage reads, an id without a row is kept */
void __wrap_F_2b5fe8(CPU *c)
{
    u32 id = c->r[1], garage = c->r[2], lr = c->r[14];
    __real_F_2b5fe8(c);
    if (!enabled() || (lr != load_sites[0] && lr != load_sites[1] && lr != load_sites[2]))
        return;
    u32 n = garage ? rd32(garage + G_COUNT) : 0;
    if (garage != ld_garage || ld_k >= ld_n) {          /* the first entry of a load */
        ld_garage = garage, ld_n = n, ld_k = 0, nkept = 0;
        chosen_was = chosen_now = -1;
        nhidden = 0, hidden_chosen = chosen_vis = -1;
        fresh = 1;
    }
    ld_k++;
    if (c->r[0] == 0xffffffffu && nkept < MAX_KEPT) {
        kept[nkept++] = id;
        rt_log("[cars] the save's car %u is not in the car table (an add-on off or gone): kept in the save\n", id);
    }
}

/* the car file's loader (car table, garage entry, ...): never for an entry without a row */
void __wrap_F_2b7e78(CPU *c)
{
    u32 tbl = c->r[0], e = c->r[1], rows = tbl ? rd32(tbl + 8) : 0;
    if (!enabled() || !e || rd32(e + E_ROW) < rows) {
        __real_F_2b7e78(c);
        return;
    }
    /* which list the entry is in: the player's copy or the garage's own */
    u32 pl = player(), g = pl ? rd32(pl + P_GARAGE) : 0, list = 0, n = 0;
    u32 vb = pl ? rd32(pl + P_VEC) : 0, ve = pl ? rd32(pl + P_VEC + 4) : 0;
    u32 gb = g ? rd32(g + G_ENTRIES) : 0, gn = g ? rd32(g + G_COUNT) : 0;
    if (vb && e >= vb && e < ve && (e - vb) % ENTRY == 0)
        list = vb, n = (ve - vb) / ENTRY;
    else if (gb && e >= gb && e < gb + gn * ENTRY && (e - gb) % ENTRY == 0)
        list = gb, n = gn;
    int j = list ? first_with_row(list, n, rows) : -1;
    if (j >= 0) {
        u32 slot = (e - list) / ENTRY;
        char where[64];
        snprintf(where, sizeof where, "loading it, from 0x%06x", c->r[14]);
        if (g && rd32(g + G_CHOSEN) == slot)
            choose(g, slot, (u32)j, where);
        else
            rt_log("[cars] garage slot %u is not in the car table: slot %u loaded in its place (from 0x%06x)\n", slot,
                   j, c->r[14]);
        c->r[1] = list + (u32)j * ENTRY;
        __real_F_2b7e78(c);
        return;
    }
    static int told;
    if (!told++)
        rt_log("[cars] a car not in the car table was to be loaded (from 0x%06x): skipped as a missing car file\n",
               c->r[14]);
    c->r[0] = 0;
}

/* entries of a list (base, n) whose row is not in the table: moved out, the rest closed up; the number left */
static u32 close_up(u32 base, u32 n, u32 rows)
{
    u32 w = 0;
    for (u32 j = 0; j < n; j++)
        if (rd32(base + j * ENTRY + E_ROW) < rows) {
            if (w != j)
                memmove(GP(base + w * ENTRY), GP(base + j * ENTRY), ENTRY);
            w++;
        }
    return w;
}

/* after a load: the garage's entries without a row taken out of the game (Option A) */
static void hide(u32 g, u32 rows)
{
    u32 n = rd32(g + G_COUNT), base = rd32(g + G_ENTRIES), chosen = rd32(g + G_CHOSEN);
    int k = 0;
    nhidden = 0;
    for (u32 j = 0; base && j < n && j < 4096; j++) {
        if (rd32(base + j * ENTRY + E_ROW) < rows)
            continue;
        if (nhidden == MAX_KEPT) {
            rt_log("[cars] more than %d cars the table lacks: the rest stay in the garage\n", MAX_KEPT);
            return;                     /* nothing taken out: the guards above still hold */
        }
        Hidden *h = &hidden[nhidden++];
        h->place = j;
        h->id = k < nkept ? kept[k] : 0;
        k++;
        memcpy(h->bytes, GP(base + j * ENTRY), ENTRY);
    }
    if (!nhidden)
        return;
    /* the chosen slot, counted among the visible entries (the load put it on a car that has a row) */
    u32 before = 0;
    for (int i = 0; i < nhidden; i++)
        before += hidden[i].place < chosen;
    u32 vis_chosen = chosen - before;
    hidden_chosen = -1;
    for (int i = 0; chosen_was >= 0 && i < nhidden; i++)
        if (hidden[i].place == (u32)chosen_was)
            hidden_chosen = i;
    u32 left = close_up(base, n, rows);
    wr32(g + G_COUNT, left);
    if (vis_chosen >= left)
        vis_chosen = 0;
    wr32(g + G_CHOSEN, vis_chosen);
    chosen_vis = hidden_chosen >= 0 ? (int)vis_chosen : -1;
    /* the player's list, if the load made it already */
    u32 pl = player();
    u32 vb = pl ? rd32(pl + P_VEC) : 0, ve = pl ? rd32(pl + P_VEC + 4) : 0;
    u32 vleft = 0;
    if (vb && ve > vb && rd32(pl + P_GARAGE) == g) {
        vleft = close_up(vb, (ve - vb) / ENTRY, rows);
        wr32(pl + P_VEC + 4, vb + vleft * ENTRY);
    }
    for (int i = 0; i < nhidden; i++)
        rt_log("[cars] garage slot %u (car %u) left out of the game while its add-on is off; it stays in the save\n",
               hidden[i].place, hidden[i].id);
    rt_log("[cars] garage: %u cars to play (list %u), chosen slot %u%s\n", left, vb ? (ve > vb ? vleft : 0) : 0,
           vis_chosen, hidden_chosen >= 0 ? " (the save's chosen car is one of those left out)" : "");
}

/* the profile load: the chosen car checked once more (a load that did not ready it), then the hiding */
void __wrap_F_270ac0(CPU *c)
{
    __real_F_270ac0(c);
    if (!enabled() || !ld_garage || ld_k < ld_n || !fresh)
        return;
    fresh = 0;
    u32 g = ld_garage, rows = table_rows(), n = rd32(g + G_COUNT), chosen = rd32(g + G_CHOSEN);
    u32 base = rd32(g + G_ENTRIES);
    /* one line a load: the garage as the save has it (the car id: the table's field 0 of the chosen car's row) */
    u32 crow = chosen < n ? rd32(base + chosen * ENTRY + E_ROW) : 0xffffffffu;
    u32 cid = crow < rows ? rt_call3(0x2b689cu, gt_car_table(), crow, 0) : 0;
    rt_log("[cars] profile loaded: %u cars in the garage, chosen slot %u (car %u)\n", n, chosen, cid);
    if (dev_getenv("GT_CARS_LOG")) {        /* testing: every slot's car id, and the player's list's length */
        char line[1024];
        int o = 0;
        for (u32 j = 0; j < n && j < 64 && o < (int)sizeof line - 16; j++) {
            u32 r = rd32(base + j * ENTRY + E_ROW);
            u32 id = r < rows ? rt_call3(0x2b689cu, gt_car_table(), r, 0) : 0xffffffffu;
            o += snprintf(line + o, sizeof line - (size_t)o, " %u:%d", j, (int)id);
        }
        u32 pl = player(), vb = pl ? rd32(pl + P_VEC) : 0, ve = pl ? rd32(pl + P_VEC + 4) : 0;
        rt_log("[cars] slots:%s; the player's list %u\n", line, ve > vb ? (ve - vb) / ENTRY : 0);
    }
    if (!rows || !n)
        return;
    if (!(chosen < n && rd32(base + chosen * ENTRY + E_ROW) < rows)) {
        int j = first_with_row(base, n, rows);
        if (j < 0) {
            rt_log("[cars] no car of the garage is in the car table\n");
            return;                     /* nothing to play: left as it is */
        }
        choose(g, chosen, (u32)j, "after the load");
    }
    if (nkept)
        hide(g, rows);
}

/* (car table, row, field): at the writer's car ids, an entry without a row writes its kept id */
void __wrap_F_2b689c(CPU *c)
{
    u32 lr = c->r[14], row = c->r[1], field = c->r[2];
    __real_F_2b689c(c);
    if (!writing || lr != WRITE_SITE || field != 0 || row < table_rows())
        return;
    if (nhidden) {
        if (wr_missing < nhidden)
            c->r[0] = hidden[wr_missing].id;
    } else if (wr_missing < nkept)
        c->r[0] = kept[wr_missing];
    wr_missing++;
}

/* the writer with the hidden entries back at their places; 0 if it could not be set up (then as without) */
static int write_whole(CPU *c, u32 pl)
{
    u32 g = rd32(pl + P_GARAGE);
    if (!g)
        return 0;
    u32 nvis = rd32(g + G_COUNT), img = rd32(g + G_ENTRIES), vchosen = rd32(g + G_CHOSEN);
    u32 vb = rd32(pl + P_VEC), ve = rd32(pl + P_VEC + 4);
    int total = (int)nvis + nhidden, order[4096 + MAX_KEPT];
    if (nvis > 4096)
        return 0;
    u8 *whole = malloc((size_t)total * ENTRY);
    if (!whole)
        return 0;
    /* the game's own copy first (the writer's 0x2668d4: the list over the garage when it holds `count`) */
    if (vb != ve && (ve - vb) / ENTRY >= nvis)
        memmove(GP(img), GP(vb), nvis * ENTRY);
    u32 place[MAX_KEPT];
    for (int k = 0; k < nhidden; k++)
        place[k] = hidden[k].place;
    gt_missing_cars_order(place, nhidden, (int)nvis, order);
    u32 wchosen = 0;
    for (int i = 0; i < total; i++) {
        if (order[i] >= 0) {
            memcpy(whole + i * ENTRY, GP(img + (u32)order[i] * ENTRY), ENTRY);
            if ((u32)order[i] == vchosen)
                wchosen = (u32)i;
        } else
            memcpy(whole + i * ENTRY, hidden[-1 - order[i]].bytes, ENTRY);
    }
    /* the save's chosen car, unless the player chose another since */
    if (hidden_chosen >= 0 && (int)vchosen == chosen_vis)
        for (int i = 0; i < total; i++)
            if (order[i] == -1 - hidden_chosen)
                wchosen = (u32)i;
    wr32(g + G_ENTRIES, GA(whole));
    wr32(g + G_COUNT, (u32)total);
    wr32(g + G_CHOSEN, wchosen);
    wr32(pl + P_VEC + 4, vb);          /* an empty list: the writer copies nothing over the whole one */
    writing = 1, wr_missing = 0;
    __real_F_266820(c);
    writing = 0;
    wr32(pl + P_VEC + 4, ve);
    wr32(g + G_ENTRIES, img);
    wr32(g + G_COUNT, nvis);
    wr32(g + G_CHOSEN, vchosen);
    free(whole);
    static int told;
    if (!told++)
        rt_log("[cars] profile saved with the %d car%s left out back in the garage%s (thread %lu)\n", nhidden,
               nhidden == 1 ? "" : "s", hidden_chosen >= 0 && (int)vchosen == chosen_vis ? ", the save's chosen car too"
                                                                                          : "",
               GetCurrentThreadId());
    return 1;
}

/* the profile's writer (player, file) */
void __wrap_F_266820(CPU *c)
{
    if (!enabled() || (!nkept && chosen_now < 0)) {
        __real_F_266820(c);
        return;
    }
    if (nhidden && c->r[0] == player() && write_whole(c, c->r[0]))
        return;
    /* nothing hidden (more than MAX_KEPT, or the hiding not done): the entries without a row are still in the
       garage, written with their kept ids; the save's chosen car while it is written */
    u32 g = ld_garage, rows = table_rows();
    u32 gb = g ? rd32(g + G_ENTRIES) : 0, gn = g ? rd32(g + G_COUNT) : 0;
    int was_lost = chosen_was >= 0 && ((u32)chosen_was >= gn || rd32(gb + (u32)chosen_was * ENTRY + E_ROW) >= rows);
    int restore = g && chosen_now >= 0 && rd32(g + G_CHOSEN) == (u32)chosen_now && was_lost;
    if (restore)
        wr32(g + G_CHOSEN, (u32)chosen_was);
    writing = 1, wr_missing = 0;
    __real_F_266820(c);
    writing = 0;
    if (restore)
        wr32(g + G_CHOSEN, (u32)chosen_now);
    static int told;
    if (!told++ && nkept)
        rt_log("[cars] profile saved with the %d car%s the table lacks%s\n", nkept, nkept == 1 ? "" : "s",
               restore ? ", the save's chosen car too" : "");
}
