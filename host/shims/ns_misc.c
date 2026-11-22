/* ns_misc.c -- paths and files, NSBundle, NSFileManager, NSUserDefaults, NSProcessInfo, notifications,
   NSException, NSError, NSURL, NSLocale / NSTimeZone and the Foundation C functions.  Time and threads (NSDate,
   timers, the run loop, NSThread, locks) are ns_runloop.c's.

   The game sees iOS-like paths: its home is /var/mobile/Applications/GTRACING (Documents,
   Library, tmp) with the .app inside it, as on the device.  gt_host_path maps them to the
   host: the .app folder, and a "home" folder next to the program. */
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "plat.h"

#include "hmap.h"
#include "ns.h"
#include "ns_internal.h"

const char *gt_guest_home = "/var/mobile/Applications/GTRACING";
const char *gt_guest_bundle = "/var/mobile/Applications/GTRACING/" GT_GAME_NAME ".app";
static char host_home[MAX_PATH], host_bundle[MAX_PATH], host_patch[MAX_PATH];

/* Add-ons (port addition, HYUNDAI_CARS; an add-on folder as in the Gangstar port): each folder in add-on\ next to
   the program holds bundle files, made by a
   tool from a game the player has (tools\hyundai_addon.py: add-on\hyundai).  They are in the bundle's listing
   (fm_contents) and read from there (gt_host_path), so the game finds them as its own: it makes its car table
   from every .ba01 in the bundle and reads every .ba5 / .ba4 pack.  A bundle file of the same name is read
   from the add-on instead (build\host\patch still first).  GT_ADDONS=0: none. */
typedef struct {
    char name[96];
    char path[MAX_PATH * 2];
} AddonFile;
static AddonFile *addon_files;
static int addon_count;

/* FILE_INDEX (port addition, optimisation pass 3): an in-memory hash index of the files in patch\ and
   add-on\, built at start: every bundle file the game opened was first looked
   for in build\host\patch with GetFileAttributesA (a failed call to the file system for all but the six patched
   menus), and its name compared with each add-on file in turn (_stricmp over about 150 names: premium-audio alone
   has 121).  Both folders are now listed once at start into hash maps of lower-case names (Windows names ignore
   case), so a lookup asks no one.  A file put into patch\ or add-on\ while the game runs is seen at the next
   start, as add-ons already were. */
static MapStr patch_idx, addon_idx;        /* "\sub\name" under patch\ -> 1; add-on file name -> its index + 1 */

static void lower_ascii(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s += 'a' - 'A';
}
static int idx_has(MapStr *m, const char *key, u32 *v)
{
    char k[MAX_PATH * 2];
    u32 dummy;
    if (!m->n || strlen(key) >= sizeof k)
        return 0;
    strcpy(k, key);
    lower_ascii(k);
    return ms_get(m, k, v ? v : &dummy);
}
static void idx_put(MapStr *m, const char *key, u32 v)
{
    char *k = _strdup(key);
    if (!k)
        return;
    lower_ascii(k);
    ms_put(m, k, v);
}
/* the files under dir (rel = their path below the folder indexed, "" at the top), into patch_idx */
static int patch_index_dir(const char *dir, const char *rel, int depth)
{
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s%s" GT_SEP_S "*", dir, rel);
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int n = 0;
    do {
        if (!strcmp(d.cFileName, ".") || !strcmp(d.cFileName, ".."))
            continue;
        char sub[MAX_PATH * 2];
        snprintf(sub, sizeof sub, "%s" GT_SEP_S "%s", rel, d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8)
                n += patch_index_dir(dir, sub, depth + 1);
        } else {
            idx_put(&patch_idx, sub, 1);
            n++;
        }
    } while (FindNextFileA(h, &d));
    FindClose(h);
    return n;
}

static void addons_init(const char *exe_dir)
{
    const char *e = getenv("GT_ADDONS");
    if (e && *e == '0') {
        rt_log("[addon] off (GT_ADDONS=0)\n");
        return;
    }
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s" GT_SEP_S "add-on" GT_SEP_S "*", exe_dir);
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pat, &d);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == '.')
            continue;
        char dir[MAX_PATH * 2], fpat[MAX_PATH * 2 + 4];
        if (snprintf(dir, sizeof dir, "%s" GT_SEP_S "add-on" GT_SEP_S "%s", exe_dir, d.cFileName) >= (int)sizeof dir)
            continue;                    /* a folder name too long for a Windows path */
        snprintf(fpat, sizeof fpat, "%s" GT_SEP_S "*", dir);
        WIN32_FIND_DATAA f;
        HANDLE g = FindFirstFileA(fpat, &f);
        if (g == INVALID_HANDLE_VALUE)
            continue;
        int n = 0;
        do {
            if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || strlen(f.cFileName) >= sizeof addon_files->name ||
                !strchr(f.cFileName, '.') || !_stricmp(strrchr(f.cFileName, '.'), ".txt"))
                continue;
            if (strlen(dir) + 1 + strlen(f.cFileName) >= sizeof addon_files->path)
                continue;
            if (idx_has(&addon_idx, f.cFileName, NULL)) {
                rt_log("[addon] %s" GT_SEP_S "%s: another add-on has that file, not used\n", d.cFileName, f.cFileName);
                continue;
            }
            addon_files = realloc(addon_files, sizeof *addon_files * (addon_count + 1));
            AddonFile *a = &addon_files[addon_count++];
            snprintf(a->name, sizeof a->name, "%s", f.cFileName);
            snprintf(a->path, sizeof a->path, "%s" GT_SEP_S "%s", dir, f.cFileName);
            idx_put(&addon_idx, a->name, (u32)addon_count);      /* index + 1 */
            n++;
        } while (FindNextFileA(g, &f));
        FindClose(g);
        rt_log("[addon] %s: %d files\n", d.cFileName, n);
    } while (FindNextFileA(h, &d));
    FindClose(h);
}

/* a file at the top of the bundle (host path) -> the add-on's copy, or NULL */
static const char *addon_file(const char *host)
{
    size_t bl = strlen(host_bundle);
    if (!addon_count || !bl || _strnicmp(host, host_bundle, bl) || host[bl] != GT_SEP || strchr(host + bl + 1, GT_SEP))
        return NULL;
    u32 i;
    return idx_has(&addon_idx, host + bl + 1, &i) ? addon_files[i - 1].path : NULL;
}
u32 ns_main_thread_id;

/* paths, files */

static int prefix_of(const char *p, const char *pre)
{
    size_t n = strlen(pre);
    return !strncmp(p, pre, n) && (p[n] == 0 || p[n] == '/');
}

/* The guest's working directory (chdir/getcwd): iOS starts apps in "/".  Relative paths are joined
   to it here, so the host's own working directory never matters. */
static char guest_cwd[1024] = "/";

/* absolute, with "." and ".." resolved and no doubled slashes */
static void guest_abs(const char *g, char *out, size_t n)
{
    char tmp[2048];
    if (g[0] == '/')
        snprintf(tmp, sizeof tmp, "%s", g);
    else
        snprintf(tmp, sizeof tmp, "%s/%s", guest_cwd, g);
    size_t o = 0;
    out[0] = 0;
    for (char *s = tmp; *s;) {
        while (*s == '/')
            s++;
        char *e = s;
        while (*e && *e != '/')
            e++;
        size_t len = (size_t)(e - s);
        if (!len)
            break;
        if (len == 1 && s[0] == '.') {
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--;
        } else if (o + len + 2 < n) {
            out[o++] = '/';
            memcpy(out + o, s, len);
            o += len;
        }
        out[o] = 0;
        s = e;
    }
    if (!o)
        snprintf(out, n, "/");
}

u32 h_chdir(u32 p)
{
    char g[1024], host[1024];
    guest_abs(GP(p), g, sizeof g);
    DWORD a = GetFileAttributesA(gt_host_path(g, host, sizeof host));
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        rt_log("[ns] chdir %s: no such directory\n", g);
        rt_thread()->errno_cell = 2;
        return (u32)-1;
    }
    snprintf(guest_cwd, sizeof guest_cwd, "%s", g);
    return 0;
}
u32 h_getcwd(u32 buf, u32 n)
{
    if (!buf || strlen(guest_cwd) + 1 > n)
        return 0;
    strcpy(GP(buf), guest_cwd);
    return buf;
}

const char *gt_host_path(const char *g0, char *buf, size_t n)
{
    char ga[1024];
    guest_abs(g0, ga, sizeof ga);
    const char *g = ga;
    if (prefix_of(g, gt_guest_bundle))
        snprintf(buf, n, "%s%s", host_bundle, g + strlen(gt_guest_bundle));
    else if (prefix_of(g, gt_guest_home))
        snprintf(buf, n, "%s%s", host_home, g + strlen(gt_guest_home));
    else
        snprintf(buf, n, "%s", g);
    for (char *s = buf; *s; s++)
        if (*s == '/')
            *s = GT_SEP;
    /* a bundle file the build patched (build\host\patch: the menu SWF for PC_CONTROLS, pcctl.c) is read
       from there instead; the game mostly opens bundle files by their host path (NSBundle gives it) */
    size_t bl = strlen(host_bundle);
    if (host_patch[0] && bl && !_strnicmp(buf, host_bundle, bl) && buf[bl] == GT_SEP &&
        idx_has(&patch_idx, buf + bl, NULL)) {              /* FILE_INDEX: listed at start, no file system call */
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s%s", host_patch, buf + bl);
        static int logs;
        if (logs++ < 20)
            rt_log("[ns] %s read from %s\n", buf, p);
        snprintf(buf, n, "%s", p);
        return buf;
    }
    const char *a = addon_file(buf);
    if (a)
        snprintf(buf, n, "%s", a);
    return buf;
}

static void mkdir_host(const char *p) { CreateDirectoryA(p, NULL); }

void gt_paths_init(const char *bundle, const char *exe_dir)
{
    snprintf(host_bundle, sizeof host_bundle, "%s", bundle);
    snprintf(host_home, sizeof host_home, "%s" GT_SEP_S "home", exe_dir);
    const char *pc = getenv("GT_PC_CONTROLS");          /* =0: the original menu SWF (pcctl.c) */
    if (!(pc && *pc == '0'))
        snprintf(host_patch, sizeof host_patch, "%s" GT_SEP_S "patch", exe_dir);
    if (getenv("GT_HOME") && *getenv("GT_HOME"))     /* testing: a copy of the save, not the player's own */
        snprintf(host_home, sizeof host_home, "%s", getenv("GT_HOME"));
    if (host_patch[0]) {
        int n = patch_index_dir(host_patch, "", 0);
        rt_log("[ns] patch folder: %d files indexed\n", n);
    }
    addons_init(exe_dir);
    static const char *const sub[] = {"", GT_SEP_S "Documents", GT_SEP_S "Library", GT_SEP_S "Library" GT_SEP_S "Caches",
                                      GT_SEP_S "Library" GT_SEP_S "Preferences", GT_SEP_S "tmp", NULL};
    for (int i = 0; sub[i]; i++) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s%s", host_home, sub[i]);
        mkdir_host(p);
    }
    ns_main_thread_id = GetCurrentThreadId();
    rt_log("[ns] home %s (guest %s)\n", host_home, gt_guest_home);
}

u8 *gt_read_file(const char *g, u32 *len)
{
    char p[MAX_PATH * 2];
    FILE *f = fopen(gt_host_path(g, p, sizeof p), "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc((size_t)(n > 0 ? n : 0) + 2);
    if (!d || (n > 0 && fread(d, 1, (size_t)n, f) != (size_t)n)) {
        fclose(f);
        free(d);
        return NULL;
    }
    fclose(f);
    d[n > 0 ? n : 0] = 0;
    d[(n > 0 ? n : 0) + 1] = 0;
    if (len)
        *len = (u32)(n > 0 ? n : 0);
    return d;
}

int gt_write_file(const char *g, const void *data, u32 n)
{
    char p[MAX_PATH * 2];
    FILE *f = fopen(gt_host_path(g, p, sizeof p), "wb");
    if (!f) {
        rt_log("[ns] cannot write %s (%s)\n", g, p);
        return 0;
    }
    int ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

static DWORD attrs(const char *g)
{
    char p[MAX_PATH * 2];
    return GetFileAttributesA(gt_host_path(g, p, sizeof p));
}

u32 h_NSSearchPathForDirectoriesInDomains(u32 dir, u32 mask, u32 expand)
{
    (void)mask;
    const char *sub;
    switch (dir) {
    case 9: sub = "/Documents"; break;
    case 13: sub = "/Library/Caches"; break;
    case 14: sub = "/Library/Application Support"; break;
    case 1: sub = "/Applications"; break;
    default: sub = "/Library"; break;
    }
    id s = expand ? ns_stringf("%s%s", gt_guest_home, sub) : ns_stringf("~%s", sub);
    return objc_autorelease(ns_array_new(&s, 1));
}
u32 h_NSHomeDirectory(void) { return ns_string(gt_guest_home); }

/* small layouts, allocation */

static void alloc16(CPU *c) { ret_i(c, ns_alloc(SELF, 16)); }
static void alloc32(CPU *c) { ret_i(c, ns_alloc(SELF, 32)); }
static void alloc64(CPU *c) { ret_i(c, ns_alloc(SELF, 64)); }

/* the one object of a shared-instance class (made on the first ask, kept) */
id ns_singleton(id *slot, const char *cls, u32 bytes)
{
    if (!*slot) {
        *slot = ns_alloc(ns_class(cls), bytes);
        objc_retain(*slot);
    }
    return *slot;
}

/* NSBundle */

typedef struct { u32 isa; u32 path, info, strings; } Bundle;
#define BU(o) ((Bundle *)GP(o))
static id main_bundle;

static id bundle_info(void)
{
    Bundle *b = BU(main_bundle);
    if (!b->info) {
        char p[1024];
        snprintf(p, sizeof p, "%s/Info.plist", gt_guest_bundle);
        b->info = plist_read_file(p);
        if (!b->info)
            b->info = ns_dict_new();
    }
    return b->info;
}

static void bu_main(CPU *c)
{
    if (!main_bundle) {
        main_bundle = ns_alloc(ns_class("NSBundle"), sizeof(Bundle));
        objc_retain(main_bundle);
        BU(main_bundle)->path = objc_retain(ns_string(gt_guest_bundle));
    }
    ret_i(c, main_bundle);
}
static void bu_path(CPU *c) { ret_i(c, BU(SELF)->path); }
static void bu_exe(CPU *c) { ret_i(c, ns_stringf("%s/%s", gt_guest_bundle, GT_GAME_NAME)); }
static void bu_info(CPU *c) { ret_i(c, bundle_info()); }
static void bu_infoKey(CPU *c) { ret_i(c, ns_dict_get(bundle_info(), ARG(0))); }
static void bu_identifier(CPU *c) { ret_i(c, ns_dict_get(bundle_info(), ns_string("CFBundleIdentifier"))); }

static int exists(const char *g) { return attrs(g) != INVALID_FILE_ATTRIBUTES; }

/* name[.type] in dir, then in en.lproj (or English.lproj) */
static id find_resource(id name, id type, id dir)
{
    const char *n = ns_utf8(name), *t = type ? ns_utf8(type) : "", *d = dir ? ns_utf8(dir) : "";
    if (!*n && !*t)
        return 0;
    static const char *const loc[] = {"", "en.lproj/", "English.lproj/", NULL};
    for (int i = 0; loc[i]; i++) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s%s%s%s%s%s", gt_guest_bundle, d, *d ? "/" : "", loc[i], n, *t ? "." : "", t);
        if (exists(p))
            return ns_string(p);
    }
    return 0;
}
static void bu_pathForResource(CPU *c)
{
    int in_dir = c->r[1] == sel_get("pathForResource:ofType:inDirectory:");
    id r = find_resource(ARG(0), ARG(1), in_dir ? ARG(2) : 0);
    if (!r && rt_trace)
        rt_log("[ns] pathForResource %s.%s: not in the bundle\n", ns_utf8(ARG(0)), ARG(1) ? ns_utf8(ARG(1)) : "");
    ret_i(c, r);
}
static void bu_URLForResource(CPU *c)
{
    id p = find_resource(ARG(0), ARG(1), 0);
    ret_i(c, p ? msg1(ns_class("NSURL"), "fileURLWithPath:", p) : 0);
}

/* .strings: binary plist, XML plist or the old "key" = "value"; text (UTF-16 or UTF-8) */
static void parse_strings(id dict, const char *s)
{
    const char *p = s;
    char key[1024], val[4096];
    for (;;) {
        while (*p && *p != '"') {
            if (p[0] == '/' && p[1] == '*') {
                const char *e = strstr(p + 2, "*/");
                p = e ? e + 2 : p + strlen(p);
            } else if (p[0] == '/' && p[1] == '/') {
                while (*p && *p != '\n')
                    p++;
            } else {
                p++;
            }
        }
        if (!*p)
            break;
        for (int part = 0; part < 2; part++) {
            char *o = part ? val : key;
            size_t cap = part ? sizeof val : sizeof key, k = 0;
            if (*p != '"')
                return;
            p++;
            while (*p && *p != '"') {
                char ch = *p++;
                if (ch == '\\' && *p) {
                    ch = *p++;
                    ch = ch == 'n' ? '\n' : ch == 't' ? '\t' : ch == 'r' ? '\r' : ch;
                }
                if (k + 1 < cap)
                    o[k++] = ch;
            }
            o[k] = 0;
            if (*p)
                p++;
            if (!part) {
                while (*p && *p != '=' && *p != ';')
                    p++;
                if (*p != '=')
                    break;
                p++;
                while (*p && *p != '"')
                    p++;
            }
        }
        id k = ns_string(key), v = ns_string(val);
        ns_dict_set(dict, k, v);
        while (*p && *p != ';')
            p++;
    }
}
static id strings_table(id table)
{
    Bundle *b = BU(main_bundle);
    if (!b->strings)
        b->strings = ns_dict_new();
    id name = table ? table : ns_string("Localizable");
    id t = ns_dict_get(b->strings, name);
    if (t)
        return t;
    id d = 0;
    id path = find_resource(name, ns_string("strings"), 0);
    if (path) {
        u32 n;
        u8 *p = gt_read_file(ns_utf8(path), &n);
        if (p) {
            d = plist_parse(p, n);
            if (d && !ns_is_dict(d)) {
                objc_release(d);
                d = 0;
            }
            if (!d) {
                d = ns_dict_new();
                id text = msg2(msg0(ns_class("NSString"), "alloc"), "initWithData:encoding:",
                               objc_autorelease(ns_data_new(p, n)), (p[0] == 0xFF || p[0] == 0xFE) ? 10 : 4);
                parse_strings(d, ns_utf8(text));
                objc_release(text);
            }
            free(p);
        }
    }
    if (!d)
        d = ns_dict_new();
    ns_dict_set(b->strings, name, d);
    objc_release(d);
    return d;
}
static void bu_localized(CPU *c)
{
    id v = ns_dict_get(strings_table(ARG(2)), ARG(0));
    if (!v)
        v = ARG(1) && ns_utf8_len(ARG(1)) ? ARG(1) : ARG(0);
    ret_i(c, v);
}
static void bu_preferredLocalizations(CPU *c)
{
    id s = ns_string("en");
    ret_i(c, objc_autorelease(ns_array_new(&s, 1)));
}

/* NSFileManager */

static id file_manager;
static void fm_default(CPU *c) { ret_i(c, ns_singleton(&file_manager, "NSFileManager", 8)); }

static void fm_exists(CPU *c)
{
    DWORD a = attrs(ns_utf8(ARG(0)));
    if (c->r[1] == sel_get("fileExistsAtPath:isDirectory:") && ARG(1))
        *(u8 *)GP(ARG(1)) = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    ret_i(c, a != INVALID_FILE_ATTRIBUTES);
}
static int mkdirs(const char *g, int parents)
{
    char p[MAX_PATH * 2];
    gt_host_path(g, p, sizeof p);
    if (!parents)
        return CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    for (char *s = p + (GT_SEP == '/' ? 1 : 3); *s; s++)    /* past "/" or "C:\\" */
        if (*s == GT_SEP) {
            *s = 0;
            CreateDirectoryA(p, NULL);
            *s = GT_SEP;
        }
    CreateDirectoryA(p, NULL);
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static void fm_createDir(CPU *c)
{
    /* createDirectoryAtPath:withIntermediateDirectories:attributes:error: / createDirectoryAtPath:attributes: */
    int full = c->r[1] == sel_get("createDirectoryAtPath:withIntermediateDirectories:attributes:error:");
    int ok = mkdirs(ns_utf8(ARG(0)), full && (u8)ARG(1));
    if (!ok && full && ARG(3))
        wr32(ARG(3), 0);
    ret_i(c, ok);
}
static int remove_tree(const char *hp)
{
    DWORD a = GetFileAttributesA(hp);
    if (a == INVALID_FILE_ATTRIBUTES)
        return 0;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY))
        return DeleteFileA(hp) != 0;
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s" GT_SEP_S "*", hp);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            char sub[MAX_PATH * 2];
            snprintf(sub, sizeof sub, "%s" GT_SEP_S "%s", hp, fd.cFileName);
            remove_tree(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(hp) != 0;
}
static void fm_remove(CPU *c)
{
    char p[MAX_PATH * 2];
    int ok = remove_tree(gt_host_path(ns_utf8(ARG(0)), p, sizeof p));
    if (!ok && ARG(1) && c->r[1] == sel_get("removeItemAtPath:error:"))
        wr32(ARG(1), 0);
    ret_i(c, ok);
}
static void fm_contents(CPU *c)
{
    char p[MAX_PATH * 2], pat[MAX_PATH * 2 + 4];
    gt_host_path(ns_utf8(ARG(0)), p, sizeof p);
    snprintf(pat, sizeof pat, "%s" GT_SEP_S "*", p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (c->r[1] == sel_get("contentsOfDirectoryAtPath:error:") && ARG(1))
            wr32(ARG(1), 0);
        ret_i(c, 0);
        return;
    }
    id a = ns_array_new(NULL, 0);
    char *seen = calloc(addon_count + 1, 1);
    size_t bl = strlen(host_bundle);
    int bundle = bl && !_strnicmp(p, host_bundle, bl) && (!p[bl] || (p[bl] == GT_SEP && !p[bl + 1]));
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
            ns_array_add(a, ns_string(fd.cFileName));
            u32 i;
            if (bundle && seen && idx_has(&addon_idx, fd.cFileName, &i))
                seen[i - 1] = 1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    /* the add-ons' files, as if in the bundle (a name the bundle has is listed once) */
    for (int i = 0; bundle && seen && i < addon_count; i++)
        if (!seen[i])
            ns_array_add(a, ns_string(addon_files[i].name));
    free(seen);
    ret_i(c, objc_autorelease(a));
}
static double filetime_ref(FILETIME ft)
{
    u64 v = (u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return (double)v / 1e7 - 11644473600.0 - SINCE_1970;
}
static void fm_attributes(CPU *c)
{
    char p[MAX_PATH * 2];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(gt_host_path(ns_utf8(ARG(0)), p, sizeof p), GetFileExInfoStandard, &fa)) {
        if (c->r[1] == sel_get("attributesOfItemAtPath:error:") && ARG(1))
            wr32(ARG(1), 0);
        ret_i(c, 0);
        return;
    }
    id d = ns_dict_new();
    u64 size = (u64)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    ns_dict_set(d, ns_string("NSFileSize"), ns_num_ll((int64_t)size));
    ns_dict_set(d, ns_string("NSFileModificationDate"), ns_date(filetime_ref(fa.ftLastWriteTime)));
    ns_dict_set(d, ns_string("NSFileCreationDate"), ns_date(filetime_ref(fa.ftCreationTime)));
    ns_dict_set(d, ns_string("NSFileType"),
                ns_string(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? "NSFileTypeDirectory" : "NSFileTypeRegular"));
    ret_i(c, objc_autorelease(d));
}
static void fm_fsAttributes(CPU *c)
{
    ULARGE_INTEGER freeb, total, totalfree;
    char p[MAX_PATH * 2];
    gt_host_path(gt_guest_home, p, sizeof p);
    if (!GetDiskFreeSpaceExA(p, &freeb, &total, &totalfree))
        freeb.QuadPart = total.QuadPart = 8ull << 30;
    id d = ns_dict_new();
    ns_dict_set(d, ns_string("NSFileSystemSize"), ns_num_ll((int64_t)total.QuadPart));
    ns_dict_set(d, ns_string("NSFileSystemFreeSize"), ns_num_ll((int64_t)freeb.QuadPart));
    ret_i(c, objc_autorelease(d));
}
static void fm_copy(CPU *c)
{
    char a[MAX_PATH * 2], b[MAX_PATH * 2];
    int ok = CopyFileA(gt_host_path(ns_utf8(ARG(0)), a, sizeof a), gt_host_path(ns_utf8(ARG(1)), b, sizeof b), TRUE);
    if (!ok && ARG(2))
        wr32(ARG(2), 0);
    ret_i(c, ok != 0);
}
static void fm_move(CPU *c)
{
    char a[MAX_PATH * 2], b[MAX_PATH * 2];
    int ok = MoveFileA(gt_host_path(ns_utf8(ARG(0)), a, sizeof a), gt_host_path(ns_utf8(ARG(1)), b, sizeof b));
    if (!ok && ARG(2))
        wr32(ARG(2), 0);
    ret_i(c, ok != 0);
}
static void fm_createFile(CPU *c)
{
    u32 n = 0;
    const u8 *p = ARG(1) ? ns_data_bytes(ARG(1), &n) : (const u8 *)"";
    ret_i(c, gt_write_file(ns_utf8(ARG(0)), p, n));
}
static void fm_contentsAt(CPU *c)
{
    u32 n;
    u8 *p = gt_read_file(ns_utf8(ARG(0)), &n);
    if (!p) {
        ret_i(c, 0);
        return;
    }
    id d = ns_data_new(p, n);
    free(p);
    ret_i(c, objc_autorelease(d));
}
static void fm_chdir(CPU *c)
{
    ret_i(c, h_chdir(GA(ns_utf8(ARG(0)))) == 0);
}
static void fm_cwd(CPU *c) { ret_i(c, ns_string(guest_cwd)); }
static void fm_isReadable(CPU *c) { ret_i(c, exists(ns_utf8(ARG(0)))); }
static void fm_displayName(CPU *c) { ret_i(c, msg0(ARG(0), "lastPathComponent")); }

/* NSUserDefaults */

typedef struct { u32 isa; u32 values, registered, dirty; } Defaults;
#define DF(o) ((Defaults *)GP(o))
static id defaults_obj;
static char defaults_path[1024];

static void defaults_save(void)
{
    if (defaults_obj && DF(defaults_obj)->dirty) {
        plist_write_file(defaults_path, DF(defaults_obj)->values);
        DF(defaults_obj)->dirty = 0;
    }
}
static id defaults(void)
{
    if (!defaults_obj) {
        defaults_obj = ns_alloc(ns_class("NSUserDefaults"), sizeof(Defaults));
        objc_retain(defaults_obj);
        if (!main_bundle) {
            CPU tmp = {0};
            bu_main(&tmp);
        }
        id bid = ns_dict_get(bundle_info(), ns_string("CFBundleIdentifier"));
        snprintf(defaults_path, sizeof defaults_path, "%s/Library/Preferences/%s.plist", gt_guest_home,
                 bid ? ns_utf8(bid) : "game");
        id v = plist_read_file(defaults_path);
        if (v && !ns_is_dict(v)) {
            objc_release(v);
            v = 0;
        }
        DF(defaults_obj)->values = v ? v : ns_dict_new();
        DF(defaults_obj)->registered = ns_dict_new();
        /* NSGlobalDomain, as an iPad has it: the language list and the locale (GT_LANG=fr picks French) */
        const char *lang = getenv("GT_LANG") ? getenv("GT_LANG") : "en";
        id l = ns_string(lang);
        id langs = ns_array_new(&l, 1);
        ns_dict_set(DF(defaults_obj)->registered, ns_string("AppleLanguages"), langs);
        objc_release(langs);
        ns_dict_set(DF(defaults_obj)->registered, ns_string("AppleLocale"), ns_string("en_US"));
        atexit(defaults_save);
    }
    return defaults_obj;
}
static id df_get(id key)
{
    id v = ns_dict_get(DF(defaults())->values, key);
    return v ? v : ns_dict_get(DF(defaults())->registered, key);
}
static void df_set(id key, id v)
{
    if (v)
        ns_dict_set(DF(defaults())->values, key, v);
    else
        msg1(DF(defaults())->values, "removeObjectForKey:", key);
    DF(defaults())->dirty = 1;
}
static void ud_standard(CPU *c) { ret_i(c, defaults()); }
static void ud_objectForKey(CPU *c) { ret_i(c, df_get(ARG(0))); }
static void ud_typed(CPU *c)
{
    /* stringForKey: arrayForKey: dictionaryForKey: dataForKey: -- nil when of another kind */
    id v = df_get(ARG(0));
    const char *s = sel_name(c->r[1]);
    int ok = !v || (s[0] == 's' ? ns_is_string(v) : s[0] == 'a' ? ns_is_array(v) : s[0] == 'd' && s[1] == 'i' ? ns_is_dict(v) : ns_is_data(v));
    if (v && s[0] == 's' && ns_is_number(v)) {
        ret_i(c, msg0(v, "stringValue"));
        return;
    }
    ret_i(c, ok ? v : 0);
}
static void ud_int(CPU *c)
{
    id v = df_get(ARG(0));
    ret_i(c, v ? (ns_is_number(v) ? (u32)ns_num_int(v) : msg0(v, "intValue")) : 0);
}
static void ud_bool(CPU *c)
{
    id v = df_get(ARG(0));
    ret_i(c, v ? (ns_is_number(v) ? ns_num_int(v) != 0 : msg0(v, "boolValue") & 0xFF) : 0);
}
static double df_double(id key)
{
    id v = df_get(key);
    if (!v)
        return 0;
    if (ns_is_number(v))
        return ns_num_double(v);
    return strtod(ns_utf8(v), NULL);
}
static void ud_float(CPU *c) { ret_f(c, (float)df_double(ARG(0))); }
static void ud_double(CPU *c) { ret_d(c, df_double(ARG(0))); }
static void ud_setObject(CPU *c) { df_set(ARG(1), ARG(0)); }
static void ud_setInt(CPU *c) { df_set(ARG(1), ns_num_ll((int32_t)ARG(0))); }
static void ud_setBool(CPU *c) { df_set(ARG(1), ns_num_bool((u8)ARG(0))); }
static void ud_setFloat(CPU *c) { df_set(ARG(1), ns_num_d(ARGF(0))); }
static void ud_setDouble(CPU *c) { df_set(arg_word(c, 4), ns_num_d(ARGD(0))); }
static void ud_remove(CPU *c) { df_set(ARG(0), 0); }
static void ud_register(CPU *c) { msg1(DF(defaults())->registered, "addEntriesFromDictionary:", ARG(0)); }
static void ud_sync(CPU *c)
{
    defaults();
    defaults_save();
    ret_i(c, 1);
}
static void ud_dictRep(CPU *c)
{
    id d = ns_dict_new();
    msg1(d, "addEntriesFromDictionary:", DF(defaults())->registered);
    msg1(d, "addEntriesFromDictionary:", DF(defaults())->values);
    ret_i(c, objc_autorelease(d));
}

/* NSProcessInfo */

static id process_info;
static void pi_info(CPU *c) { ret_i(c, ns_singleton(&process_info, "NSProcessInfo", 8)); }
static void pi_name(CPU *c) { ret_i(c, ns_string(GT_GAME_NAME)); }
static void pi_guid(CPU *c)
{
    static u64 s;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    u8 b[16];
    for (int i = 0; i < 16; i++) {
        s += (u64)q.QuadPart + 0x9E3779B97F4A7C15ull;
        u64 z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        b[i] = (u8)(z ^ (z >> 31));
    }
    b[6] = (u8)((b[6] & 0x0F) | 0x40);
    b[8] = (u8)((b[8] & 0x3F) | 0x80);
    ret_i(c, ns_stringf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2],
                        b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]));
}
static void pi_arguments(CPU *c)
{
    id s = ns_stringf("%s/%s", gt_guest_bundle, GT_GAME_NAME);
    ret_i(c, objc_autorelease(ns_array_new(&s, 1)));
}
static void pi_environment(CPU *c) { ret_i(c, objc_autorelease(ns_dict_new())); }
static void pi_uptime(CPU *c) { ret_d(c, (double)GetTickCount64() / 1000.0); }
static void pi_pid(CPU *c) { ret_i(c, GetCurrentProcessId()); }
static void pi_memory(CPU *c) { ret_l(c, 512ull << 20); }
static void pi_cpus(CPU *c) { ret_i(c, 2); }
static void pi_osString(CPU *c) { ret_i(c, ns_string("Version 4.3.3 (Build 8J2)")); }

/* NSNotificationCenter */

typedef struct { u32 isa; u32 name, object, info; } Note;
#define NT(o) ((Note *)GP(o))
typedef struct Observer { u32 obs, sel, name, object; struct Observer *next; } Observer;
static Observer *observers;
static CRITICAL_SECTION note_lock;
static id note_center;
static u32 cls_note;

static id note_make(id name, id obj, id info)
{
    id n = ns_alloc(cls_note, sizeof(Note));
    NT(n)->name = objc_retain(name);
    NT(n)->object = objc_retain(obj);
    NT(n)->info = objc_retain(info);
    return objc_autorelease(n);
}
static void nt_with(CPU *c)
{
    int info = c->r[1] == sel_get("notificationWithName:object:userInfo:");
    ret_i(c, note_make(ARG(0), ARG(1), info ? ARG(2) : 0));
}
static void nt_init(CPU *c)
{
    NT(SELF)->name = objc_retain(ARG(0));
    NT(SELF)->object = objc_retain(ARG(1));
    NT(SELF)->info = objc_retain(ARG(2));
}
static void nt_name(CPU *c) { ret_i(c, NT(SELF)->name); }
static void nt_object(CPU *c) { ret_i(c, NT(SELF)->object); }
static void nt_info(CPU *c) { ret_i(c, NT(SELF)->info); }
static void nt_dealloc(CPU *c)
{
    objc_release(NT(SELF)->name);
    objc_release(NT(SELF)->object);
    objc_release(NT(SELF)->info);
    objc_free_instance(SELF);
}

static void nc_default(CPU *c) { ret_i(c, ns_singleton(&note_center, "NSNotificationCenter", 8)); }
static void nc_add(CPU *c)
{
    /* addObserver:selector:name:object: -- the observer is not retained (as in Cocoa) */
    Observer *o = calloc(1, sizeof *o);
    o->obs = ARG(0);
    o->sel = ARG(1);
    o->name = ARG(2) ? msg1(ARG(2), "copyWithZone:", 0) : 0;
    o->object = ARG(3);
    EnterCriticalSection(&note_lock);
    Observer **pp = &observers;
    while (*pp)
        pp = &(*pp)->next;
    *pp = o;
    LeaveCriticalSection(&note_lock);
}
static void nc_remove(CPU *c)
{
    /* removeObserver: / removeObserver:name:object: */
    int all = c->r[1] == sel_get("removeObserver:");
    id obs = ARG(0), name = all ? 0 : ARG(1), obj = all ? 0 : ARG(2);
    EnterCriticalSection(&note_lock);
    for (Observer **pp = &observers; *pp;) {
        Observer *o = *pp;
        if (o->obs == obs && (!name || (o->name && ns_equal(o->name, name))) && (!obj || o->object == obj)) {
            *pp = o->next;
            objc_release(o->name);
            free(o);
        } else {
            pp = &o->next;
        }
    }
    LeaveCriticalSection(&note_lock);
}
static void post(id note)
{
    id name = NT(note)->name, obj = NT(note)->object;
    u32 n = 0, cap = 16, *list = malloc(cap * 8);
    EnterCriticalSection(&note_lock);
    for (Observer *o = observers; o; o = o->next)
        if ((!o->name || ns_equal(o->name, name)) && (!o->object || o->object == obj)) {
            if (n == cap)
                list = realloc(list, (cap *= 2) * 8);
            list[2 * n] = o->obs;
            list[2 * n + 1] = o->sel;
            n++;
        }
    LeaveCriticalSection(&note_lock);
    if (rt_trace)
        rt_log("[ns] notification %s -> %u observers\n", ns_utf8(name), n);
    for (u32 i = 0; i < n; i++)
        msg1(list[2 * i], sel_name(list[2 * i + 1]), note);
    free(list);
}
static void nc_postName(CPU *c)
{
    int info = c->r[1] == sel_get("postNotificationName:object:userInfo:");
    post(note_make(ARG(0), ARG(1), info ? ARG(2) : 0));
}
static void nc_post(CPU *c) { post(ARG(0)); }
void ns_post_notification(const char *name, id object)
{
    post(note_make(ns_string(name), object, 0));
}

/* NSException, NSError */

void h_objc_exception_throw(CPU *c, u32 exc);

typedef struct { u32 isa; u32 name, reason, info; } Exc;
#define EX(o) ((Exc *)GP(o))

static id exc_make(u32 cls, id name, id reason, id info)
{
    id e = ns_alloc(cls, sizeof(Exc));
    EX(e)->name = objc_retain(name);
    EX(e)->reason = objc_retain(reason);
    EX(e)->info = objc_retain(info);
    return e;
}
static void ex_with(CPU *c) { ret_i(c, objc_autorelease(exc_make(SELF, ARG(0), ARG(1), ARG(2)))); }
static void ex_init(CPU *c)
{
    EX(SELF)->name = objc_retain(ARG(0));
    EX(SELF)->reason = objc_retain(ARG(1));
    EX(SELF)->info = objc_retain(ARG(2));
}
static void ex_raiseFormat(CPU *c)
{
    /* +raise:format:, ... */
    u32 w[VA_WORDS];
    Buf b = {0};
    ns_format(&b, ns_utf8(ARG(1)), va_words(c, 4, w));
    id e = objc_autorelease(exc_make(SELF, ARG(0), ns_string(b.p ? b.p : ""), 0));
    buf_free(&b);
    rt_log("[ns] +[NSException raise:%s format:] %s\n", ns_utf8(ARG(0)), ns_utf8(EX(e)->reason));
    h_objc_exception_throw(c, e);
}
static void ex_raise(CPU *c)
{
    rt_log("[ns] -[NSException raise] %s: %s\n", ns_utf8(EX(SELF)->name), ns_utf8(EX(SELF)->reason));
    h_objc_exception_throw(c, SELF);
}
static void ex_name(CPU *c) { ret_i(c, EX(SELF)->name); }
static void ex_reason(CPU *c) { ret_i(c, EX(SELF)->reason); }
static void ex_info(CPU *c) { ret_i(c, EX(SELF)->info); }
static void ex_stack(CPU *c) { ret_i(c, objc_autorelease(ns_array_new(NULL, 0))); }
static void ex_description(CPU *c) { ret_i(c, EX(SELF)->reason ? EX(SELF)->reason : EX(SELF)->name); }
static void ex_dealloc(CPU *c)
{
    objc_release(EX(SELF)->name);
    objc_release(EX(SELF)->reason);
    objc_release(EX(SELF)->info);
    objc_free_instance(SELF);
}

/* NSError {isa, domain, code, userInfo} (game subclasses were compiled against 20 bytes) */
static void er_with(CPU *c)
{
    id e = ns_alloc(SELF, 16);
    W(e, 1) = objc_retain(ARG(0));
    W(e, 2) = ARG(1);
    W(e, 3) = objc_retain(ARG(2));
    ret_i(c, objc_autorelease(e));
}
static void er_init(CPU *c)
{
    set_copy(SELF, 1, ARG(0));
    W(SELF, 2) = ARG(1);
    set_obj(SELF, 3, ARG(2));
}
static void er_domain(CPU *c) { ret_i(c, W(SELF, 1)); }
static void er_code(CPU *c) { ret_i(c, W(SELF, 2)); }
static void er_info(CPU *c) { ret_i(c, W(SELF, 3)); }
static void er_localized(CPU *c)
{
    id d = W(SELF, 3) ? ns_dict_get(W(SELF, 3), ns_string("NSLocalizedDescription")) : 0;
    ret_i(c, d ? d : ns_stringf("The operation couldn't be completed. (%s error %d.)", ns_utf8(W(SELF, 1)), (int)W(SELF, 2)));
}
static void er_reason(CPU *c)
{
    ret_i(c, W(SELF, 3) ? ns_dict_get(W(SELF, 3), ns_string("NSLocalizedFailureReason")) : 0);
}
static void er_dealloc(CPU *c)
{
    objc_release(W(SELF, 1));
    objc_release(W(SELF, 3));
    objc_free_instance(SELF);
}
id ns_error(const char *domain, int code)
{
    id e = ns_alloc(ns_class("NSError"), 16);
    W(e, 1) = objc_retain(ns_string(domain));
    W(e, 2) = (u32)code;
    return objc_autorelease(e);
}

/* NSURL */

typedef struct { u32 isa; u32 str, file; } Url;
#define UR(o) ((Url *)GP(o))
static u32 cls_url;

static id url_make(u32 cls, id s, int file)
{
    id u = ns_alloc(cls ? cls : cls_url, sizeof(Url));
    UR(u)->str = objc_retain(s);
    UR(u)->file = (u32)file;
    return u;
}
static void ur_withString(CPU *c)
{
    if (!ARG(0)) {
        ret_i(c, 0);
        return;
    }
    const char *s = ns_utf8(ARG(0));
    ret_i(c, objc_autorelease(url_make(SELF, ARG(0), !strncmp(s, "file:", 5))));
}
static void ur_fileURL(CPU *c)
{
    id p = ARG(0);
    if (!p) {
        ret_i(c, 0);
        return;
    }
    ret_i(c, objc_autorelease(url_make(SELF, ns_stringf("file://localhost%s", ns_utf8(p)), 1)));
}
static void ur_initWithString(CPU *c)
{
    set_copy(SELF, 1, ARG(0));
    UR(SELF)->file = ARG(0) && !strncmp(ns_utf8(ARG(0)), "file:", 5);
}
static void ur_initFile(CPU *c)
{
    set_obj(SELF, 1, ns_stringf("file://localhost%s", ns_utf8(ARG(0))));
    UR(SELF)->file = 1;
}
static void ur_absolute(CPU *c) { ret_i(c, UR(SELF)->str); }
/* scheme://host[:port]/path?query#fragment */
static void url_parts(id u, const char **scheme_end, const char **host, const char **path)
{
    const char *s = ns_utf8(UR(u)->str), *p = strstr(s, "://");
    *scheme_end = p ? p : s;
    *host = p ? p + 3 : s;
    const char *q = *host;
    while (*q && *q != '/' && *q != '?' && *q != '#')
        q++;
    *path = q;
}
static void ur_scheme(CPU *c)
{
    const char *se, *h, *p, *s = ns_utf8(UR(SELF)->str);
    url_parts(SELF, &se, &h, &p);
    ret_i(c, se > s ? objc_autorelease(ns_string_new(s, (u32)(se - s))) : 0);
}
static void ur_host(CPU *c)
{
    const char *se, *h, *p;
    url_parts(SELF, &se, &h, &p);
    const char *colon = memchr(h, ':', (size_t)(p - h));
    ret_i(c, objc_autorelease(ns_string_new(h, (u32)((colon ? colon : p) - h))));
}
static void ur_path(CPU *c)
{
    const char *se, *h, *p;
    url_parts(SELF, &se, &h, &p);
    const char *e = p;
    while (*e && *e != '?' && *e != '#')
        e++;
    id raw = objc_autorelease(ns_string_new(p, (u32)(e - p)));
    ret_i(c, msg1(raw, "stringByReplacingPercentEscapesUsingEncoding:", 4));
}
static void ur_query(CPU *c)
{
    const char *s = ns_utf8(UR(SELF)->str), *q = strchr(s, '?');
    if (!q) {
        ret_i(c, 0);
        return;
    }
    const char *e = strchr(q, '#');
    ret_i(c, objc_autorelease(ns_string_new(q + 1, (u32)(e ? e - q - 1 : (long)strlen(q + 1)))));
}
static void ur_fragment(CPU *c)
{
    const char *q = strchr(ns_utf8(UR(SELF)->str), '#');
    ret_i(c, q ? ns_string(q + 1) : 0);
}
static void ur_resourceSpecifier(CPU *c)
{
    const char *s = ns_utf8(UR(SELF)->str), *q = strchr(s, ':');
    ret_i(c, ns_string(q ? q + 1 : s));
}
static void ur_port(CPU *c)
{
    const char *se, *h, *p;
    url_parts(SELF, &se, &h, &p);
    const char *colon = memchr(h, ':', (size_t)(p - h));
    ret_i(c, colon ? ns_num_ll(atoi(colon + 1)) : 0);
}
static void ur_isFile(CPU *c) { ret_i(c, UR(SELF)->file); }
static void ur_isEqual(CPU *c)
{
    id o = ARG(0);
    ret_i(c, o && objc_is_kind(o, cls_url) && ns_str_eq(UR(SELF)->str, UR(o)->str));
}
static void ur_hash(CPU *c) { ret_i(c, ns_hash(UR(SELF)->str)); }
static void ur_copy(CPU *c) { ret_i(c, objc_retain(SELF)); }
static void ur_dealloc(CPU *c)
{
    objc_release(UR(SELF)->str);
    objc_free_instance(SELF);
}

/* NSLocale, NSTimeZone */

static id locale_obj;
static void lc_current(CPU *c) { ret_i(c, ns_singleton(&locale_obj, "NSLocale", 8)); }
static void lc_init(CPU *c) { (void)c; }
static void lc_languages(CPU *c)
{
    id s = ns_string("en");
    ret_i(c, objc_autorelease(ns_array_new(&s, 1)));
}
static void lc_identifier(CPU *c) { ret_i(c, ns_string("en_US")); }
static void lc_objectForKey(CPU *c)
{
    const char *k = ns_utf8(ARG(0));
    if (strstr(k, "CountryCode"))
        ret_i(c, ns_string("US"));
    else if (strstr(k, "LanguageCode"))
        ret_i(c, ns_string("en"));
    else if (strstr(k, "CurrencyCode"))
        ret_i(c, ns_string("USD"));
    else if (strstr(k, "CurrencySymbol"))
        ret_i(c, ns_string("$"));
    else if (strstr(k, "DecimalSeparator"))
        ret_i(c, ns_string("."));
    else if (strstr(k, "Identifier"))
        ret_i(c, ns_string("en_US"));
    else
        ret_i(c, 0);
}
static void lc_displayName(CPU *c) { ret_i(c, ARG(1)); }

typedef struct { u32 isa; s32 secs; u32 name; } Tz;
static id tz_local;
static id tz_make(s32 secs, const char *name)
{
    id t = ns_alloc(ns_class("NSTimeZone"), sizeof(Tz));
    ((Tz *)GP(t))->secs = secs;
    ((Tz *)GP(t))->name = objc_retain(ns_string(name));
    return t;
}
static void tz_localZone(CPU *c)
{
    if (!tz_local) {
#ifdef _WIN32
        TIME_ZONE_INFORMATION z;
        DWORD r = GetTimeZoneInformation(&z);
        s32 bias = -(s32)(z.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? z.DaylightBias : 0)) * 60;
#else
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        s32 bias = (s32)lt.tm_gmtoff;
#endif
        tz_local = tz_make(bias, "Local");
        objc_retain(tz_local);
    }
    ret_i(c, tz_local);
}
static void tz_forSeconds(CPU *c) { ret_i(c, objc_autorelease(tz_make((s32)ARG(0), "GMT"))); }
static void tz_withName(CPU *c)
{
    const char *n = ns_utf8(ARG(0));
    ret_i(c, objc_autorelease(tz_make(0, *n ? n : "GMT")));
}
static void tz_secs(CPU *c) { ret_i(c, (u32)((Tz *)GP(SELF))->secs); }
static void tz_name(CPU *c) { ret_i(c, ((Tz *)GP(SELF))->name); }
static void tz_known(CPU *c)
{
    /* the start of iOS 5's list (sorted, as it gives them) */
    static const char *const names[] = {
        "Africa/Abidjan", "Africa/Accra", "Africa/Addis_Ababa", "Africa/Algiers", "Africa/Asmara", "Africa/Bamako",
        "Africa/Bangui", "Africa/Banjul", "Africa/Bissau", "Africa/Blantyre", "Africa/Brazzaville", "Africa/Bujumbura",
        "Africa/Cairo", "Africa/Casablanca", "Africa/Johannesburg", "Africa/Lagos", "Africa/Nairobi",
        "America/Anchorage", "America/Argentina/Buenos_Aires", "America/Bogota", "America/Caracas",
        "America/Chicago", "America/Denver", "America/Halifax", "America/Lima", "America/Los_Angeles",
        "America/Mexico_City", "America/New_York", "America/Phoenix", "America/Santiago", "America/Sao_Paulo",
        "America/St_Johns", "America/Toronto", "America/Vancouver", "Asia/Bangkok", "Asia/Dhaka", "Asia/Dubai",
        "Asia/Hong_Kong", "Asia/Jakarta", "Asia/Jerusalem", "Asia/Karachi", "Asia/Kathmandu", "Asia/Kolkata",
        "Asia/Kuala_Lumpur", "Asia/Manila", "Asia/Seoul", "Asia/Shanghai", "Asia/Singapore", "Asia/Taipei",
        "Asia/Tehran", "Asia/Tokyo", "Atlantic/Azores", "Atlantic/Reykjavik", "Australia/Adelaide",
        "Australia/Brisbane", "Australia/Melbourne", "Australia/Perth", "Australia/Sydney", "Europe/Amsterdam",
        "Europe/Athens", "Europe/Berlin", "Europe/Brussels", "Europe/Dublin", "Europe/Helsinki", "Europe/Istanbul",
        "Europe/Lisbon", "Europe/London", "Europe/Madrid", "Europe/Moscow", "Europe/Paris", "Europe/Prague",
        "Europe/Rome", "Europe/Stockholm", "Europe/Vienna", "Europe/Warsaw", "Europe/Zurich", "GMT",
        "Pacific/Auckland", "Pacific/Honolulu", NULL};
    id a = ns_array_new(NULL, 0);
    for (int i = 0; names[i]; i++)
        ns_array_add(a, ns_string(names[i]));
    ret_i(c, objc_autorelease(a));
}

/* C functions */

u32 h_NSClassFromString(u32 s) { return s ? objc_getClass_addr(ns_utf8(s)) : 0; }
u32 h_NSStringFromClass(u32 cls) { return cls ? ns_string(objc_class_name(cls)) : 0; }
u32 h_NSSelectorFromString(u32 s) { return s ? sel_get(ns_utf8(s)) : 0; }
u32 h_NSStringFromSelector(u32 sel) { return sel ? ns_string(sel_name(sel)) : 0; }
u32 h_NSStringFromCGRect(u32 x, u32 y, u32 w, u32 h)
{
    return ns_stringf("{{%g, %g}, {%g, %g}}", bits2f(x), bits2f(y), bits2f(w), bits2f(h));
}
u32 h_NSStringFromCGSize(u32 w, u32 h) { return ns_stringf("{%g, %g}", bits2f(w), bits2f(h)); }
static u32 uncaught_handler;
void h_NSSetUncaughtExceptionHandler(u32 fn) { uncaught_handler = fn; }
u32 h_NSAllocateObject(u32 cls, u32 extra, u32 zone)
{
    (void)zone;
    return objc_alloc_instance(cls, extra);
}

/* registration */

void ns_init_runloop(void);           /* ns_runloop.c */

static void ns_init_misc(void)
{
    ns_init_runloop();
    InitializeCriticalSection(&note_lock);
    cls_note = ns_class("NSNotification");
    cls_url = ns_class("NSURL");

    static const NsMethod bundle[] = {
        {"mainBundle", bu_main, 1}, {"bundlePath", bu_path, 0}, {"resourcePath", bu_path, 0},
        {"executablePath", bu_exe, 0}, {"infoDictionary", bu_info, 0}, {"localizedInfoDictionary", bu_info, 0},
        {"objectForInfoDictionaryKey:", bu_infoKey, 0}, {"bundleIdentifier", bu_identifier, 0},
        {"pathForResource:ofType:", bu_pathForResource, 0}, {"pathForResource:ofType:inDirectory:", bu_pathForResource, 0},
        {"URLForResource:withExtension:", bu_URLForResource, 0},
        {"localizedStringForKey:value:table:", bu_localized, 0},
        {"preferredLocalizations", bu_preferredLocalizations, 0}, {"localizations", bu_preferredLocalizations, 0},
        {"load", ret_yes, 0}, {"isLoaded", ret_yes, 0}, {NULL, NULL, 0}};
    ns_methods("NSBundle", bundle);

    static const NsMethod fm[] = {
        {"defaultManager", fm_default, 1}, {"fileExistsAtPath:", fm_exists, 0},
        {"fileExistsAtPath:isDirectory:", fm_exists, 0},
        {"createDirectoryAtPath:withIntermediateDirectories:attributes:error:", fm_createDir, 0},
        {"createDirectoryAtPath:attributes:", fm_createDir, 0}, {"removeItemAtPath:error:", fm_remove, 0},
        {"removeFileAtPath:handler:", fm_remove, 0}, {"contentsOfDirectoryAtPath:error:", fm_contents, 0},
        {"directoryContentsAtPath:", fm_contents, 0}, {"attributesOfItemAtPath:error:", fm_attributes, 0},
        {"fileAttributesAtPath:traverseLink:", fm_attributes, 0},
        {"attributesOfFileSystemForPath:error:", fm_fsAttributes, 0},
        {"fileSystemAttributesAtPath:", fm_fsAttributes, 0},
        {"copyItemAtPath:toPath:error:", fm_copy, 0}, {"moveItemAtPath:toPath:error:", fm_move, 0},
        {"createFileAtPath:contents:attributes:", fm_createFile, 0}, {"contentsAtPath:", fm_contentsAt, 0},
        {"changeCurrentDirectoryPath:", fm_chdir, 0}, {"currentDirectoryPath", fm_cwd, 0},
        {"isReadableFileAtPath:", fm_isReadable, 0}, {"isWritableFileAtPath:", fm_isReadable, 0},
        {"isDeletableFileAtPath:", fm_isReadable, 0}, {"displayNameAtPath:", fm_displayName, 0},
        {"setAttributes:ofItemAtPath:error:", ret_yes, 0},
        {"createSymbolicLinkAtPath:withDestinationPath:error:", ret_zero, 0}, {NULL, NULL, 0}};
    ns_methods("NSFileManager", fm);

    static const NsMethod ud[] = {
        {"standardUserDefaults", ud_standard, 1}, {"objectForKey:", ud_objectForKey, 0},
        {"stringForKey:", ud_typed, 0}, {"arrayForKey:", ud_typed, 0}, {"dictionaryForKey:", ud_typed, 0},
        {"dataForKey:", ud_typed, 0}, {"integerForKey:", ud_int, 0}, {"boolForKey:", ud_bool, 0},
        {"floatForKey:", ud_float, 0}, {"doubleForKey:", ud_double, 0}, {"setObject:forKey:", ud_setObject, 0},
        {"setValue:forKey:", ud_setObject, 0}, {"setInteger:forKey:", ud_setInt, 0}, {"setBool:forKey:", ud_setBool, 0},
        {"setFloat:forKey:", ud_setFloat, 0}, {"setDouble:forKey:", ud_setDouble, 0},
        {"removeObjectForKey:", ud_remove, 0}, {"registerDefaults:", ud_register, 0}, {"synchronize", ud_sync, 0},
        {"dictionaryRepresentation", ud_dictRep, 0}, {NULL, NULL, 0}};
    ns_methods("NSUserDefaults", ud);

    static const NsMethod pi[] = {
        {"processInfo", pi_info, 1}, {"processName", pi_name, 0}, {"globallyUniqueString", pi_guid, 0},
        {"arguments", pi_arguments, 0}, {"environment", pi_environment, 0}, {"systemUptime", pi_uptime, 0},
        {"processIdentifier", pi_pid, 0}, {"physicalMemory", pi_memory, 0}, {"processorCount", pi_cpus, 0},
        {"activeProcessorCount", pi_cpus, 0}, {"operatingSystemVersionString", pi_osString, 0}, {NULL, NULL, 0}};
    ns_methods("NSProcessInfo", pi);

    static const NsMethod note[] = {
        {"alloc", alloc16, 1}, {"notificationWithName:object:", nt_with, 1},
        {"notificationWithName:object:userInfo:", nt_with, 1}, {"initWithName:object:userInfo:", nt_init, 0},
        {"name", nt_name, 0}, {"object", nt_object, 0}, {"userInfo", nt_info, 0}, {"dealloc", nt_dealloc, 0},
        {NULL, NULL, 0}};
    ns_methods("NSNotification", note);
    static const NsMethod nc[] = {
        {"defaultCenter", nc_default, 1}, {"addObserver:selector:name:object:", nc_add, 0},
        {"removeObserver:", nc_remove, 0}, {"removeObserver:name:object:", nc_remove, 0},
        {"postNotificationName:object:", nc_postName, 0}, {"postNotificationName:object:userInfo:", nc_postName, 0},
        {"postNotification:", nc_post, 0}, {NULL, NULL, 0}};
    ns_methods("NSNotificationCenter", nc);

    static const NsMethod exc[] = {
        {"alloc", alloc16, 1}, {"allocWithZone:", alloc16, 1}, {"exceptionWithName:reason:userInfo:", ex_with, 1},
        {"raise:format:", ex_raiseFormat, 1}, {"initWithName:reason:userInfo:", ex_init, 0},
        {"raise", ex_raise, 0}, {"name", ex_name, 0}, {"reason", ex_reason, 0}, {"userInfo", ex_info, 0},
        {"callStackReturnAddresses", ex_stack, 0}, {"callStackSymbols", ex_stack, 0},
        {"description", ex_description, 0}, {"dealloc", ex_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("NSException", exc);
    static const NsMethod err[] = {
        {"alloc", alloc16, 1}, {"allocWithZone:", alloc16, 1}, {"errorWithDomain:code:userInfo:", er_with, 1},
        {"initWithDomain:code:userInfo:", er_init, 0}, {"domain", er_domain, 0}, {"code", er_code, 0},
        {"userInfo", er_info, 0}, {"localizedDescription", er_localized, 0},
        {"localizedFailureReason", er_reason, 0}, {"description", er_localized, 0}, {"dealloc", er_dealloc, 0},
        {NULL, NULL, 0}};
    ns_methods("NSError", err);

    static const NsMethod url[] = {
        {"alloc", alloc16, 1}, {"allocWithZone:", alloc16, 1}, {"URLWithString:", ur_withString, 1},
        {"fileURLWithPath:", ur_fileURL, 1}, {"fileURLWithPath:isDirectory:", ur_fileURL, 1},
        {"initWithString:", ur_initWithString, 0}, {"initFileURLWithPath:", ur_initFile, 0},
        {"absoluteString", ur_absolute, 0}, {"description", ur_absolute, 0}, {"relativeString", ur_absolute, 0},
        {"scheme", ur_scheme, 0}, {"host", ur_host, 0}, {"path", ur_path, 0}, {"query", ur_query, 0},
        {"fragment", ur_fragment, 0}, {"resourceSpecifier", ur_resourceSpecifier, 0}, {"port", ur_port, 0},
        {"isFileURL", ur_isFile, 0}, {"isEqual:", ur_isEqual, 0}, {"hash", ur_hash, 0},
        {"copyWithZone:", ur_copy, 0}, {"dealloc", ur_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("NSURL", url);

    static const NsMethod loc[] = {
        {"currentLocale", lc_current, 1}, {"systemLocale", lc_current, 1}, {"autoupdatingCurrentLocale", lc_current, 1},
        {"preferredLanguages", lc_languages, 1}, {"alloc", alloc16, 1}, {"initWithLocaleIdentifier:", lc_init, 0},
        {"localeIdentifier", lc_identifier, 0}, {"objectForKey:", lc_objectForKey, 0},
        {"displayNameForKey:value:", lc_displayName, 0}, {NULL, NULL, 0}};
    ns_methods("NSLocale", loc);
    static const NsMethod tz[] = {
        {"localTimeZone", tz_localZone, 1}, {"systemTimeZone", tz_localZone, 1}, {"defaultTimeZone", tz_localZone, 1},
        {"timeZoneForSecondsFromGMT:", tz_forSeconds, 1}, {"timeZoneWithName:", tz_withName, 1},
        {"timeZoneWithAbbreviation:", tz_withName, 1}, {"knownTimeZoneNames", tz_known, 1},
        {"setDefaultTimeZone:", ret_self, 1}, {"secondsFromGMT", tz_secs, 0}, {"name", tz_name, 0},
        {"abbreviation", tz_name, 0}, {NULL, NULL, 0}};
    ns_methods("NSTimeZone", tz);
    (void)alloc32;
    (void)alloc64;
}

void ns_init_strings(void);
void ns_init_collections(void);

void ns_init(void)
{
    ns_init_strings();
    ns_init_collections();
    ns_init_misc();
}
