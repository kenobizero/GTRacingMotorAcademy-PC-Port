/* notouch.c -- NO_TOUCH_TEXT (port addition): with Touch Screen Off, the texts that say touch or tap name the
   keyboard, mouse or pad action instead.

   With Touch Screen Off (PC_CONTROLS, pcctl.c) the game is played with the keyboard, the mouse or a pad,
   but its texts still say "Touch To Continue", "Please tap the play button.", "- Touch the car to
   continue -" and so on.  This is not a fault of the original (it was an iPad game) nor of the port: the
   texts were written for a touch screen.  In this mode they now name what the PC has instead:
       touch / tap to continue        ->  press Enter to continue (with a pad: press A, or press Cross;
                                          pcinput.c lets Cross / A start the race whatever gear up is on)
       touch / tap the X, anywhere,   ->  click the X, click anywhere, click to ... (the pointer: the
       here, to, license ...              mouse, or the pad's)
       any other touch / tap          ->  press (touch and hold -> press and hold)
       the tutorial's touch controls  ->  "Steer with A / D.", "Shift gears with Left Shift / Left Ctrl.",
                                          "Hold V to see behind the car." (the keys of KEY_MAP, keymap.c;
                                          with a pad, its inputs); "Please press S (brake)." /
                                          "Please press W (accelerate)." for the Brake / Accelerate icons
   keeping the word's case (TOUCH -> PRESS, Touch -> Press) and the -ing / -ed endings.  First made
   "select the X"; "Touch the car to select a new one" then read "Select the car to select ...", so the
   pointer's word, click, took its place.  Not changed: the
   noun ("Touch Screen", the touch controls, the iPod touch), words inside markup (<font ...>), and the
   other languages (English only; the text is left as it is).

   Where: every text a gameswf text field shows goes through edit_text_character's set text, 0x4fde00
   (this, const tu_string *text, html): the SWF's own default texts when a field is made, the game's
   strings (SP_TOUCH_TO_CONTINUE ...) and ActionScript's.  Its instruction 0x4fde1c is replaced at
   recompile time (recomp/codegen.py HOST_INSNS) by gt_text_filter(text), which hands back the text or a
   changed copy; 0x4fde00 then copies it into the field as usual (and does nothing when the field already
   shows it, so a text the game sets every frame is not laid out again).  A field made while Touch Screen
   was On keeps its text until the game sets it again; the menus and the race make theirs as they open.
   GT_NO_TOUCH_TEXT=0: the texts as they were. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"

#include "features.h"

#define SLOTS 8
#define TEXT_MAX 4096

/* a tu_string as gameswf keeps a long one: byte 0 = 0xff, +4 size (with the 0), +8 capacity, +0xc the
   characters, +0x10 the hash (low 24 bits all ones = not worked out yet) and flags */
static struct slot {
    u8 str[0x18];
    char buf[TEXT_MAX];
} slots[SLOTS] __attribute__((aligned(16)));
static volatile LONG next_slot;
static int on = -1;
static int logged;

static const char *word_case(const char *w, int n, const char *rep, char *out, size_t cap)
{
    int upper = n > 1 && isupper((unsigned char)w[0]) && isupper((unsigned char)w[1]);
    int cap1 = isupper((unsigned char)w[0]);
    size_t i = 0;
    for (; rep[i] && i + 1 < cap; i++) {
        char ch = rep[i];
        out[i] = upper ? (char)toupper((unsigned char)ch) : (i == 0 && cap1) ? (char)toupper((unsigned char)ch) : ch;
    }
    out[i] = 0;
    return out;
}

/* the word that starts at s (letters only), lower case, in w; its length */
static int word_at(const char *s, char *w, int cap)
{
    int n = 0;
    while (isalpha((unsigned char)s[n])) {
        if (n + 1 < cap)
            w[n] = (char)tolower((unsigned char)s[n]);
        n++;
    }
    w[n < cap ? n : cap - 1] = 0;
    return n;
}

/* the next word after position p (spaces between), outside markup; "" at the end */
static const char *next_word(const char *p, char *w, int cap)
{
    while (*p == ' ')
        p++;
    word_at(p, w, cap);
    return p;
}

/* touch, touching, touched, tap, tapping, tapped: the base (0 touch, 1 tap) and the ending */
static int touch_word(const char *w, const char **end)
{
    static const struct { const char *w, *end; int base; } t[] = {
        {"touch", "", 0}, {"touching", "ing", 0}, {"touched", "ed", 0}, {"touches", "es", 0},
        {"tap", "", 1}, {"tapping", "ing", 1}, {"tapped", "ed", 1}, {"taps", "s", 1},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(w, t[i].w)) {
            *end = t[i].end;
            return t[i].base;
        }
    return -1;
}

static const char *ending(const char *verb, const char *end, char *out, size_t cap)
{
    /* press -> pressing / pressed / presses; select -> selecting / selected / selects; click -> ... */
    if (!*end)
        snprintf(out, cap, "%s", verb);
    else if (!strcmp(end, "es") || !strcmp(end, "s"))
        snprintf(out, cap, "%s%s", verb, !strcmp(verb, "press") ? "es" : "s");
    else
        snprintf(out, cap, "%s%s", verb, end);
    return out;
}

/* the key (or with a pad, the pad input) an action is on: KEY_MAP's first one (keymap.c) */
static const char *key_of(int a)
{
    const char *k = keymap_cell(a, keymap_pad_kind() ? KM_KEYS : 0);
    if (!strcmp(k, "-"))
        k = keymap_cell(a, 1);
    return k;
}

/* the tutorial's sentences on the touch controls name the keys instead (case as written in the game) */
static int phrases(const char *s, char *out, size_t cap)
{
    static const struct { const char *from; int a1, a2; const char *fmt; } t[] = {
        {"by touching the screen", KM_LEFT, KM_RIGHT, "with %s / %s"},
        {"by touching icons on the screen", KM_GEAR_UP, KM_GEAR_DOWN, "with %s / %s"},
        {"Touch the top of the screen and hold", KM_REAR, -1, "Hold %s"},
        {"tap the Brake icon", KM_BRAKE, -1, "press %s (brake)"},
        {"tap the Accelerate icon", KM_GAS, -1, "press %s (accelerate)"},
    };
    int changed = 0;
    snprintf(out, cap, "%s", s);
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        char *at = strstr(out, t[i].from);
        if (!at)
            continue;
        char rep[96], rest[TEXT_MAX];
        snprintf(rep, sizeof rep, t[i].fmt, key_of(t[i].a1), t[i].a2 >= 0 ? key_of(t[i].a2) : "");
        snprintf(rest, sizeof rest, "%s", at + strlen(t[i].from));
        if ((size_t)(at - out) + strlen(rep) + strlen(rest) + 1 > cap)
            continue;
        snprintf(at, cap - (size_t)(at - out), "%s%s", rep, rest);
        changed = 1;
    }
    return changed;
}

/* the text with its touch words changed; 0 when there were none */
static int change(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    int changed = 0, tag = 0;
    char prev[16] = "";
    const char *pad = keymap_pad_kind() == 2 ? "Cross" : keymap_pad_kind() == 1 ? "A" : "Enter";
    for (const char *p = s; *p;) {
        if (*p == '<')
            tag = 1;
        if (tag || !isalpha((unsigned char)*p)) {
            if (*p == '>')
                tag = 0;
            if (o + 1 >= cap)
                return 0;
            out[o++] = *p++;
            continue;
        }
        char w[16];
        int n = word_at(p, w, sizeof w);
        const char *end;
        int base = (p == s || !isalpha((unsigned char)p[-1])) ? touch_word(w, &end) : -1;
        const char *rep = NULL;
        char extra[24] = "";
        if (base >= 0 && strcmp(prev, "ipod")) {
            char n1[16], n2[16];
            const char *q = next_word(p + n, n1, sizeof n1);
            next_word(q + strlen(n1), n2, sizeof n2);
            if (!strcmp(n1, "screen") || !strcmp(n1, "screens") || !strcmp(n1, "control") ||
                !strcmp(n1, "controls") || !strcmp(n1, "zone") || !strcmp(n1, "area") ||
                !strcmp(n1, "sensitivity") || !strcmp(n1, "pad") || !strcmp(n1, "button"))
                rep = NULL;                                     /* the noun: Touch Screen ... */
            else if (!strcmp(n1, "to") && !strcmp(n2, "continue") && !*end) {
                rep = "press";
                snprintf(extra, sizeof extra, " %s", pad);
            } else if (!strcmp(n1, "anywhere") || !strcmp(n1, "to") || !strcmp(n1, "here") ||
                       !strcmp(n1, "the") || !strcmp(n1, "a") || !strcmp(n1, "an") || !strcmp(n1, "license") ||
                       !strcmp(n1, "licence") || !strcmp(n1, "on") || !strcmp(n1, "your") || !strcmp(n1, "any") ||
                       !strcmp(n1, "it") || !strcmp(n1, "them") || !strcmp(n1, "icons") || !strcmp(n1, "this"))
                rep = "click";
            else
                rep = "press";                                  /* touch and hold -> press and hold */
        }
        if (rep) {
            char verb[24], cased[48];
            ending(rep, end, verb, sizeof verb);
            word_case(p, n, verb, cased, sizeof cased);
            int upper = n > 1 && isupper((unsigned char)p[0]) && isupper((unsigned char)p[1]);
            if (*extra && upper)
                for (char *e = extra; *e; e++)
                    *e = (char)toupper((unsigned char)*e);
            size_t l = strlen(cased), le = strlen(extra);
            if (o + l + le + 1 >= cap)
                return 0;
            memcpy(out + o, cased, l);
            memcpy(out + o + l, extra, le);
            o += l + le;
            changed = 1;
        } else {
            if (o + n + 1 >= cap)
                return 0;
            memcpy(out + o, p, n);
            o += n;
        }
        snprintf(prev, sizeof prev, "%s", w);
        p += n;
    }
    out[o] = 0;
    return changed;
}

/* from the generated 0x4fde00 (codegen HOST_INSNS 0x4fde1c): the text a field is given, or our copy */
u32 gt_text_filter(u32 text)
{
    if (on < 0) {
        const char *e = getenv("GT_NO_TOUCH_TEXT");
        on = !(e && *e == '0');
        rt_log("[notouch] %s\n", on ? "with Touch Screen Off the texts say press / click and name the keys"
                                    : "off (GT_NO_TOUCH_TEXT=0): the texts as they were");
    }
    if (!on || !text || !pcctl_keys_mode())
        return text;
    const u8 *t = (const u8 *)GP(text);
    const char *s = t[0] == 0xff ? (const char *)GP(rd32(text + 0xc)) : (const char *)t + 1;
    if (!s)
        return text;
    /* quick look: no "tou" / "tap" in any case, nothing to do */
    const char *q = s;
    for (; *q; q++) {
        char a = (char)(*q | 0x20);
        if ((a == 't') && (((q[1] | 0x20) == 'o' && (q[2] | 0x20) == 'u') || ((q[1] | 0x20) == 'a' && (q[2] | 0x20) == 'p')))
            break;
    }
    if (!*q)
        return text;
    struct slot *sl = &slots[(unsigned)InterlockedIncrement(&next_slot) % SLOTS];
    char tmp[TEXT_MAX];
    int by_phrase = phrases(s, tmp, sizeof tmp);
    if (!change(tmp, sl->buf, sizeof sl->buf) && !by_phrase)
        return text;
    if (!by_phrase && !strcmp(sl->buf, s))
        return text;
    u32 len = (u32)strlen(sl->buf) + 1, hash = rd32(text + 0x10);
    sl->str[0] = 0xff;
    memcpy(sl->str + 4, &len, 4);
    memcpy(sl->str + 8, &len, 4);
    u32 b = GA(sl->buf);
    memcpy(sl->str + 0xc, &b, 4);
    hash = (hash & 0xff000000u) | 0x00ffffffu;
    memcpy(sl->str + 0x10, &hash, 4);
    /* each text once in the log (the game sets some every frame) */
    u32 h = 2166136261u;
    for (const char *k = s; *k; k++)
        h = (h ^ (u8)*k) * 16777619u;
    static u32 seen[256];
    int known = 0;
    for (int i = 0; i < logged && i < 256; i++)
        if (seen[i] == h)
            known = 1;
    if (!known && logged < 256) {
        seen[logged++] = h;
        rt_log("[notouch] \"%.200s\" -> \"%.200s\"\n", s, sl->buf);
    }
    return GA(sl->str);
}
