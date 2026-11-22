/* dataimp.c -- data the game imports from the system libraries (rt_data_import, loader.c).

   A data import's slot gets the address of the thing itself: a class_t, a struct constant, or a
   variable cell (an NSString * const, a FILE *).  Names the host has nothing for are left to the
   loader's guard page. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi.h"
#include "loader.h"
#include "ns.h"
#include "objc.h"

u32 objc_data_import(const char *name);
u32 cxx_data_import(const char *name);

/* A constant NSString with the CFString constant layout {isa, flags, chars, length}. */
static u32 gt_const_nsstring(const char *s)
{
    u32 *o = calloc(4, 4);
    o[0] = objc_class_addr(objc_host_class("__NSCFConstantString"));
    o[1] = 0x7C8;
    o[2] = GA(strdup(s));
    o[3] = (u32)strlen(s);
    return GA(o);
}

static u32 cell(u32 v)
{
    u32 *p = calloc(1, 4);
    *p = v;
    return GA(p);
}

static u32 zeros(u32 bytes) { return GA(calloc(1, bytes)); }

/* Darwin's ctype table (_DefaultRuneLocale), for the game's inline isalpha() & co */

enum {
    CT_A = 0x100, CT_C = 0x200, CT_D = 0x400, CT_G = 0x800, CT_L = 0x1000, CT_P = 0x2000, CT_S = 0x4000,
    CT_U = 0x8000, CT_X = 0x10000, CT_B = 0x20000, CT_R = 0x40000, CT_SW1 = 0x40000000
};

static u8 rune_locale[3124 + 64];

u32 gt_runetype(int ch)
{
    if (ch < 0 || ch > 255)
        return 0;
    u32 v;
    memcpy(&v, rune_locale + 52 + 4 * ch, 4);
    return v;
}

static u32 make_rune_locale(void)
{
    memcpy(rune_locale, "RuneMagA", 8);
    memcpy(rune_locale + 8, "NONE", 4);
    for (int ch = 0; ch < 256; ch++) {
        u32 t = 0;
        int lo = ch, up = ch;
        if (ch < 128) {
            if (isalpha(ch)) t |= CT_A;
            if (iscntrl(ch)) t |= CT_C;
            if (isdigit(ch)) t |= CT_D | (u32)(ch - '0');
            if (isgraph(ch)) t |= CT_G;
            if (islower(ch)) t |= CT_L;
            if (ispunct(ch)) t |= CT_P;
            if (isspace(ch)) t |= CT_S;
            if (isupper(ch)) t |= CT_U;
            if (isxdigit(ch)) t |= CT_X | (u32)(isdigit(ch) ? ch - '0' : (tolower(ch) - 'a' + 10));
            if (ch == ' ' || ch == '\t') t |= CT_B;
            if (isprint(ch)) t |= CT_R | CT_SW1;
            lo = tolower(ch);
            up = toupper(ch);
        }
        memcpy(rune_locale + 52 + 4 * ch, &t, 4);
        memcpy(rune_locale + 1076 + 4 * ch, &lo, 4);
        memcpy(rune_locale + 2100 + 4 * ch, &up, 4);
    }
    return GA(rune_locale);
}

/* the table */

static const char *const string_prefixes[] = {"_NS", "_UI", "_MP", "_AD", "_k", NULL};

u32 rt_data_import(const char *name)
{
    u32 v = objc_data_import(name);
    if (v || (v = cxx_data_import(name)))
        return v;
    if (!strcmp(name, "___stderrp"))
        return cell(gt_file_handle(stderr));
    if (!strcmp(name, "___stdoutp"))
        return cell(gt_file_handle(stdout));
    if (!strcmp(name, "___stdinp"))
        return cell(gt_file_handle(stdin));
    if (!strcmp(name, "__DefaultRuneLocale"))
        return make_rune_locale();
    if (!strcmp(name, "_CGRectZero") || !strcmp(name, "_CGPointZero") || !strcmp(name, "_CGSizeZero") ||
        !strcmp(name, "_UIEdgeInsetsZero"))
        return zeros(16);
    if (!strcmp(name, "_CGRectNull")) {
        static GtRect null_rect = {__builtin_inff(), __builtin_inff(), 0, 0};
        return GA(&null_rect);
    }
    if (!strcmp(name, "_CGAffineTransformIdentity")) {
        static GtAffine id = {1, 0, 0, 1, 0, 0};
        return GA(&id);
    }
    if (!strcmp(name, "_UIWindowLevelAlert"))
        return cell(f2bits(2000.0f));
    if (!strcmp(name, "_UIWindowLevelStatusBar"))
        return cell(f2bits(1000.0f));
    if (!strcmp(name, "_UIBackgroundTaskInvalid"))
        return cell(0);
    if (!strcmp(name, "_kCLLocationAccuracyBest")) {
        static double best = -1.0;
        return GA(&best);
    }
    if (!strcmp(name, "_kABPersonFirstNameProperty")) return cell(0);
    if (!strcmp(name, "_kABPersonLastNameProperty")) return cell(1);
    if (!strcmp(name, "_kABPersonPhoneProperty")) return cell(3);
    if (!strcmp(name, "_kABPersonEmailProperty")) return cell(4);
    if (!strcmp(name, "_kCFAllocatorDefault"))
        return cell(0);
    if (!strcmp(name, "_kCFTypeArrayCallBacks"))
        return GA(ns_cf_callbacks[0]);
    if (!strcmp(name, "_kCFTypeDictionaryKeyCallBacks") || !strcmp(name, "_kCFCopyStringDictionaryKeyCallBacks"))
        return GA(ns_cf_callbacks[1]);
    if (!strcmp(name, "_kCFTypeDictionaryValueCallBacks"))
        return GA(ns_cf_callbacks[2]);
    if (!strcmp(name, "_kCFBooleanTrue") || !strcmp(name, "_kCFBooleanFalse"))
        return cell(ns_num_bool(name[11] == 'T'));
    if (!strcmp(name, "_kCFNumberNaN"))
        return cell(objc_retain(ns_num_d(__builtin_nan(""))));
    if (!strcmp(name, "__dispatch_main_q"))
        return zeros(64);
    /* NSString * const: the value is the name (a few have other values on iOS; the game only
       compares and passes them back) */
    for (int i = 0; string_prefixes[i]; i++)
        if (!strncmp(name, string_prefixes[i], strlen(string_prefixes[i]))) {
            const char *s = name + 1;
            if (s[0] == 'k' && s[1] >= 'A' && s[1] <= 'Z')
                s++;
            return cell(gt_const_nsstring(s));
        }
    return 0;
}
