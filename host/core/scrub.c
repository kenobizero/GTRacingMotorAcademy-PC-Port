/* scrub.c -- the folders the log does not name, and what a crash dump must not hold (CRASH_DUMP, crashdump.c).
   From the Fast Five host (core/log.c), with the crash dump.

   Why: the log names folders (the save's, the game's, the program's: "[ns] home C:\Users\<name>\..."), and a
   log a player sends with a bug report must not carry the Windows user's name.  So each line has these folders
   as tags instead, the longest (most specific) first:
     %HOME%          the save's folder (home\ next to the program, or GT_HOME)
     %APP%           the game's GTRacingFreemiumHD.app
     %EXE%           the program's folder
     %USERPROFILE%   the Windows user's folder
   either slash, any case, in the ANSI code page (what the narrow Windows calls hand back) and in UTF-8, and each
   public IPv4 address as %IP% (below).  A crash dump is memory, which no one can read before sending it: there
   every copy of those folders, of every value of the environment (the Windows user's name, the PC's) and of a public
   address written out is blanked, keeping the length so the dump stays valid. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "plat.h"

#include "rt.h"

enum { HIDE_HOME, HIDE_APP = 2, HIDE_EXE = 4, HIDE_PROFILE = 6, NHIDE = 8 };
static struct { char path[MAX_PATH * 3]; size_t n; const char *tag; } hide[NHIDE];

static void hide_finish(int slot, const char *tag)
{
    char *p = hide[slot].path;
    size_t n = strlen(p);
    while (n > 3 && (p[n - 1] == '\\' || p[n - 1] == '/'))
        p[--n] = 0;
    if (n <= 3 || n < strlen(tag))      /* "C:\" alone would hide every path; the tag must fit in its place */
        n = 0;
    hide[slot].tag = tag;
    hide[slot].n = n;
}

#ifndef _WIN32
/* Android: paths are UTF-8, one spelling */
void rt_hide_path(const char *path, const char *tag)
{
    int slot = !strcmp(tag, "%HOME%") ? HIDE_HOME : !strcmp(tag, "%APP%") ? HIDE_APP : HIDE_EXE;
    char full[MAX_PATH * 2];
    if (!GetFullPathNameA(path, sizeof full, full, NULL))
        snprintf(full, sizeof full, "%s", path);
    snprintf(hide[slot].path, sizeof hide[slot].path, "%s", full);
    hide[slot + 1].path[0] = 0;
    hide_finish(slot, tag);
    hide_finish(slot + 1, tag);
}
#else
/* slot: the folder in the ANSI code page; slot + 1: in UTF-8, when that is spelled otherwise */
static void hide_set_wide(int slot, const wchar_t *w, const char *tag)
{
    BOOL lossy = FALSE;
    hide[slot].path[0] = hide[slot + 1].path[0] = 0;
    if (!WideCharToMultiByte(CP_ACP, 0, w, -1, hide[slot].path, sizeof hide[slot].path, NULL, &lossy) || lossy)
        hide[slot].path[0] = 0;         /* a name the code page cannot spell never shows up in it */
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, hide[slot + 1].path, sizeof hide[slot + 1].path, NULL, NULL) &&
        !strcmp(hide[slot + 1].path, hide[slot].path))
        hide[slot + 1].path[0] = 0;
    hide_finish(slot, tag);
    hide_finish(slot + 1, tag);
}

/* path: as the narrow Windows calls give it (the ANSI code page) */
void rt_hide_path(const char *path, const char *tag)
{
    int slot = !strcmp(tag, "%HOME%") ? HIDE_HOME : !strcmp(tag, "%APP%") ? HIDE_APP : HIDE_EXE;
    wchar_t w[MAX_PATH * 2];
    char full[MAX_PATH * 2];
    /* GT_HOME may be relative; the game's own calls see it whole */
    if (!GetFullPathNameA(path, sizeof full, full, NULL))
        snprintf(full, sizeof full, "%s", path);
    if (MultiByteToWideChar(CP_ACP, 0, full, -1, w, MAX_PATH * 2))
        hide_set_wide(slot, w, tag);
}
#endif

/* The Windows user's name and the PC's name as whole words (privacy): the folders above hide them inside the
   Windows user's
   own folder, but a folder elsewhere can still carry the name (D:\<name>\Games\..., a Pictures or OneDrive folder
   moved to another drive), and nothing the game logs should name the PC.  Each is written as %USER% / %PC% (or
   a shorter tag when the name is shorter), in the ANSI code page and in UTF-8, and only as a whole word, so a name
   such as "Car" leaves "Cars" alone. */
static int same_char(char a, char b);
enum { WORD_USER, WORD_PC = 2, NWORD = 4 };
static struct { char s[256]; size_t n; const char *tag; } word[NWORD];

#ifdef _WIN32
static void word_set(int slot, const wchar_t *env, const char *tag)
{
    wchar_t w[128];
    DWORD n = GetEnvironmentVariableW(env, w, 128);
    if (!n || n >= 128)
        return;
    for (int k = 0; k < 2; k++) {
        BOOL lossy = FALSE;
        int m = WideCharToMultiByte(k ? CP_UTF8 : CP_ACP, 0, w, -1, word[slot + k].s, sizeof word[slot + k].s, NULL,
                                    k ? NULL : &lossy);
        if (!m || lossy || (k && !strcmp(word[slot + 1].s, word[slot].s)))
            continue;
        word[slot + k].n = strlen(word[slot + k].s);
        word[slot + k].tag = tag;
    }
}
#endif

static int word_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || c == '_' || c >= 0x80;
}

static size_t scrub_word(char *s, size_t n, const char *w, size_t len, const char *tag)
{
    if (len < 3 || n < len)
        return n;
    const char *t = strlen(tag) <= len ? tag : len >= 4 ? "%%%%" : "%%%";      /* never longer than the name */
    size_t o = 0, nt = strlen(t);
    for (size_t i = 0; i < n;) {
        size_t k = 0;
        if (!(i && word_char((unsigned char)s[i - 1])))
            while (k < len && i + k < n && same_char(s[i + k], w[k]))
                k++;
        if (k == len && !(i + k < n && word_char((unsigned char)s[i + k]))) {
            memmove(s + o, t, nt);
            o += nt;
            i += len;
        } else
            s[o++] = s[i++];
    }
    return o;
}

#ifndef _WIN32
static void profile_init(void) {}     /* Android: no user or PC name in the app's paths */
#else
static void profile_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    wchar_t w[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", w, MAX_PATH);
    if (n && n < MAX_PATH)
        hide_set_wide(HIDE_PROFILE, w, "%USERPROFILE%");
    word_set(WORD_USER, L"USERNAME", "%USER%");
    word_set(WORD_PC, L"COMPUTERNAME", "%PC%");
}
#endif

static int same_char(char a, char b)
{
    if (a == '/')
        a = '\\';
    if (b == '/')
        b = '\\';
    return a == b || ((a | 0x20) == (b | 0x20) && (a | 0x20) >= 'a' && (a | 0x20) <= 'z');
}

/* s with every copy of a hidden folder written as its tag */
static size_t scrub1(char *s, size_t n, const char *path, size_t len, const char *tag)
{
    if (!len || n < len)
        return n;
    size_t o = 0, nt = strlen(tag);
    for (size_t i = 0; i < n;) {
        size_t k = 0;
        while (k < len && i + k < n && same_char(s[i + k], path[k]))
            k++;
        if (k == len) {
            memmove(s + o, tag, nt);    /* the tag is never longer than what it replaces */
            o += nt;
            i += len;
        } else
            s[o++] = s[i++];
    }
    return o;
}

/* public IPv4 addresses (privacy)
   The online game (p2p.c, net.c, board.c) logged this PC's public address and the friends' ("joining with a room
   code: <address>:<port>", the router's answers, GT_NET_LOG's every packet).  A public address tells where someone
   lives, so a log sent with a bug report has each one as %IP%; the local ones (10.x, 192.168.x, 172.16-31.x,
   100.64-127.x, 169.254.x, 127.x, the game's 10.254.254.x) stay, they name no one and the LAN tests read them. */
static int ip_local(unsigned a, unsigned b)
{
    return a == 0 || a == 10 || a == 127 || a >= 224 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) ||
           (a == 169 && b == 254) || (a == 100 && b >= 64 && b < 128);
}

static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* the length of a public address "a.b.c.d" at s[i] (each part 0..255, no digit or dotted digit around it), else 0 */
static size_t ip_public_at(const unsigned char *s, size_t n, size_t i)
{
    if (i && (is_digit(s[i - 1]) || s[i - 1] == '.'))
        return 0;
    unsigned part[4];
    size_t k = i;
    for (int p = 0; p < 4; p++) {
        if (p) {
            if (k >= n || s[k] != '.')
                return 0;
            k++;
        }
        size_t d = 0;
        part[p] = 0;
        while (k < n && is_digit(s[k]) && d < 4)
            part[p] = part[p] * 10 + (s[k++] - '0'), d++;
        if (!d || d > 3 || part[p] > 255)
            return 0;
    }
    if (k < n && (is_digit(s[k]) || (s[k] == '.' && k + 1 < n && is_digit(s[k + 1]))))
        return 0;                       /* a longer number: a version, not an address */
    return ip_local(part[0], part[1]) ? 0 : k - i;
}

static size_t scrub_ips(char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        size_t len = is_digit(u[i]) ? ip_public_at(u, n, i) : 0;
        if (len) {
            memcpy(s + o, "%IP%", 4);   /* "1.2.3.4" is 7 characters at least */
            o += 4;
            i += len;
        } else
            s[o++] = s[i++];
    }
    return o;
}

/* The hidden folders, longest first: a folder inside another (the save's inside the Windows user's) is replaced
   whole before the shorter one could cut it, so the tag names the most specific folder. */
static void longest_first(const size_t *len, int count, int *order)
{
    for (int i = 0; i < count; i++) {
        int j = i;
        while (j > 0 && len[order[j - 1]] < len[i]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
}

size_t rt_scrub(char *s, size_t n)
{
    profile_init();
    _Static_assert((int)NWORD <= (int)NHIDE, "one order array for both");
    size_t len[NHIDE];
    int order[NHIDE];
    for (int i = 0; i < NHIDE; i++)
        len[i] = hide[i].n;
    longest_first(len, NHIDE, order);
    for (int k = 0; k < NHIDE; k++) {
        int i = order[k];
        n = scrub1(s, n, hide[i].path, hide[i].n, hide[i].tag ? hide[i].tag : "");
    }
    for (int i = 0; i < NWORD; i++)         /* the user's and the PC's names: the longer first, as above */
        len[i] = word[i].n;
    longest_first(len, NWORD, order);
    for (int k = 0; k < NWORD; k++) {
        int i = order[k];
        if (word[i].n)
            n = scrub_word(s, n, word[i].s, word[i].n, word[i].tag);
    }
    return scrub_ips(s, n);
}

/* Blanks each copy of s in d, keeping the length.  tail: and what follows it in that string, the rest of a path. */
#ifdef _WIN32
static void blank_narrow(unsigned char *d, size_t n, const char *s, size_t len, int tail)
{
    for (size_t o = 0; o + len <= n; o++) {
        size_t k = 0;
        while (k < len && same_char((char)d[o + k], s[k]))
            k++;
        if (k < len)
            continue;
        while (tail && o + k < n && k < MAX_PATH && d[o + k] >= 0x20)
            k++;
        memset(d + o, '_', k);
        o += k - 1;
    }
}

static void blank_wide(unsigned char *d, size_t n, const wchar_t *s, size_t len, int tail)
{
    for (size_t o = 0; o + len * 2 <= n; o++) {
        size_t k = 0;
        while (k < len && (s[k] < 0x80 ? !d[o + k * 2 + 1] && same_char((char)d[o + k * 2], (char)s[k])
                                       : d[o + k * 2] == (s[k] & 0xff) && d[o + k * 2 + 1] == s[k] >> 8))
            k++;
        if (k < len)
            continue;
        while (tail && o + k * 2 + 1 < n && k < MAX_PATH && (d[o + k * 2] >= 0x20 || d[o + k * 2 + 1]))
            k++;
        for (size_t j = 0; j < k; j++)
            d[o + j * 2] = '_', d[o + j * 2 + 1] = 0;
        o += k * 2 - 1;
    }
}

/* Blanks the whole string (8-bit or UTF-16) that holds s: the pieces of a path another string overwrote the start
   of, which no longer match a folder whole (found in the Fast Five host). */
static void blank_run(unsigned char *d, size_t n, const char *s, size_t len, const wchar_t *w, size_t nw)
{
    for (size_t o = 0; o + len <= n; o++) {
        size_t k = 0;
        while (k < len && same_char((char)d[o + k], s[k]))
            k++;
        if (k < len)
            continue;
        size_t a = o, b = o + len;
        while (a > 0 && o - a < MAX_PATH && d[a - 1] >= 0x20)
            a--;
        while (b < n && b - o < MAX_PATH && d[b] >= 0x20)
            b++;
        memset(d + a, '_', b - a);
        o = b - 1;
    }
    for (size_t o = 0; nw && o + nw * 2 <= n; o++) {
        size_t k = 0;
        while (k < nw && (w[k] < 0x80 ? !d[o + k * 2 + 1] && same_char((char)d[o + k * 2], (char)w[k])
                                      : d[o + k * 2] == (w[k] & 0xff) && d[o + k * 2 + 1] == w[k] >> 8))
            k++;
        if (k < nw)
            continue;
        size_t a = o, b = o + nw * 2;
        while (a >= 2 && (o - a) / 2 < MAX_PATH && (d[a - 2] >= 0x20 || d[a - 1]))
            a -= 2;
        while (b + 1 < n && (b - o) / 2 < MAX_PATH && (d[b] >= 0x20 || d[b + 1]))
            b += 2;
        for (size_t j = a; j < b; j += 2)
            d[j] = '_', d[j + 1] = 0;
        o = b - 1;
    }
}

static int slot_cp(int i) { return i % 2 ? CP_UTF8 : CP_ACP; }

/* In the dump's stack memory: each folder name of the hidden folders, but the drive's and the shortest. */
void rt_scrub_names(unsigned char *d, size_t n)
{
    profile_init();
    for (int i = 0; i < NHIDE; i++) {
        const char *p = hide[i].path, *end = p + hide[i].n;
        while (p < end) {
            const char *q = p;
            while (q < end && *q != '\\' && *q != '/')
                q++;
            size_t len = (size_t)(q - p);
            if (len >= 3 && !memchr(p, ':', len)) {
                wchar_t w[MAX_PATH];
                int nw = MultiByteToWideChar(slot_cp(i), 0, p, (int)len, w, MAX_PATH);
                blank_run(d, n, p, len, w, nw > 0 ? (size_t)nw : 0);
            }
            p = q + 1;
        }
    }
}

/* The whole dump: each folder (narrow and UTF-16), and every value of the environment, which the C library keeps
   copies of (the Windows user's name, the PC's name, and whatever else someone keeps there). */
void rt_scrub_bytes(unsigned char *d, size_t n)
{
    profile_init();
    /* longest first (as rt_scrub): a value inside another (the user's name inside USERPROFILE) would otherwise be
       blanked first and leave the longer one no longer whole to match.  The blanks keep every length, so the dump's
       offsets stay valid. */
    size_t hlen[NHIDE];
    int horder[NHIDE];
    for (int i = 0; i < NHIDE; i++)
        hlen[i] = hide[i].n;
    longest_first(hlen, NHIDE, horder);
    for (int k = 0; k < NHIDE; k++) {
        int i = horder[k];
        size_t len = hide[i].n;
        if (!len)
            continue;
        blank_narrow(d, n, hide[i].path, len, 1);
        wchar_t w[MAX_PATH * 2];
        int nw = MultiByteToWideChar(slot_cp(i), 0, hide[i].path, (int)len, w, MAX_PATH * 2);
        if (nw > 0)
            blank_wide(d, n, w, (size_t)nw, 1);
    }
    wchar_t *env = GetEnvironmentStringsW();
    enum { MAX_VALUES = 1024 };
    static const wchar_t *val[MAX_VALUES];      /* static: this runs on the dump's thread, with a small stack */
    static size_t vlen[MAX_VALUES];
    static int vorder[MAX_VALUES];
    int nvals = 0;
    for (wchar_t *e = env; e && *e && nvals < MAX_VALUES; e += wcslen(e) + 1) {
        wchar_t *v = wcschr(e + 1, L'=');       /* +1: some names start with one, "=C:=C:\..." */
        size_t nv = v ? wcslen(++v) : 0;
        if (nv >= 3)
            val[nvals] = v, vlen[nvals++] = nv;
    }
    longest_first(vlen, nvals, vorder);
    static char a[8192];
    for (int k = 0; k < nvals; k++) {
        const wchar_t *v = val[vorder[k]];
        size_t nv = vlen[vorder[k]];
        int path = wcschr(v, L'\\') != NULL;    /* HOMEPATH also stands inside a path that starts \Device\... */
        blank_wide(d, n, v, nv, path);
        for (int cp = 0; cp < 2; cp++) {
            int m = WideCharToMultiByte(cp ? CP_ACP : CP_UTF8, 0, v, (int)nv, a, sizeof a, NULL, NULL);
            if (m)
                blank_narrow(d, n, a, (size_t)m, path);
        }
    }
    if (env)
        FreeEnvironmentStringsW(env);
    /* a public address written out (the router's answer to UPnP sits on a thread's stack): blanked in place */
    for (size_t o = 0; o < n; o++) {
        size_t len = is_digit(d[o]) ? ip_public_at(d, n, o) : 0;
        if (len) {
            memset(d + o, '_', len);
            o += len - 1;
        }
    }
}
#else
void rt_scrub_names(unsigned char *d, size_t n) { (void)d, (void)n; }   /* no crash dump on Android */
void rt_scrub_bytes(unsigned char *d, size_t n) { (void)d, (void)n; }
#endif
