/* keymap.c -- KEY_MAP (port addition): a remappable keyboard and gamepad keys page in Options > Controls
   when Touch Screen = Off.

   The default driving keys (pcinput.c): A/D or Left/Right steer, W/Up gas, S/Down brake, Shift / Ctrl gears,
   V look back, C camera, Esc pause; the pad laid out like Gran Turismo 7's, PAD_GT7.  Here each
   of the nine actions has two keyboard keys and one pad input, those same ones by default, which the
   Keyboard / Gamepad page (Options > Controls with Touch Screen Off, pcctl.c) changes: a click on a
   cell, then the key or the pad's button / trigger / stick direction.  While a cell waits:
     Esc         leaves it as it was (a key cell or a pad cell)
     Backspace,  Delete: no key in that slot (a keyboard cell)
     a mouse click anywhere, or 8 s: leaves it as it was.
   A key or a pad input already used by another action is taken from it (one action per key).
   The menus' own keys stay fixed, as they were: W A S D / the arrows, Enter, Esc (KEY_MENU); the pad's
   D-pad, left stick, Cross (A), Circle (B) (PAD_MENU_SNAP).  In the menus, a pad action on one of those
   pad buttons does nothing (Cross selects there; before, only gear up on Cross was held back).
   The keys are saved per profile in <home>\Library\Preferences\pc_keys.txt (no file: the defaults).
   Key names are SDL's (layout keys: W is the key that types W). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "features.h"

static const char *const action_id[KM_ACTIONS] = {
    "steer_left", "steer_right", "gas", "brake", "gear_up", "gear_down", "look_back", "camera", "pause"};
static const char *const action_title[KM_ACTIONS] = {
    "Steer Left", "Steer Right", "Gas", "Brake", "Gear Up", "Gear Down", "Look Back", "Camera", "Pause"};

typedef struct {
    const char *id, *xbox, *ps;     /* file name; label on an Xbox / PlayStation pad */
    int button;                     /* SDL_GamepadButton, or -1: an axis */
    int axis, sign;                 /* the axis and its half (+1, -1; triggers +1) */
} PadInput;

static const PadInput pad_in[] = {
    {"a", "A", "Cross", SDL_GAMEPAD_BUTTON_SOUTH, 0, 0},
    {"b", "B", "Circle", SDL_GAMEPAD_BUTTON_EAST, 0, 0},
    {"x", "X", "Square", SDL_GAMEPAD_BUTTON_WEST, 0, 0},
    {"y", "Y", "Triangle", SDL_GAMEPAD_BUTTON_NORTH, 0, 0},
    {"leftshoulder", "LB", "L1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0, 0},
    {"rightshoulder", "RB", "R1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0, 0},
    {"lefttrigger", "LT", "L2", -1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1},
    {"righttrigger", "RT", "R2", -1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1},
    {"leftstick", "LS Click", "L3", SDL_GAMEPAD_BUTTON_LEFT_STICK, 0, 0},
    {"rightstick", "RS Click", "R3", SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0, 0},
    {"back", "View", "Share", SDL_GAMEPAD_BUTTON_BACK, 0, 0},
    {"start", "Menu", "Options", SDL_GAMEPAD_BUTTON_START, 0, 0},
    {"dpup", "D-Pad Up", "D-Pad Up", SDL_GAMEPAD_BUTTON_DPAD_UP, 0, 0},
    {"dpdown", "D-Pad Down", "D-Pad Down", SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0, 0},
    {"dpleft", "D-Pad Left", "D-Pad Left", SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0, 0},
    {"dpright", "D-Pad Right", "D-Pad Right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0, 0},
    {"leftx-", "LS Left", "LS Left", -1, SDL_GAMEPAD_AXIS_LEFTX, -1},
    {"leftx+", "LS Right", "LS Right", -1, SDL_GAMEPAD_AXIS_LEFTX, 1},
    {"lefty-", "LS Up", "LS Up", -1, SDL_GAMEPAD_AXIS_LEFTY, -1},
    {"lefty+", "LS Down", "LS Down", -1, SDL_GAMEPAD_AXIS_LEFTY, 1},
    {"rightx-", "RS Left", "RS Left", -1, SDL_GAMEPAD_AXIS_RIGHTX, -1},
    {"rightx+", "RS Right", "RS Right", -1, SDL_GAMEPAD_AXIS_RIGHTX, 1},
    {"righty-", "RS Up", "RS Up", -1, SDL_GAMEPAD_AXIS_RIGHTY, -1},
    {"righty+", "RS Down", "RS Down", -1, SDL_GAMEPAD_AXIS_RIGHTY, 1},
};
#define PAD_INPUTS ((int)(sizeof pad_in / sizeof pad_in[0]))
enum { P_A, P_B, P_X, P_Y, P_LB, P_RB, P_LT, P_RT, P_LS, P_RS, P_BACK, P_START, P_UP, P_DOWN, P_LEFT, P_RIGHT,
       P_LX_NEG, P_LX_POS, P_LY_NEG, P_LY_POS };

static const SDL_Keycode key_default[KM_ACTIONS][KM_KEYS] = {
    {SDLK_A, SDLK_LEFT}, {SDLK_D, SDLK_RIGHT}, {SDLK_W, SDLK_UP}, {SDLK_S, SDLK_DOWN},
    {SDLK_LSHIFT, SDLK_RSHIFT}, {SDLK_LCTRL, SDLK_RCTRL}, {SDLK_V, 0}, {SDLK_C, 0}, {SDLK_ESCAPE, 0}};
static const int pad_default[KM_ACTIONS] = {P_LX_NEG, P_LX_POS, P_RT, P_LT, P_A, P_X, P_LB, P_RB, P_START};

static SDL_Keycode keys[KM_ACTIONS][KM_KEYS];
static int pads[KM_ACTIONS];            /* pad_in index, -1 none */
static unsigned char held[KM_ACTIONS][KM_KEYS];
static unsigned taps;                   /* bit per action: a key went down since keymap_key_tap looked */
static int version = 1;                 /* changes when a cell's text does (pcctl.c refreshes them) */
static int ps_pad;                      /* the pad in use is a PlayStation one (its labels) */
static int has_pad;                     /* a pad is plugged in (notouch.c names its Cross / A) */

static struct {
    int active, action, col;
    int armed;                          /* pad: everything was let go since the click */
    Uint64 until;
} cap;
static int pad_quiet;                   /* after a pad capture: the pad stays out of the menus until let go */

/* the file */
static const char *file_path(char *buf, size_t n)
{
    char g[512];
    snprintf(g, sizeof g, "%s/Library/Preferences/pc_keys.txt", gt_guest_home);
    return gt_host_path(g, buf, n);
}

static void defaults(void)
{
    memcpy(keys, key_default, sizeof keys);
    memcpy(pads, pad_default, sizeof pads);
}

static void save(void)
{
    char p[MAX_PATH * 2];
    FILE *f = fopen(file_path(p, sizeof p), "w");
    if (!f) {
        rt_log("[keymap] cannot write %s\n", p);
        return;
    }
    fprintf(f, "# KEY_MAP (Options > Controls > Keyboard / Gamepad, with Touch Screen Off): the driving keys\n"
               "# key.<action>.1 / .2 = SDL key name (blank: none), pad.<action> = a, b, x, y, leftshoulder,\n"
               "# rightshoulder, lefttrigger, righttrigger, leftstick, rightstick, back, start, dpup .. dpright,\n"
               "# leftx- leftx+ lefty- lefty+ rightx- .. righty+ (stick halves), blank: none\n");
    for (int a = 0; a < KM_ACTIONS; a++) {
        for (int s = 0; s < KM_KEYS; s++)
            fprintf(f, "key.%s.%d=%s\n", action_id[a], s + 1, keys[a][s] ? SDL_GetKeyName(keys[a][s]) : "");
        fprintf(f, "pad.%s=%s\n", action_id[a], pads[a] >= 0 ? pad_in[pads[a]].id : "");
    }
    fclose(f);
}

static void load(void)
{
    defaults();
    char p[MAX_PATH * 2], line[160];
    FILE *f = fopen(file_path(p, sizeof p), "r");
    if (!f)
        return;
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        const char *val = eq + 1;
        for (int a = 0; a < KM_ACTIONS; a++) {
            char k[48];
            for (int s = 0; s < KM_KEYS; s++) {
                snprintf(k, sizeof k, "key.%s.%d=", action_id[a], s + 1);
                if (!strncmp(line, k, strlen(k))) {
                    SDL_Keycode code = *val ? SDL_GetKeyFromName(val) : 0;
                    keys[a][s] = code == SDLK_UNKNOWN ? 0 : code;
                    n++;
                }
            }
            snprintf(k, sizeof k, "pad.%s=", action_id[a]);
            if (!strncmp(line, k, strlen(k))) {
                pads[a] = -1;
                for (int i = 0; i < PAD_INPUTS; i++)
                    if (!strcmp(val, pad_in[i].id))
                        pads[a] = i;
                n++;
            }
        }
    }
    fclose(f);
    rt_log("[keymap] %d keys from %s\n", n, p);
}

void keymap_init(void)
{
    load();
}

/* the game's input */
int keymap_key(SDL_Keycode k, int down, int repeat)
{
    /* waiting: a press is taken here; a let-go still goes on (Enter, which clicked the cell, held) */
    if (cap.active && down) {
        if (repeat)
            return 1;
        int a = cap.action, col = cap.col;
        cap.active = 0;
        version++;
        if (k == SDLK_ESCAPE) {
            rt_log("[keymap] %s: left as it was\n", action_title[a]);
            return 1;
        }
        if (col >= KM_KEYS)              /* a pad cell: only Esc (above) from the keyboard */
            return 1;
        if (k == SDLK_BACKSPACE || k == SDLK_DELETE)
            k = 0;
        else
            for (int b = 0; b < KM_ACTIONS; b++)     /* one action per key: taken from another */
                for (int s = 0; s < KM_KEYS; s++)
                    if (keys[b][s] == k && (b != a || s != col)) {
                        rt_log("[keymap] %s key %d: %s taken off\n", action_title[b], s + 1, SDL_GetKeyName(k));
                        keys[b][s] = 0;
                    }
        keys[a][col] = k;
        rt_log("[keymap] %s key %d = %s\n", action_title[a], col + 1, k ? SDL_GetKeyName(k) : "(none)");
        memset(held, 0, sizeof held);
        save();
        return 1;
    }
    if (!k)
        return 0;
    for (int a = 0; a < KM_ACTIONS; a++)
        for (int s = 0; s < KM_KEYS; s++)
            if (keys[a][s] == k) {
                held[a][s] = (unsigned char)down;
                if (down && !repeat)
                    taps |= 1u << a;
            }
    return 0;
}

int keymap_key_held(int a) { return held[a][0] || held[a][1]; }

int keymap_key_tap(int a)
{
    int t = (taps >> a) & 1;
    taps &= ~(1u << a);
    return t;
}

static float pad_value(SDL_Gamepad *p, int i)
{
    if (!p || i < 0)
        return 0;
    const PadInput *in = &pad_in[i];
    if (in->button >= 0)
        return SDL_GetGamepadButton(p, (SDL_GamepadButton)in->button) ? 1.0f : 0.0f;
    float v = SDL_GetGamepadAxis(p, (SDL_GamepadAxis)in->axis) / 32767.0f * (float)in->sign;
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

float keymap_pad(SDL_Gamepad *p, int a) { return pad_value(p, pads[a]); }

/* the pad's menu buttons (PAD_MENU_SNAP): D-pad, left stick, Cross, Circle */
int keymap_pad_menu_input(int a)
{
    int i = pads[a];
    return i == P_A || i == P_B || (i >= P_UP && i <= P_LY_POS);
}

/* a driving action is on a half of the right stick (RACE_LOOK, racelook.c, then leaves the stick alone) */
int keymap_right_stick_bound(void)
{
    for (int a = 0; a < KM_ACTIONS; a++)
        if (pads[a] >= 0 && !strncmp(pad_in[pads[a]].id, "right", 5) && pad_in[pads[a]].button < 0 &&
            pad_in[pads[a]].axis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
            return 1;
    return 0;
}

/* once a frame from ui_pump: 1 = the pad is busy here (a cell waits, or the press that set it is still
   held), the menus do not take it */
int keymap_frame(SDL_Gamepad *p)
{
    has_pad = p != NULL;
    if (p) {
        SDL_GamepadType t = SDL_GetGamepadType(p);
        int ps = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
        if (ps != ps_pad) {
            ps_pad = ps;
            version++;
        }
    }
    float most = 0;
    int first = -1;
    for (int i = 0; p && i < PAD_INPUTS; i++) {
        float v = pad_value(p, i);
        if (v > most)
            most = v;
        if (v > 0.6f && first < 0)
            first = i;
    }
    if (cap.active && SDL_GetTicks() > cap.until) {
        cap.active = 0;
        version++;
        rt_log("[keymap] %s: no key in 8 s, left as it was\n", action_title[cap.action]);
    }
    if (!cap.active || cap.col < KM_KEYS) {
        if (pad_quiet && most < 0.3f)
            pad_quiet = 0;
        return pad_quiet || (cap.active && p);
    }
    if (!cap.armed) {                    /* the Cross that clicked the cell may still be down */
        cap.armed = most < 0.3f;
        return 1;
    }
    if (first < 0)
        return 1;
    int a = cap.action;
    for (int b = 0; b < KM_ACTIONS; b++)
        if (b != a && pads[b] == first) {
            rt_log("[keymap] %s: pad %s taken off\n", action_title[b], pad_in[first].id);
            pads[b] = -1;
        }
    pads[a] = first;
    cap.active = 0;
    pad_quiet = 1;
    version++;
    rt_log("[keymap] %s pad = %s\n", action_title[a], pad_in[first].id);
    save();
    return 1;
}

/* the Keyboard / Gamepad page (pcctl.c) */
const char *keymap_title(int a) { return action_title[a]; }

const char *keymap_cell(int a, int col)
{
    if (cap.active && cap.action == a && cap.col == col)
        return col < KM_KEYS ? "Press a key" : "Press a button";
    if (col < KM_KEYS)
        return keys[a][col] ? SDL_GetKeyName(keys[a][col]) : "-";
    return pads[a] >= 0 ? (ps_pad ? pad_in[pads[a]].ps : pad_in[pads[a]].xbox) : "-";
}

void keymap_capture(int a, int col)
{
    cap.active = 1, cap.action = a, cap.col = col, cap.armed = 0;
    cap.until = SDL_GetTicks() + 8000;
    version++;
    rt_log("[keymap] %s %s %d: waiting for a %s\n", action_title[a], col < KM_KEYS ? "key" : "pad", col + 1,
           col < KM_KEYS ? "key" : "pad input");
}

void keymap_cancel(void)
{
    if (cap.active) {
        cap.active = 0;
        version++;
        rt_log("[keymap] %s: left as it was\n", action_title[cap.action]);
    }
}

int keymap_capturing(int *a, int *col)
{
    if (cap.active && a)
        *a = cap.action, *col = cap.col;
    return cap.active;
}

void keymap_defaults(void)
{
    cap.active = 0;
    defaults();
    memset(held, 0, sizeof held);
    version++;
    save();
    rt_log("[keymap] the default keys again\n");
}

int keymap_version(void) { return version; }

/* NO_TOUCH_TEXT (notouch.c): 0 no pad, 1 an Xbox-like pad (A), 2 a PlayStation pad (Cross) */
int keymap_pad_kind(void) { return has_pad ? 1 + ps_pad : 0; }
