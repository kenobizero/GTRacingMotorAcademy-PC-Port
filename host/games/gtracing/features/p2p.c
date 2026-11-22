/* p2p.c -- ONLINE_P2P: the main menu's Multiplayer > Online without Gameloft's server.

   Gameloft's server is gone, so Online runs peer to peer, and a friend can host without changing the game's
   settings.  The original Online
   mode (NetworkManagerOnline) logs into Gameloft's lobby server (GLXPlayerMPLobby), which is gone, and the
   server's side of that protocol is not in the game.  So:
     - The Online tile takes the Local tile's path (GameTypeMenu's handler 0x1bb6c0 sets the game type
       [G]+0x4390 = 1, Local's NetworkManagerWLAN; Online's was 2), marked as online here.  The races are
       the Local mode's (net.c), with the players on the internet reached through net.c's tunnel: one UDP
       port, every far player a virtual address 10.254.254.<n> to the game.
     - Hosting (the WLAN host binds port 28190): the tunnel opens on UDP 28200 (GT_P2P_PORT) and the port is
       opened on the way to the internet, first by NAT-PMP to the default route's gateway (home routers with
       NAT-PMP, and ProtonVPN's port forwarding: its gateway 10.2.0.1 hands out a random port, renewed every
       45 s), then by UPnP IGD (SSDP, the router's description, AddPortMapping / GetExternalIPAddress).  The
       public address and port become a room code, GT-XXXXX-XXXXX (Crockford base32 of the 6 bytes), copied
       to the clipboard and shown in the lobby's server message line.
     - Joining (the WLAN join search binds 54046): the room code is read from the clipboard (checked again
       every 2 s while the Join list is open), and the host gets hellos from the tunnel; it learns the joiner
       from them, so only the host's port has to be open.  The host's server then shows in the Join list.
     - No port (UPnP off, or the router's own internet address is private: carrier-grade NAT), the lobby
       says so: ProtonVPN with Port Forwarding on the host's PC, or the other player hosts.
   Testing: GT_LAN_IP (net.c) gives the room code of that address with no port mapping;
   GT_P2P_CODE_FILE=<file> is used instead of the clipboard (written by the host, read by the joiner). */
#define _CRT_RAND_S                       /* rand_s: the session keys */
#include "plat_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "features.h"
#include "game.h"
#include "net.h"

u16 net_p2p_open(u16 port);
void net_p2p_reset(void);
void net_p2p_stop(void);
void net_p2p_add_peer(u32 ip_be, u16 port_be);
void net_p2p_set_key(u32 key);

#define PORT_HOST 28190                   /* the WLAN host's join port */
#define PORT_SEARCH 54046                 /* the WLAN join list's port */
#define LOBBY_VTBL 0x787ef8u
#define LOBBY_UPDATE_SLOT (LOBBY_VTBL + 0x1c)
#define LOBBY_UPDATE_FN 0x1b4c64u

static int p2p_online;                    /* the Online tile was the last one pressed */
static volatile int host_state;           /* 0 idle, 1 opening the port, 2 code ready, 3 no port */
static char room_code[32], host_msg[256];
static int joining;
static u32 join_ip, join_key, host_key;
static u16 join_port;
static volatile int form_kind, form_enter, form_open_code, name_form_pending;   /* the forms, below */
static HANDLE host_thread;
static volatile LONG host_gen;

/* room codes */

/* GT-XXXX-XXXX-XXXX-XXXX: Crockford base32 of 10 bytes, the host's public address (4) and port (2), the session's
   random key (3: the tunnel takes only packets with it, so a stale code or a stranger who knows the address
   gets nowhere, and every hosting gets a new code) and a CRC-8 of those 9 (a mistyped code is told apart) */
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static u8 crc8(const u8 *p, int n)
{
    u8 c = 0;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (u8)(c & 0x80 ? c << 1 ^ 0x07 : c << 1);
    }
    return c;
}

static void code_make(u32 ip_be, u16 port_be, u32 key, char *out)
{
    u8 b[10];
    memcpy(b, &ip_be, 4);
    memcpy(b + 4, &port_be, 2);
    b[6] = (u8)(key >> 16), b[7] = (u8)(key >> 8), b[8] = (u8)key;
    b[9] = crc8(b, 9);
    char d[16];
    int acc = 0, nb = 0, k = 0;
    for (int i = 0; i < 10; i++) {
        acc = acc << 8 | b[i];
        nb += 8;
        while (nb >= 5) {
            nb -= 5;
            d[k++] = B32[acc >> nb & 31];
        }
    }
    sprintf(out, "GT-%.4s-%.4s-%.4s-%.4s", d, d + 4, d + 8, d + 12);
}

static int b32v(int ch)
{
    ch = toupper(ch);
    if (ch == 'O')
        ch = '0';
    if (ch == 'I' || ch == 'L')
        ch = '1';
    const char *p = ch ? strchr(B32, ch) : NULL;
    return p ? (int)(p - B32) : -1;
}

/* the first room code in the text: 1, 0 for none, -1 for one that does not check out (mistyped) */
static int code_parse(const char *t, u32 *ip_be, u16 *port_be, u32 *key)
{
    int bad = 0;
    for (const char *p = t; p && *p; p++) {
        if (toupper(p[0]) != 'G' || toupper(p[1]) != 'T' || p[2] != '-')
            continue;
        u8 b[10];
        int acc = 0, nb = 0, k = 0, n = 0;
        for (const char *q = p + 3; *q && n < 16; q++) {
            if (*q == '-' || *q == ' ')
                continue;
            int d = b32v((u8)*q);
            if (d < 0)
                break;
            n++;
            acc = acc << 5 | d;
            nb += 5;
            if (nb >= 8) {
                nb -= 8;
                b[k++] = (u8)(acc >> nb);
            }
        }
        if (n != 16 || k != 10 || crc8(b, 9) != b[9]) {
            bad = 1;
            continue;
        }
        memcpy(ip_be, b, 4);
        memcpy(port_be, b + 4, 2);
        *key = (u32)b[6] << 16 | (u32)b[7] << 8 | b[8];
        if (*ip_be && *port_be)
            return 1;
        bad = 1;
    }
    return bad ? -1 : 0;
}

/* clipboard (or GT_P2P_CODE_FILE) */

static void clip_put(const char *s)
{
    const char *f = dev_getenv("GT_P2P_CODE_FILE");
    if (f && *f) {
        FILE *o = fopen(f, "w");
        if (o) {
            fputs(s, o);
            fclose(o);
        }
        return;
    }
#ifndef _WIN32
    SDL_SetClipboardText(s);
    return;
#else
    if (!OpenClipboard(NULL))
        return;
    EmptyClipboard();
    size_t n = strlen(s) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n * 2);
    if (h) {
        WCHAR *w = GlobalLock(h);
        for (size_t i = 0; i < n; i++)
            w[i] = (WCHAR)(u8)s[i];
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h))
            GlobalFree(h);
    }
    CloseClipboard();
#endif
}

static int clip_get(char *out, size_t cap)
{
    out[0] = 0;
    const char *f = dev_getenv("GT_P2P_CODE_FILE");
    if (f && *f) {
        FILE *i = fopen(f, "r");
        if (!i)
            return 0;
        size_t n = fread(out, 1, cap - 1, i);
        out[n] = 0;
        fclose(i);
        return n > 0;
    }
#ifndef _WIN32
    char *t = SDL_HasClipboardText() ? SDL_GetClipboardText() : NULL;
    if (t) {
        size_t i = 0;
        for (; t[i] && i < cap - 1; i++)
            out[i] = (u8)t[i] < 128 ? t[i] : ' ';
        out[i] = 0;
        SDL_free(t);
    }
    return out[0] != 0;
#else
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(NULL))
        return 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *w = h ? GlobalLock(h) : NULL;
    if (w) {
        size_t i = 0;
        for (; w[i] && i < cap - 1; i++)
            out[i] = w[i] < 128 ? (char)w[i] : ' ';
        out[i] = 0;
        GlobalUnlock(h);
    }
    CloseClipboard();
    return out[0] != 0;
#endif
}

/* the way to the internet */

static int is_private(u32 ip_be)
{
    u32 h = ntohl(ip_be);
    return (h >> 24) == 10 || (h >> 24) == 127 || (h >> 20) == 0xac1 || (h >> 16) == 0xc0a8 ||
           (h >> 16) == 0xa9fe || (h >> 22) == (0x64400000u >> 22) || !h;
}

/* the gateway and local address the internet traffic takes (a VPN's when one is on) */
static int default_route(u32 *gw_be, u32 *local_be)
{
#ifndef _WIN32
    /* Android has no routing table an app may read: the local address the internet traffic leaves from (a UDP
       socket "connected" to a public address sends nothing), and the router at .1 of its /24, the usual home
       network's */
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in d = {0}, me = {0};
    socklen_t ml = sizeof me;
    d.sin_family = AF_INET;
    d.sin_port = htons(53);
    d.sin_addr.s_addr = inet_addr("8.8.8.8");
    int ok = u != INVALID_SOCKET && !connect(u, (struct sockaddr *)&d, sizeof d) &&
             !getsockname(u, (struct sockaddr *)&me, &ml) && me.sin_addr.s_addr;
    if (u != INVALID_SOCKET)
        closesocket(u);
    if (!ok)
        return 0;
    *local_be = me.sin_addr.s_addr;
    *gw_be = htonl((ntohl(me.sin_addr.s_addr) & 0xFFFFFF00u) | 1u);
    return 1;
#else
    MIB_IPFORWARDROW r;
    if (GetBestRoute(inet_addr("8.8.8.8"), 0, &r) != NO_ERROR)
        return 0;
    *gw_be = r.dwForwardNextHop;
    *local_be = 0;
    ULONG len = 16 * 1024;
    IP_ADAPTER_ADDRESSES *aa = malloc(len);
    if (aa && GetAdaptersAddresses(AF_INET, 0, NULL, aa, &len) == NO_ERROR)
        for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next)
            if (a->IfIndex == r.dwForwardIfIndex && a->FirstUnicastAddress)
                *local_be = ((struct sockaddr_in *)a->FirstUnicastAddress->Address.lpSockaddr)->sin_addr.s_addr;
    free(aa);
    return *gw_be != 0;
#endif
}

/* NAT-PMP (RFC 6886): the external address, then a UDP mapping; 0 on no answer */
static int natpmp(u32 gw_be, u16 port, u32 lifetime, u32 *ext_ip, u16 *ext_port)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    gt_sock_timeout(s, SO_RCVTIMEO, 300);
    struct sockaddr_in g = {0};
    g.sin_family = AF_INET;
    g.sin_addr.s_addr = gw_be;
    g.sin_port = htons(5351);
    u8 rq[12], rs[16];
    int ok = 0;
    for (int tries = 0; tries < 3 && !ok; tries++) {
        rq[0] = 0, rq[1] = 0;
        sendto(s, (const char *)rq, 2, 0, (struct sockaddr *)&g, sizeof g);
        int n = recv(s, (char *)rs, sizeof rs, 0);
        if (n >= 12 && rs[1] == 128 && !rs[2] && !rs[3]) {
            memcpy(ext_ip, rs + 8, 4);
            ok = 1;
        }
    }
    if (ok) {
        ok = 0;
        for (int tries = 0; tries < 3 && !ok; tries++) {
            memset(rq, 0, sizeof rq);
            rq[1] = 1;                                /* UDP */
            rq[4] = (u8)(port >> 8), rq[5] = (u8)port, rq[6] = (u8)(port >> 8), rq[7] = (u8)port;
            rq[8] = (u8)(lifetime >> 24), rq[9] = (u8)(lifetime >> 16), rq[10] = (u8)(lifetime >> 8), rq[11] = (u8)lifetime;
            sendto(s, (const char *)rq, 12, 0, (struct sockaddr *)&g, sizeof g);
            int n = recv(s, (char *)rs, sizeof rs, 0);
            if (n >= 16 && rs[1] == 129 && !rs[2] && !rs[3]) {
                *ext_port = (u16)(rs[10] << 8 | rs[11]);
                ok = 1;
            }
        }
    }
    closesocket(s);
    return ok;
}

/* UPnP IGD: SSDP, the description, two SOAP calls */

static int http(u32 ip_be, u16 port, const char *req, char *out, int cap, u32 *local_be)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    gt_sock_timeout(s, SO_RCVTIMEO, 3000);
    gt_sock_timeout(s, SO_SNDTIMEO, 3000);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip_be;
    a.sin_port = htons(port);
    int n = 0;
    if (!connect(s, (struct sockaddr *)&a, sizeof a)) {
        if (local_be) {
            struct sockaddr_in l;
            int ll = sizeof l;
            getsockname(s, (struct sockaddr *)&l, &ll);
            *local_be = l.sin_addr.s_addr;
        }
        send(s, req, (int)strlen(req), 0);
        for (int r; n < cap - 1 && (r = recv(s, out + n, cap - 1 - n, 0)) > 0;)
            n += r;
    }
    closesocket(s);
    out[n] = 0;
    return n;
}

static int url_split(const char *url, u32 *ip_be, u16 *port, char *path, size_t cap)
{
    char host[64];
    unsigned p = 80;
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t i = 0;
    while (h[i] && h[i] != ':' && h[i] != '/' && i < sizeof host - 1)
        host[i] = h[i], i++;
    host[i] = 0;
    h += i;
    if (*h == ':')
        p = (unsigned)strtoul(h + 1, (char **)&h, 10);
    snprintf(path, cap, "%s", *h ? h : "/");
    *port = (u16)p;
    return inet_pton(AF_INET, host, ip_be) == 1;
}

static const char *tag(const char *x, const char *name, char *out, size_t cap)
{
    char open[64];
    snprintf(open, sizeof open, "<%s>", name);
    const char *p = x ? strstr(x, open) : NULL;
    if (!p)
        return NULL;
    p += strlen(open);
    const char *e = strchr(p, '<');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= cap)
        n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return p;
}

static int soap(u32 ip_be, u16 port, const char *path, const char *svc, const char *action, const char *args,
                char *out, int cap)
{
    char body[1536], req[2560];
    snprintf(body, sizeof body,
             "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
             "</s:Body></s:Envelope>\r\n", action, svc, args, action);
    char a[16];
    inet_ntop(AF_INET, &ip_be, a, sizeof a);
    snprintf(req, sizeof req,
             "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
             "SOAPAction: \"%s#%s\"\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
             path, a, port, svc, action, (unsigned)strlen(body), body);
    int n = http(ip_be, port, req, out, cap, NULL);
    return n > 0 && strstr(out, " 200 ");
}

/* the mapping made, taken away again when the game ends */
static struct { u32 ip; u16 port, ext; char path[256]; const char *svc; } um;
static void upnp_unmap(void)
{
    if (!um.svc)
        return;
    char args[256], out[2048];
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
                                "<NewProtocol>UDP</NewProtocol>", um.ext);
    soap(um.ip, um.port, um.path, um.svc, "DeletePortMapping", args, out, sizeof out);
    um.svc = NULL;
}

static int upnp(u32 gw_be, u16 *port, u32 *ext_ip, u16 *ext_port)
{
    /* SSDP: the gateway's description URL */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    gt_sock_timeout(s, SO_RCVTIMEO, 700);
    struct sockaddr_in m = {0};
    m.sin_family = AF_INET;
    m.sin_addr.s_addr = inet_addr("239.255.255.250");
    m.sin_port = htons(1900);
    static const char *const st[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                     "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                     "urn:schemas-upnp-org:service:WANIPConnection:1"};
    char loc[256] = "", buf[2048];
    for (int round = 0; round < 3 && !loc[0]; round++)
        for (int k = 0; k < 3 && !loc[0]; k++) {
            char q[256];
            snprintf(q, sizeof q, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                  "MX: 1\r\nST: %s\r\n\r\n", st[k]);
            sendto(s, q, (int)strlen(q), 0, (struct sockaddr *)&m, sizeof m);
            /* the unicast to the gateway too: some routers answer only that */
            struct sockaddr_in g = m;
            g.sin_addr.s_addr = gw_be;
            sendto(s, q, (int)strlen(q), 0, (struct sockaddr *)&g, sizeof g);
            for (;;) {
                struct sockaddr_in from;
                int fl = sizeof from;
                int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
                if (n <= 0)
                    break;
                buf[n] = 0;
                if (from.sin_addr.s_addr != gw_be)
                    continue;                     /* only the router the internet traffic takes */
                for (char *l = buf; l && *l; l = strstr(l, "\r\n") ? strstr(l, "\r\n") + 2 : NULL)
                    if (!_strnicmp(l, "LOCATION:", 9)) {
                        const char *v = l + 9;
                        while (*v == ' ')
                            v++;
                        size_t i = 0;
                        while (v[i] && v[i] != '\r' && i < sizeof loc - 1)
                            loc[i] = v[i], i++;
                        loc[i] = 0;
                        break;
                    }
                if (loc[0])
                    break;
            }
        }
    closesocket(s);
    if (!loc[0]) {
        rt_log("[p2p] UPnP: no answer from the router\n");
        return 0;
    }
    u32 dip;
    u16 dport;
    char path[256];
    if (!url_split(loc, &dip, &dport, path, sizeof path))
        return 0;
    static char desc[65536];
    char req[512], dh[16];
    inet_ntop(AF_INET, &dip, dh, sizeof dh);
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n", path, dh, dport);
    u32 local = 0;
    if (http(dip, dport, req, desc, sizeof desc, &local) <= 0)
        return 0;
    static const char *const svcs[] = {"urn:schemas-upnp-org:service:WANIPConnection:2",
                                       "urn:schemas-upnp-org:service:WANIPConnection:1",
                                       "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (int k = 0; k < 3; k++) {
        const char *sv = strstr(desc, svcs[k]);
        char ctl[256];
        if (!sv || !tag(sv, "controlURL", ctl, sizeof ctl))
            continue;
        u32 cip = dip;
        u16 cport = dport;
        char cpath[256];
        if (strstr(ctl, "://"))
            url_split(ctl, &cip, &cport, cpath, sizeof cpath);
        else if (snprintf(cpath, sizeof cpath, "%s%s", ctl[0] == '/' ? "" : "/", ctl) >= (int)sizeof cpath)
            continue;                              /* a control path too long for any router: not this one */
        char out[4096], args[768], lip[16], ip[32];
        inet_ntop(AF_INET, &local, lip, sizeof lip);
        if (!soap(cip, cport, cpath, svcs[k], "GetExternalIPAddress", "", out, sizeof out) ||
            !tag(out, "NewExternalIPAddress", ip, sizeof ip) || inet_pton(AF_INET, ip, ext_ip) != 1)
            continue;
        /* the port, else the next ones: another PC behind the same router may host already (its mapping is
           left alone: a port the router maps to another address is skipped) */
        u16 base = *port;
        for (int t = 0; t < 10; t++) {
            u16 p = (u16)(base + t);
            if (t && net_p2p_open(p) != p)
                continue;                                     /* taken on this PC */
            snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
                                        "<NewProtocol>UDP</NewProtocol>", p);
            char owner[32];
            if (soap(cip, cport, cpath, svcs[k], "GetSpecificPortMappingEntry", args, out, sizeof out) &&
                tag(out, "NewInternalClient", owner, sizeof owner) && strcmp(owner, lip)) {
                rt_log("[p2p] UPnP: UDP %u is another PC's (%s), trying the next port\n", p, owner);
                continue;
            }
            for (int lease = 0; lease < 2; lease++) {
                snprintf(args, sizeof args,
                         "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                         "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient>"
                         "<NewEnabled>1</NewEnabled><NewPortMappingDescription>GT Racing online</NewPortMappingDescription>"
                         "<NewLeaseDuration>%u</NewLeaseDuration>", p, p, lip, lease ? 0u : 7200u);
                if (soap(cip, cport, cpath, svcs[k], "AddPortMapping", args, out, sizeof out)) {
                    *port = p, *ext_port = p;
                    rt_log("[p2p] UPnP: UDP %u mapped on the router (%s)\n", p, svcs[k] + 27);
                    um.ip = cip, um.port = cport, um.ext = p;
                    snprintf(um.path, sizeof um.path, "%s", cpath);
                    um.svc = svcs[k];
                    static int once;
                    if (!once++)
                        atexit(upnp_unmap);
                    return 1;
                }
            }
            rt_log("[p2p] UPnP: the router refused UDP %u, trying the next port\n", p);
        }
        net_p2p_open(base);
        rt_log("[p2p] UPnP: the router refused the mapping\n");
        return 0;
    }
    rt_log("[p2p] UPnP: no WAN connection service in the router's description\n");
    return 0;
}

/* hosting */

static DWORD WINAPI host_work(void *arg)
{
    LONG gen = (LONG)(intptr_t)arg;
    u16 port = net_p2p_open((u16)rt_setting_int("GT_P2P_PORT", 28200, 1, 65535));
    if (!port) {
        host_state = 3;
        return 0;
    }
    u32 ext_ip = 0;
    u16 ext_port = 0;
    int natpmp_on = 0;
    u32 gw = 0, local = 0;
    if (net_test_ip()) {
        ext_ip = net_test_ip(), ext_port = port;                 /* testing: no mapping */
    } else if (default_route(&gw, &local)) {
        if (natpmp(gw, port, 60, &ext_ip, &ext_port) && !is_private(ext_ip)) {
            natpmp_on = 1;
            if (ext_port != port) {
                /* ProtonVPN hands out its own port and forwards it to the same port here: the tunnel moves
                   there, and the mapping is asked again with that port on both sides (a home router then
                   maps it straight through as well) */
                u16 moved = net_p2p_open(ext_port);
                if (moved == ext_port) {
                    port = moved;
                    u32 ip2;
                    u16 p2;
                    if (natpmp(gw, port, 60, &ip2, &p2))
                        ext_port = p2;
                }
            }
            rt_log("[p2p] NAT-PMP: UDP %u open as %u on the gateway\n", port, ext_port);
        } else if (!upnp(gw, &port, &ext_ip, &ext_port) || is_private(ext_ip)) {
            ext_ip = 0;
        }
    }
    if (gen != host_gen)
        return 0;
    if (!ext_ip || !ext_port) {
        snprintf(host_msg, sizeof host_msg, "ONLINE: your router did not open a port (UPnP off, or no public "
                                            "address). Turn on ProtonVPN Port Forwarding, or let your friend host.");
        host_state = 3;
        rt_log("[p2p] hosting: no open port\n");
        return 0;
    }
    code_make(ext_ip, htons(ext_port), host_key, room_code);
    clip_put(room_code);
    snprintf(host_msg, sizeof host_msg, "ROOM CODE  %s  (copied) - send it to your friend", room_code);
    host_state = 2;
    /* not the code itself: it holds this PC's public address (privacy) */
    rt_log("[p2p] hosting: room code copied (UDP %u)\n", ext_port);
    /* NAT-PMP mappings (ProtonVPN's: 60 s) are renewed while this host runs */
    while (natpmp_on && gen == host_gen) {
        Sleep(45000);
        u32 ip2;
        u16 p2;
        if (gen == host_gen && natpmp(gw, port, 60, &ip2, &p2) && p2 != ext_port)
            rt_log("[p2p] NAT-PMP: the gateway moved the port to %u\n", p2);
    }
    return 0;
}

static void host_start(void)
{
    LONG gen = InterlockedIncrement(&host_gen);
    host_state = 1;
    room_code[0] = 0;
    snprintf(host_msg, sizeof host_msg, "ONLINE: opening the port...");
    net_p2p_reset();
    unsigned r = 0;
    while (!(r & 0xffffff))
        rand_s(&r);
    host_key = r & 0xffffff;                  /* a new key, so a new code, every hosting */
    net_p2p_set_key(host_key);
    joining = 0;
    if (host_thread)
        CloseHandle(host_thread);
    host_thread = CreateThread(NULL, 0, host_work, (void *)(intptr_t)gen, 0, NULL);
    rt_log("[p2p] hosting an online server\n");
}

/* joining */

static void join_start(void)
{
    /* the room code comes from the form (JoinMenu's update hook opens it, prefilled from the clipboard) */
    InterlockedIncrement(&host_gen);
    host_state = 0;
    joining = 1;
    join_ip = 0, join_port = 0, join_key = 0;
    net_p2p_open(0);
    net_p2p_reset();
    form_open_code = 1;
}

/* net.c: a UDP socket of the game was bound */
void p2p_bound(u16 port)
{
    if (!p2p_online)
        return;
    if (port == PORT_HOST)
        host_start();
    else if (port == PORT_SEARCH)
        join_start();
}

/* net.c: the game uses its sockets (each frame in the multiplayer menus and races) */
void p2p_poll(void)
{
}

/* the Online tile, the lobby's line */

static DWORD redirect_until;
static int note_pending;              /* ServerTypeMenu's online note, once per Online tile */

/* menu3d.c's hook on the scene's menu push (slot +0x34, the menu's name): right after the Online tile (whose
   own action in the menu file opens Gameloft Live's login), the host / join menu instead */
u32 p2p_menu_redirect(u32 name)
{
    static const char server_type[] = "ServerTypeMenu";
    if (redirect_until && (int)(redirect_until - GetTickCount()) > 0 && name &&
        !strcmp((const char *)GP(name), "GLLiveLoginMenu")) {
        redirect_until = 0;
        rt_log("[p2p] GLLiveLoginMenu -> ServerTypeMenu\n");
        return GA((void *)server_type);
    }
    return name;
}

void __real_F_1bb6c0(CPU *c);
void __wrap_F_1bb6c0(CPU *c)
{
    /* GameTypeMenu's handler: [ev+4] the button's name, [ev+8] 6 = released on it */
    u32 ev = c->r[1];
    const char *name = rd32(ev + 4) ? (const char *)GP(rd32(ev + 4)) : "";
    /* PLAYER_NAME: the name tile opens the form; the form's OK (the update popup's btnNo) must not reach the
       handler's own case for it (the App Store) */
    if (!strcmp(name, "btnPCName") && rd32(ev + 8) == 6 && form_kind != 1)
        name_form_pending = 1;
    if (form_kind == 1 && !strcmp(name, "btnNo")) {
        if (rd32(ev + 8) == 6)
            form_enter = 1;
        static const char none[] = "";
        u32 old = rd32(ev + 4);
        wr32(ev + 4, GA((void *)none));
        __real_F_1bb6c0(c);
        wr32(ev + 4, old);
        return;
    }
    if (rd32(ev + 8) != 6 || !net_lan_on()) {
        __real_F_1bb6c0(c);
        return;
    }
    int online = !strcmp(name, "btnOnline"), online2 = !strcmp(name, "btnOnline2");
    if (online || online2) {
        /* the Local tile's case (the Wi-Fi test, game type 1); the menu file's own action for the tile then
           opens GLLiveLoginMenu, which the menu lookup below turns into ServerTypeMenu */
        static const char local[] = "btnWireless", local2[] = "btnWireless2";
        u32 old = rd32(ev + 4);
        wr32(ev + 4, GA((void *)(online ? local : local2)));
        p2p_online = 1;
        redirect_until = GetTickCount() + 3000;
        note_pending = 1;
        rt_log("[p2p] Online: the Local game over the internet (room codes)\n");
        __real_F_1bb6c0(c);
        wr32(ev + 4, old);
        return;
    }
    if (!strcmp(name, "btnWireless") || !strcmp(name, "btnWireless2") || !strcmp(name, "btnBluetooth")) {
        if (p2p_online) {
            p2p_online = 0;
            joining = 0;
            InterlockedIncrement(&host_gen);
            host_state = 0;
            net_p2p_stop();
        }
    }
    __real_F_1bb6c0(c);
}

static void (*lobby_update_fn)(CPU *);
static void lobby_update_hook(CPU *c)
{
    u32 self = c->r[0];
    lobby_update_fn(c);
    u32 pl = rd32(self + 4);
    if (!pl || !p2p_online || !host_state)
        return;
    static char shown[256];
    static u32 shown_pl;
    static const char box[] = "LobbyMenu.ServerMessage", field[] = "LobbyMenu.ServerMessage.txtField";
    if (!rt_call2(GT_FN_SWF_FIND, pl, GA((void *)field)))
        return;
    if (strcmp(shown, host_msg) || shown_pl != pl) {
        snprintf(shown, sizeof shown, "%s", host_msg);
        shown_pl = pl;
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)field), GA(shown), 0);
    }
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA((void *)box), 1);
}

/* ServerTypeMenu (Host server / Join server): its "no UPnP" note (NoUpNp, the original Online's) says how the
   online game works, and whether the clipboard holds a room code */
#define STYPE_UPDATE_SLOT (0x7864f8u + 0x1c)
#define STYPE_UPDATE_FN 0x1c1d18u
static void (*stype_update_fn)(CPU *);
static void stype_update_hook(CPU *c)
{
    u32 self = c->r[0];
    stype_update_fn(c);
    u32 pl = rd32(self + 4);
    if (!pl || !p2p_online)
        return;
    static const char *const fields[] = {"ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses.NoCashText.TxtNoCash",
                                         "ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses.NoCashText",
                                         "ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses.field",
                                         "ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses",
                                         "NoUpNp.PopupCongrats.txtExcuses", "ServerTypeMenu.NoUpNp.PopupCongrats.TxtCongrats",
                                         "ServerTypeMenu.NoUpNp.PopupCongrats", "ServerTypeMenu.NoUpNp.PopupCongrats.btnOk",
                                         NULL};
    static DWORD at;
    static char text[400], shown[400];
    static u32 shown_pl;
    if (GetTickCount() - at > 1000 || shown_pl != pl) {
        at = GetTickCount();
        char t[1024], code[40] = "";
        u32 ip, key;
        u16 port;
        int got = clip_get(t, sizeof t) ? code_parse(t, &ip, &port, &key) : 0;
        if (got == 1)
            code_make(ip, port, key, code);
        if (code[0])
            snprintf(text, sizeof text, "ONLINE: room code %s found. Press Join server. To host, press "
                                        "Host server.", code);
        else if (got < 0)
            snprintf(text, sizeof text, "ONLINE: the room code in the clipboard is not right (a typo?). Copy "
                                        "it again from your friend's message.");
        else
            snprintf(text, sizeof text, "ONLINE: to host, press Host server and send the room code. To join, "
                                        "press Join server and type your friend's code.");
    }
    static int rewrite;                      /* the popup's timeline puts its own text back as it opens */
    if (note_pending)
        rewrite = 120;
    if (rewrite > 0 && rewrite-- % 10 == 0)
        shown[0] = 0;
    if (strcmp(shown, text) || shown_pl != pl) {
        /* txtExcuses holds one message per frame label (NoCash, LockedCar, NoMulti, NoClass); only NoCash's
           is a text field (NoCashText.TxtNoCash): the game opens it at LockedCar */
        snprintf(shown, sizeof shown, "%s", text);
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)"ServerTypeMenu.NoUpNp.PopupCongrats.txtExcuses"),
                 GA((void *)"NoCash"), 0);
        if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)fields[0])))
            rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)fields[0]), GA(shown), 1);
        shown_pl = pl;
    }
    if (note_pending) {
        /* a popup with OK: once, when the Online tile opened this menu */
        note_pending = 0;
        rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA((void *)"ServerTypeMenu.NoUpNp"), 1);
    }
}

/* forms: PLAYER_NAME (GameTypeMenu) and the room code (JoinMenu), typed on the keyboard
   PLAYER_NAME: the player's name, changed on the multiplayer page (Local, Bluetooth and Online).
   Why the name was "v" (port fault): the profile's name is UTF-16 (in the save's header a byte length, then 42
   bytes), but this port made the profile with 8-bit "iPad" written over a buffer that held "profiles.sav", so
   "iPad\0iles.sav" read as UTF-16 is CJK characters and a 'v', and the multiplayer lists keep the 'v'.  The name
   in memory: [[0x70c040]] + 4 points to it (a 528-byte heap block); the game saves it with the profile.
   The forms are the menus' own popups, with their own OK buttons: GameTypeMenu.needToUpdate for the name (opened
   by btnPCName, tools/swfpatch_name.py) and JoinMenu.DisconnectedPopup for the room code (opened as Join server
   shows its list in the online game, again by Refresh; prefilled from the clipboard when it holds a code).
   While a form is open pcinput.c gives it the keyboard: text, Backspace, Ctrl+V, Enter = OK, Esc = close; a form
   whose menu stopped updating (left by a click elsewhere) closes itself. */

#undef NAME_MAX                            /* <limits.h>'s, on POSIX */
#define NAME_MAX 16
#define GT_UPDATE_SLOT (0x785f78u + 0x1c)          /* GameTypeMenu */
#define GT_UPDATE_FN 0x1bb57cu
#define JOIN_UPDATE_SLOT (0x7865d8u + 0x1c)        /* JoinMenu */
#define JOIN_UPDATE_FN 0x1c26fcu
#define JOIN_EVENT_SLOT (0x7865d8u + 0x2c)
#define JOIN_EVENT_FN 0x1c28d4u

enum { FORM_NONE, FORM_NAME, FORM_CODE, FORM_FIRST };   /* FORM_FIRST: FIRST_NAME's name form (the main menu) */
static CRITICAL_SECTION form_cs;
static volatile int form_kind, form_enter, form_esc, form_open_code;
static volatile u32 form_tick;
static volatile LONG form_ver;
static char form_text[48], form_msg[96];

/* a form whose menu has not updated for 30 drawn frames was left.  The opener sets the tick before it marks the
   form open: this check runs on the UI thread, and an old tick seen with the open mark closes the form as it
   opens (a port fault seen in testing).  The tick counts drawn frames, not time, so a slow moment of the game
   does not close a form either. */
int p2p_form_open(void)
{
    if (form_kind && eagl_frames() - form_tick > 30) {
        form_kind = FORM_NONE;                       /* its menu was left */
        rt_log("[p2p] form closed: its menu was left\n");
    }
    return form_kind != FORM_NONE;
}

static void form_set(const char *s, const char *msg)
{
    EnterCriticalSection(&form_cs);
    snprintf(form_text, sizeof form_text, "%s", s);
    snprintf(form_msg, sizeof form_msg, "%s", msg ? msg : "");
    LeaveCriticalSection(&form_cs);
    InterlockedIncrement(&form_ver);
}

static int form_char_ok(int kind, int ch)
{
    if (kind == FORM_CODE)
        return isalnum(ch) || ch == '-';
    return ch >= 0x20 && ch < 0x7f && ch != ',' && ch != '\\' && ch != '"';   /* ',' splits the game info */
}

static void form_add(const char *s)
{
    EnterCriticalSection(&form_cs);
    size_t n = strlen(form_text), max = form_kind == FORM_CODE ? 22 : NAME_MAX;
    for (; *s; s++) {
        int ch = (u8)*s;
        if (form_kind == FORM_CODE)
            ch = toupper(ch);
        if (!form_char_ok(form_kind, ch))
            continue;
        if (n >= max)
            break;
        if (form_kind != FORM_CODE && ch == ' ' && (!n || form_text[n - 1] == ' '))
            continue;
        form_text[n++] = (char)ch;
    }
    form_text[n] = 0;
    form_msg[0] = 0;
    LeaveCriticalSection(&form_cs);
    InterlockedIncrement(&form_ver);
}

/* pcinput.c, the main thread: 1 when the event went to a form */
int p2p_form_event(const SDL_Event *e)
{
    if (!p2p_form_open())
        return 0;
    switch (e->type) {
    case SDL_EVENT_TEXT_INPUT:
        form_add(e->text.text);
        return 1;
    case SDL_EVENT_KEY_DOWN: {
        SDL_Keycode k = e->key.key;
        if (k == SDLK_BACKSPACE) {
            EnterCriticalSection(&form_cs);
            size_t n = strlen(form_text);
            if (n)
                form_text[n - 1] = 0;
            form_msg[0] = 0;
            LeaveCriticalSection(&form_cs);
            InterlockedIncrement(&form_ver);
        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            form_enter = 1;
        } else if (k == SDLK_ESCAPE) {
            form_esc = 1;
        } else if (k == SDLK_V && (e->key.mod & SDL_KMOD_CTRL)) {
            char t[1024], code[40];
            u32 ip, key;
            u16 port;
            if (clip_get(t, sizeof t)) {
                if (form_kind == FORM_CODE && code_parse(t, &ip, &port, &key) == 1) {
                    code_make(ip, port, key, code);
                    form_set(code, NULL);
                } else {
                    char *nl = strpbrk(t, "\r\n");
                    if (nl)
                        *nl = 0;
                    if (form_kind == FORM_CODE)
                        form_set("", NULL);
                    form_add(t);
                }
            }
        }
        return 1;
    }
    case SDL_EVENT_KEY_UP:
        return 1;
    }
    return 0;
}

/* the form in its popup: the title (or a message in its place) in the popup's text field, the typed text in a white
   input box like Gameloft LIVE's login: the sprite <popup>.Input that tools/swfpatch_name.py adds, shown by
   its frame "On" (its first frame, "Off", is empty, so the popups' other uses don't show it).  A popup without the
   box (an unpatched menu file) gets both in its text field.  The popup's timeline puts its own text
   back as it opens, so it is written again every 10 frames for 2 s after opening. */
static char box_path[128];

static int box_on(u32 pl, const char *input)
{
    snprintf(box_path, sizeof box_path, "%s.field", input);
    if (rt_call2(GT_FN_SWF_FIND, pl, GA(box_path)))
        return 1;
    if (!rt_call2(GT_FN_SWF_FIND, pl, GA((void *)input)))
        return 0;
    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)input), GA((void *)"On"), 1);
    return rt_call2(GT_FN_SWF_FIND, pl, GA(box_path)) ? 2 : 0;
}

/* a closed form's box back to "Off" (also when its menu was left with the form open) */
static void box_off(u32 pl, const char *input)
{
    snprintf(box_path, sizeof box_path, "%s.field", input);
    if (rt_call2(GT_FN_SWF_FIND, pl, GA(box_path)))
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)input), GA((void *)"Off"), 1);
}

static void form_draw(u32 pl, const char *field, const char *input, const char *title, int opened)
{
    static LONG drawn = -1;
    static int rewrite;
    static char buf[200], val[64];
    int box = box_on(pl, input);
    if (opened)
        rewrite = 120;
    if ((rewrite > 0 && rewrite-- % 10 == 0) || box == 2)
        drawn = -1;
    if (drawn == form_ver)
        return;
    drawn = form_ver;
    EnterCriticalSection(&form_cs);
    if (box) {
        snprintf(buf, sizeof buf, "%s", form_msg[0] ? form_msg : title);
        snprintf(val, sizeof val, "%s_", form_text);
    } else {
        snprintf(buf, sizeof buf, "%s\n%s_%s%s", title, form_text, form_msg[0] ? "\n" : "", form_msg);
    }
    LeaveCriticalSection(&form_cs);
    if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)field)))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)field), GA(buf), 1);
    if (box)
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA(box_path), GA(val), 1);
}

/* the player's name */

static u32 name_buf(void)
{
    u32 player = rd32(GT_PROFILE_SLOT), p = player ? rd32(player) : 0;
    return p ? rd32(p + 4) : 0;
}

static void name_get(char *out, size_t cap)
{
    u32 b = name_buf();
    out[0] = 0;
    if (!b || cap < 2)
        return;
    const u8 *p = GP(b);
    size_t n = 0;
    if (p[0] && p[1]) {                              /* the port's old 8-bit name */
        for (; p[n] && n < 20 && n < cap - 1; n++)
            out[n] = p[n] < 0x80 ? (char)p[n] : '?';
    } else {
        for (size_t i = 0; i < 21 && n < cap - 1; i++) {
            unsigned ch = p[2 * i] | p[2 * i + 1] << 8;
            if (!ch)
                break;
            out[n++] = ch < 0x80 ? (char)ch : '?';
        }
    }
    out[n] = 0;
}

static void name_write(const char *s)
{
    u32 b = name_buf();
    if (!b)
        return;
    u8 *p = GP(b);
    memset(p, 0, 42);                                /* the save's field: 21 UTF-16 units */
    for (int i = 0; s[i] && i < 20; i++)
        p[2 * i] = (u8)s[i];
}

/* the chosen name is kept in <home>/Library/Preferences/player_name.txt (the game writes its save's header from
   another copy, so a name put in memory alone is gone at the next start; the save file itself is not touched) */
static char name_saved[32];
static int name_loaded;

static const char *name_path(char *buf, size_t n)
{
    char g[512];
    snprintf(g, sizeof g, "%s/Library/Preferences/player_name.txt", gt_guest_home);
    return gt_host_path(g, buf, n);
}

static void name_set(const char *s)
{
    snprintf(name_saved, sizeof name_saved, "%s", s);
    name_loaded = 1;
    char p[1024];
    FILE *f = fopen(name_path(p, sizeof p), "w");
    if (f) {
        fputs(name_saved, f);
        fclose(f);
    }
    name_write(name_saved);
    rt_log("[p2p] player name set (%u characters; the name itself is not logged)\n", (unsigned)strlen(s));
}

static void name_load(void)
{
    if (name_loaded)
        return;
    name_loaded = 1;
    char p[1024];
    FILE *f = fopen(name_path(p, sizeof p), "r");
    if (f) {
        if (fgets(name_saved, sizeof name_saved, f))
            name_saved[strcspn(name_saved, "\r\n")] = 0;
        fclose(f);
    }
}

/* RED_BULL_BOARD (board.c): the name the player goes by (the chosen one, else the profile's) */
void p2p_name_now(char *out, size_t cap)
{
    name_load();
    if (name_saved[0])
        snprintf(out, cap, "%s", name_saved);
    else
        name_get(out, cap);
    if (!out[0])
        snprintf(out, cap, "iPad");
}

/* each GameTypeMenu frame: the profile's name in memory is the chosen one; with none chosen, the port's old
   8-bit name is turned into UTF-16 ("iPad" instead of the "v" the multiplayer lists made of it) */
static void name_apply(void)
{
    name_load();
    u32 b = name_buf();
    if (!b)
        return;
    char now[32];
    name_get(now, sizeof now);
    const u8 *q = GP(b);
    if (name_saved[0] && strcmp(now, name_saved))
        name_write(name_saved);
    else if (!name_saved[0] && q[0] && q[1] && now[0])
        name_write(now);                             /* 8-bit -> UTF-16 */
}

/* GameTypeMenu: the name tile and its form */

static void (*gt_update_fn)(CPU *);
static volatile int name_form_pending;
static void gt_update_hook(CPU *c)
{
    u32 self = c->r[0];
    gt_update_fn(c);
    u32 pl = rd32(self + 4);
    if (!pl)
        return;
    static const char tile[] = "GameTypeMenu.btnPCName.txtTitle.field";
    static const char popup[] = "GameTypeMenu.needToUpdate", text[] = "GameTypeMenu.needToUpdate.Texts.field";
    static const char input[] = "GameTypeMenu.needToUpdate.Input";
    static char shown[64], label[64];
    static u32 shown_pl;
    char nm[32];
    name_apply();
    name_get(nm, sizeof nm);
    snprintf(label, sizeof label, "NAME: %s", nm[0] ? nm : "-");
    if ((strcmp(shown, label) || shown_pl != pl) && rt_call2(GT_FN_SWF_FIND, pl, GA((void *)tile))) {
        snprintf(shown, sizeof shown, "%s", label);
        shown_pl = pl;
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)tile), GA(shown), 1);
    }
    int opened = 0;
    if (name_form_pending) {
        name_form_pending = 0;
        form_tick = eagl_frames();                   /* before form_kind: the UI thread checks both */
        form_kind = FORM_NAME;
        form_enter = form_esc = 0;
        form_set(dev_getenv("GT_FORM_TYPE") ? dev_getenv("GT_FORM_TYPE") : nm, NULL);   /* GT_FORM_TYPE: testing */
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)popup), GA((void *)"Show"), 1);
        opened = 1;
        rt_log("[p2p] name form open\n");
    }
    if (form_kind != FORM_NAME) {
        box_off(pl, input);
        return;
    }
    form_tick = eagl_frames();
    if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)"GameTypeMenu.needToUpdate.txtOK.field")))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)"GameTypeMenu.needToUpdate.txtOK.field"), GA((void *)"OK"), 1);
    form_draw(pl, text, input, "YOUR NAME (type it, Enter = OK):", opened);
    if (form_esc || form_enter) {
        int ok = form_enter;
        form_enter = form_esc = 0;
        char v[48];
        EnterCriticalSection(&form_cs);
        snprintf(v, sizeof v, "%s", form_text);
        LeaveCriticalSection(&form_cs);
        size_t n = strlen(v);
        while (n && v[n - 1] == ' ')
            v[--n] = 0;
        if (ok && !n) {
            form_set("", "Type a name first.");
            return;
        }
        if (ok)
            name_set(v);
        form_kind = FORM_NONE;
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)popup), GA((void *)"Hide"), 1);
    }
}

/* FIRST_NAME: a first start asks the name
   Port addition: on a first start with no save, a popup asks the name for online multiplayer as soon as the
   first loading has finished.
   The original never asks a name (Gameloft LIVE's login, now gone, gave one; the profile's own is the
   device's).  A start with no save (neither Documents/Profile_Free.0.gts nor Documents/data.sav in the home as the
   host starts) and no chosen name (player_name.txt) asks for it as soon as the main menu is up after the first
   loading: in the main menu's yes / no popup NavigationMenu.FlurryPopup (EXIT_BUTTON's question, exitbtn.c), with the
   white input box tools/swfpatch_name.py adds to it.  Check = OK, cross = LATER (the profile's name stays, and the
   multiplayer page's NAME tile changes it any time); either way the question is not asked again (the game has made
   its save by then).  On a first start the game opens its own question in the same moment (MainMenu.Tutorial_00,
   "Do you want to start the game's quick tutorial?"): hidden while the name form is open, back after it.
   Keys as in the other forms (pcinput.c): text, Backspace, Ctrl+V, Enter = OK, Esc = LATER; the pad's buttons press
   the popup's.  GT_FIRST_NAME=0: never asked (nor with GT_LAN_MULTIPLAYER=0); GT_FIRST_NAME=force: asked on any start
   (testing). */

#define TUTORIAL_Q "MainMenu.Tutorial_00"
#define CH_VISIBLE 0xab                              /* a character's byte: drawn and hit */
#define MAIN_UPDATE_SLOT (GT_VT_MAIN_MENU + GT_VT_UPDATE)          /* MainMenu (the first start's main menu) */
#define MAIN_UPDATE_FN 0x1dfb50u

static int first_ask;                                /* 1: ask at the main menu, 2: asked */
static int first_seen;                               /* main menu frames before asking */
static u32 first_reshow, first_labels;               /* frames: show the popup again / write its labels until */
static int first_menu = -1;                          /* the form's menu: 0 MainMenu, 1 NavigationMenu */
static int tut_hidden;
static u8 tut_want;                                  /* the tutorial question's visible byte to put back */

/* the popup's paths in the form's menu: MainMenu.FlurryPopup on a first start (NavigationMenu comes after the
   tutorial question), NavigationMenu.FlurryPopup otherwise; the same sprite, so the same box */
static char fp_popup[32], fp_yes[64], fp_txtyes[64], fp_yesf[64], fp_nof[64], fp_field[64], fp_input[64];

static void first_paths(int menu)
{
    const char *m = menu ? "NavigationMenu" : "MainMenu";
    snprintf(fp_popup, sizeof fp_popup, "%s.FlurryPopup", m);
    snprintf(fp_yes, sizeof fp_yes, "%s.btnYes", fp_popup);
    snprintf(fp_txtyes, sizeof fp_txtyes, "%s.Texts.txtYes", fp_popup);
    snprintf(fp_yesf, sizeof fp_yesf, "%s.Texts.txtYes.Field", fp_popup);
    snprintf(fp_nof, sizeof fp_nof, "%s.Texts.txtNo.Field", fp_popup);
    snprintf(fp_field, sizeof fp_field, "%s.Texts.field", fp_popup);
    snprintf(fp_input, sizeof fp_input, "%s.Input", fp_popup);
}

int p2p_first_open(void)
{
    return form_kind == FORM_FIRST;
}

static void flurry_set(u32 pl, const char *path, const char *s)
{
    if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path)))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)path), GA((void *)s), 0);
}

static void first_show(u32 pl)
{
    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA(fp_popup), GA((void *)"Show"), 1);
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA(fp_yes), 1);
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA(fp_txtyes), 1);
    first_labels = eagl_frames() + 120;
}

/* _root.isInTutorial (as the game sets it, 0x1e30dc / 0x1e33a8: RenderFX's set member 0x50bce0(swf, path, name,
   as_value *), the value {0, 1 = boolean, .., .., the bool}).  On a first start it is true, and the popup buttons'
   "Clicked" frames then run the tutorial's step (ActionScript in the button, not the menu's handler: PushMenu
   NavigationMenu, MainMenu's parts hidden), which a press of the name form's check or cross set off (found in
   testing).  So it is false from the press until the button's animation has ended, then true again. */
static u32 tut_var_back, tut_var_pl;                 /* frame to set it true again, the swf */
static u8 tut_val[16];

static void tut_var(u32 pl, int v)
{
    memset(tut_val, 0, sizeof tut_val);
    tut_val[1] = 1;
    tut_val[4] = (u8)v;
    rt_call4(GT_FN_SWF_SET_MEMBER, pl, GA((void *)"_root"), GA((void *)"isInTutorial"), GA(tut_val));
}

/* hidden (not drawn, not hit) while the name form is open: the game's tutorial question, and the popup's own
   tutorial hint (FlurryPopup.Tutorial_06, a green arrow at the check and "Please tap the play button.", shown by
   the popup's timeline while _root.isInTutorial, as on a first start); any frame that finds one visible (the game
   showed it) means it is wanted back */
static u8 hint_want;
static void hide_one(u32 pl, const char *path, int hide, u8 *want)
{
    u32 ch = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path));
    if (!ch)
        return;
    volatile u8 *v = (volatile u8 *)GP(ch + CH_VISIBLE);
    if (hide) {
        if (!tut_hidden || *v)
            *want = *v;
        *v = 0;
    } else {
        *v = *want;
    }
}

static void tut_hide(u32 pl, int hide)
{
    static char hint[64];
    if (!hide && !tut_hidden)
        return;
    snprintf(hint, sizeof hint, "%s.Tutorial_06", fp_popup);
    hide_one(pl, TUTORIAL_Q, hide, &tut_want);
    hide_one(pl, hint, hide, &hint_want);
    tut_hidden = hide;
    if (!hide)
        rt_log("[p2p] the tutorial question %s\n", tut_want ? "back" : "stays hidden (the game had not shown it)");
}

/* the main menus' updates: MainMenu's (hooked here) and NavigationMenu's (exitbtn.c) */
static void first_update(u32 pl, int menu)
{
    if (!pl || !first_ask)
        return;
    if (first_ask == 2 && menu != first_menu)
        return;
    if (tut_var_back && (int)(eagl_frames() - tut_var_back) >= 0) {
        tut_var_back = 0;
        tut_var(tut_var_pl, 1);
        rt_log("[p2p] _root.isInTutorial true again\n");
    }
    int opened = 0;
    if (first_ask == 1) {
        /* the main menu is up after the first loading: it has drawn a few frames and has its popup */
        first_paths(menu);
        if (++first_seen < 6 || !rt_call2(GT_FN_SWF_FIND, pl, GA(fp_popup)))
            return;
        first_ask = 2;
        first_menu = menu;
        if (!net_lan_on()) {
            rt_log("[p2p] first start: no name asked (GT_LAN_MULTIPLAYER=0)\n");
            return;
        }
        form_tick = eagl_frames();                   /* before form_kind: the UI thread checks both */
        form_kind = FORM_FIRST;
        form_enter = form_esc = 0;
        form_set(dev_getenv("GT_FORM_TYPE") ? dev_getenv("GT_FORM_TYPE") : "", NULL);   /* GT_FORM_TYPE: testing */
        first_show(pl);
        opened = 1;
        rt_log("[p2p] first start: name form open (frame %u)\n", eagl_frames());
    }
    if (form_kind != FORM_FIRST) {
        if (tut_hidden) {                            /* closed, or left: the box off, the tutorial question back */
            box_off(pl, fp_input);
            tut_hide(pl, 0);
        }
        return;
    }
    form_tick = eagl_frames();
    tut_hide(pl, 1);
    if (first_reshow && (int)(eagl_frames() - first_reshow) >= 0) {
        first_reshow = 0;
        first_show(pl);
        opened = 1;
    }
    if ((int)(first_labels - eagl_frames()) > 0 && eagl_frames() % 10 == 0) {
        flurry_set(pl, fp_yesf, "OK");               /* the popup's timeline puts its own back */
        flurry_set(pl, fp_nof, "LATER");
    }
    form_draw(pl, fp_field, fp_input, "YOUR NAME FOR ONLINE RACES:", opened);
    if (form_esc || form_enter) {
        int ok = form_enter;
        form_enter = form_esc = 0;
        char v[48];
        EnterCriticalSection(&form_cs);
        snprintf(v, sizeof v, "%s", form_text);
        LeaveCriticalSection(&form_cs);
        size_t n = strlen(v);
        while (n && v[n - 1] == ' ')
            v[--n] = 0;
        if (ok && !n) {
            form_set("", "Type a name first.");
            return;
        }
        if (ok)
            name_set(v);
        else
            rt_log("[p2p] first start: name left for later\n");
        form_kind = FORM_NONE;
        first_reshow = 0;
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA(fp_popup), GA((void *)"Hided"), 0);
        box_off(pl, fp_input);
        tut_hide(pl, 0);
    }
}

void p2p_nav_update(u32 pl)
{
    first_update(pl, 1);
}

static void (*main_update_fn)(CPU *);
static void main_update_hook(CPU *c)
{
    u32 self = c->r[0];
    main_update_fn(c);
    first_update(rd32(self + 4), 0);
}

/* the main menus' events (NavigationMenu: exitbtn.c, MainMenu: topcars.c): 1 when the event is the name form's check
   (OK) or cross (LATER); the menu's own handler then gets it nameless (its FlurryPopup case is the Flurry video's) */
int p2p_nav_event(CPU *c)
{
    if (form_kind != FORM_FIRST)
        return 0;
    u32 ev = c->r[1];
    const char *name = rd32(ev + 4) ? (const char *)GP(rd32(ev + 4)) : "";
    int yes = !strcmp(name, "btnYes"), no = !strcmp(name, "btnNo");
    if (!yes && !no)
        return 0;
    u32 path = rt_call1(GT_FN_EVENT_PATH, ev);
    if (!path || !strstr((const char *)GP(path), "FlurryPopup"))
        return 0;
    u32 pl = rd32(c->r[0] + 4);
    if (pl && hint_want) {
        /* the tutorial is on (the popup showed its hint): off until the press's animation has ended */
        if (!tut_var_back)
            tut_var(pl, 0);
        tut_var_pl = pl;
        tut_var_back = eagl_frames() + 30;
    }
    if (rd32(ev + 8) == 6) {
        if (yes)
            form_enter = 1;
        else
            form_esc = 1;
        /* an empty OK: the press's own animation closes the popup (the button's "Clicked" frame 30, 10 frames at the
           menus' 30 a second, then the popup's "Hide"): shown again after it */
        first_reshow = eagl_frames() + 45;
    }
    return 1;
}

static void first_init(void)
{
    const char *e = getenv("GT_FIRST_NAME");
    if (e && *e == '0') {
        rt_log("[p2p] FIRST_NAME off (GT_FIRST_NAME=0)\n");
        return;
    }
    if (e && !_stricmp(e, "force")) {
        first_ask = 1;
        rt_log("[p2p] FIRST_NAME: asked on this start (GT_FIRST_NAME=force)\n");
        return;
    }
    char g[512], p[1024];
    static const char *const saves[] = {"Documents/Profile_Free.0.gts", "Documents/data.sav"};
    for (int i = 0; i < 2; i++) {
        snprintf(g, sizeof g, "%s/%s", gt_guest_home, saves[i]);
        if (GetFileAttributesA(gt_host_path(g, p, sizeof p)) != INVALID_FILE_ATTRIBUTES)
            return;                                  /* a save: not a first start */
    }
    if (GetFileAttributesA(name_path(p, sizeof p)) != INVALID_FILE_ATTRIBUTES)
        return;                                      /* a name chosen before */
    first_ask = 1;
    rt_log("[p2p] first start (no save): the name is asked at the main menu\n");
}

/* JoinMenu: the room code form */

static void join_apply(u32 ip, u16 port, u32 key)
{
    join_ip = ip, join_port = port, join_key = key;
    net_p2p_reset();
    net_p2p_set_key(key);
    net_p2p_add_peer(ip, port);
    char a[16];
    inet_ntop(AF_INET, &ip, a, sizeof a);
    rt_log("[p2p] joining with a room code: %s:%u\n", a, ntohs(port));
}

static void (*join_update_fn)(CPU *), (*join_event_fn)(CPU *);
static void join_update_hook(CPU *c)
{
    u32 self = c->r[0];
    join_update_fn(c);
    u32 pl = rd32(self + 4);
    static const char popup[] = "JoinMenu.DisconnectedPopup";
    static const char text[] = "JoinMenu.DisconnectedPopup.PopupCongrats.TxtCongrats";
    static const char ok[] = "JoinMenu.DisconnectedPopup.PopupCongrats.txtOk";
    static const char input[] = "JoinMenu.DisconnectedPopup.PopupCongrats.Input";
    if (pl && form_kind != FORM_CODE && !form_open_code)
        box_off(pl, input);                          /* a lost connection's message has no box */
    if (!pl || !p2p_online)
        return;
    int opened = 0;
    if (form_open_code) {
        form_open_code = 0;
        char t[1024], code[40] = "";
        u32 ip, key;
        u16 port;
        if (clip_get(t, sizeof t) && code_parse(t, &ip, &port, &key) == 1)
            code_make(ip, port, key, code);
        form_tick = eagl_frames();                   /* before form_kind: the UI thread checks both */
        form_kind = FORM_CODE;
        form_enter = form_esc = 0;
        form_set(dev_getenv("GT_FORM_TYPE") ? dev_getenv("GT_FORM_TYPE") : code, code[0] ? "From the clipboard: press OK." : NULL);
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)popup), GA((void *)"Show"), 1);
        opened = 1;
        rt_log("[p2p] room code form open%s\n", code[0] ? " (from the clipboard)" : "");
    }
    if (form_kind != FORM_CODE)
        return;
    form_tick = eagl_frames();
    if (rt_call2(GT_FN_SWF_FIND, pl, GA((void *)ok)))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA((void *)ok), GA((void *)"OK"), 1);
    form_draw(pl, text, input, "ROOM CODE (type or Ctrl+V, Enter = OK):", opened);
    if (form_esc || form_enter) {
        int go = form_enter;
        form_enter = form_esc = 0;
        if (go) {
            char v[64];
            u32 ip, key;
            u16 port;
            EnterCriticalSection(&form_cs);
            snprintf(v, sizeof v, "GT-%s", form_text + (!_strnicmp(form_text, "GT-", 3) ? 3 : 0));
            LeaveCriticalSection(&form_cs);
            int r = code_parse(v, &ip, &port, &key);
            if (r != 1) {
                form_set(form_text, r < 0 ? "Not a room code: check it." : "Type the whole code.");
                return;
            }
            join_apply(ip, port, key);
        }
        form_kind = FORM_NONE;
        rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)popup), GA((void *)"Hide"), 1);
    }
}

/* [ev] control, [ev+4] its name, [ev+8] kind (6 = released on it) */
static void join_event_hook(CPU *c)
{
    u32 ev = c->r[1];
    const char *name = rd32(ev + 4) ? (const char *)GP(rd32(ev + 4)) : "";
    if (p2p_online && rd32(ev + 8) == 6 && !strcmp(name, "btnRestart") && form_kind != FORM_CODE)
        form_open_code = 1;                          /* Refresh: the form again */
    if (form_kind == FORM_CODE && !strcmp(name, "btnOk")) {
        u32 path = rt_call1(GT_FN_EVENT_PATH, ev);
        if (path && strstr((const char *)GP(path), "DisconnectedPopup")) {
            if (rd32(ev + 8) == 6)
                form_enter = 1;
            static const char none[] = "";             /* the popup's own case (back to the menus) must not see it */
            u32 old = rd32(ev + 4);
            wr32(ev + 4, GA((void *)none));
            join_event_fn(c);
            wr32(ev + 4, old);
            return;
        }
    }
    join_event_fn(c);
}

void p2p_init(void)
{
    InitializeCriticalSection(&form_cs);
    first_init();
    if (first_ask)
        gt_hook_slot(MAIN_UPDATE_SLOT, MAIN_UPDATE_FN, &main_update_fn, main_update_hook, "p2p main_update_hook");
    gt_hook_slot(STYPE_UPDATE_SLOT, STYPE_UPDATE_FN, &stype_update_fn, stype_update_hook, "p2p stype_update_hook");
    gt_hook_slot(LOBBY_UPDATE_SLOT, LOBBY_UPDATE_FN, &lobby_update_fn, lobby_update_hook, "p2p lobby_update_hook");
    gt_hook_slot(GT_UPDATE_SLOT, GT_UPDATE_FN, &gt_update_fn, gt_update_hook, "p2p gt_update_hook");
    gt_hook_slot(JOIN_UPDATE_SLOT, JOIN_UPDATE_FN, &join_update_fn, join_update_hook, "p2p join_update_hook");
    gt_hook_slot(JOIN_EVENT_SLOT, JOIN_EVENT_FN, &join_event_fn, join_event_hook, "p2p join_event_hook");
}
