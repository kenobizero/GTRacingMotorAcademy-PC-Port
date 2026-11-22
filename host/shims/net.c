/* net.c -- the network as the game sees it on PC.

   The game's online parts (ads, Gameloft Live, Facebook, Tapjoy, the cross-promotion pages) are not
   part of this port: names do not resolve, NSURLConnection fails as offline and SCNetworkReachability
   says "not reachable" for named hosts.  The game then takes the paths it has for an iPad without a
   connection.  Address conversions (inet_*) are done for real.

   LAN_MULTIPLAYER (default on; GT_LAN_MULTIPLAYER=0 = the all-offline network): the game's Local and
   Bluetooth multiplayer.  Port fault:
   stage 4 cut the whole network, so the main menu's Community > Multiplayer > Local said there was no
   Wi-Fi.  The game's Local mode (NetworkManagerWLAN, 0x2aa5f4..0x2abd0c) is plain BSD UDP: the host
   binds port 28190, a joining iPad binds 14367, looks for hosts with a broadcast (SO_BROADCAST) and then
   sends straight to the address recvfrom gave it, polling with select(timeout 0).  So here the sockets
   are real (Winsock), with the Darwin ABI translated: sockaddr_in's sin_len byte, the errno values,
   fd_set's bitmap, fcntl(O_NONBLOCK), ioctl(FIONBIO/FIONREAD), SO_* timeouts as timevals.
     - Socket fds are 900..1019 (Darwin's fd_set holds 1024), apart from the CRT's file descriptors;
       close() in imp_libc.c hands them here.
     - Only local addresses can be reached (private, loopback, link-local, 100.64/10 of Tailscale,
       26/8 of Radmin VPN, the subnets of this PC's adapters, GT_LAN_PEERS): the dead online services
       stay offline and a connect cannot wait on the internet.
     - A broadcast is sent as each adapter's subnet broadcast (Windows sends 255.255.255.255 on one
       adapter only, so a VPN adapter would hide the LAN or the other way round), and to every address
       in GT_LAN_PEERS=ip[,ip...] as plain unicast (a friend over the internet with the port open).
     - getifaddrs reports this PC's IPv4 adapters, the one with a gateway first as "en0" (the game's
       getWiFiIPAddress, 0x2f74f4, wants en0), and reachability for 169.254.0.0 (the game's Wi-Fi test,
       testWifiConnection 0x2f7624) says "reachable, direct".
     - Testing two games on one PC: GT_LAN_IP=127.0.0.2 makes that the PC's only address (en0), binds to
       it instead of 0.0.0.0 (an unbound socket is bound to it before its first send) and a broadcast
       goes to GT_LAN_PEERS only, so two runs with 127.0.0.2 / 127.0.0.3 see each other like two iPads.
   gk.c (GameKit's GKSession, the Bluetooth mode) is built on the same sockets.

   NO_TCP_WAIT (port fault).  With TCP sockets the start-up to the main menu takes 13.1 s instead of 3.1 s:
   GLAppleMain (Gameloft's GLXPlayer client, http://gllive.gameloft.com/ope/GenericXPlayer_v1.php)
   gets its TCP socket, so instead of failing at once it asks SockUtility::GetHostByName (0x443574,
   an async gethostbyname thread 0x443410 retried every 2000 ms, "pending" and "not found" look the same)
   and the main thread waits ~10 s (1 frame a second) until GLAppleInfoObserver::OnNetworkError.  Every
   TCP user in the game is an internet service (GLXPlayer, GL Live, SSL, curl), none can resolve here, and
   the multiplayer modes are UDP only (no TCP connect in any LAN / Online test), so socket(SOCK_STREAM)
   fails with ENETUNREACH, as with no network at all.  GT_LAN_TCP=1 gives TCP sockets back
   (testing). */
#include "plat_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "net.h"
#include "game_hooks.h"

#define D_ENETUNREACH 51
#define D_EAI_NONAME 8

static u32 fail_with(int e)
{
    rt_thread()->errno_cell = (u32)e;
    return (u32)-1;
}
static u32 fail(void) { return fail_with(D_ENETUNREACH); }

/* settings, Winsock */

static int lan_state = -1;
static int nlog;                         /* GT_NET_LOG=1: each socket call (testing) */
static u32 test_ip;                       /* GT_LAN_IP, network order (0 = the real adapters) */
static u32 peers[16];
static int n_peers;

static void lan_init(void)
{
    if (lan_state >= 0)
        return;
    const char *e = getenv("GT_LAN_MULTIPLAYER");
    int on = !(e && *e == '0');
#ifdef _WIN32
    if (on) {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd)) {
            rt_log("[net] WSAStartup failed: LAN multiplayer off\n");
            on = 0;
        }
    }
#endif
    nlog = dev_getenv("GT_NET_LOG") != NULL;
    e = dev_getenv("GT_LAN_IP");
    if (e && *e)
        inet_pton(AF_INET, e, &test_ip);
    e = getenv("GT_LAN_PEERS");
    if (e && *e) {
        char tmp[512], *ctx = NULL;
        snprintf(tmp, sizeof tmp, "%s", e);
        for (char *t = strtok_s(tmp, ",; ", &ctx); t && n_peers < 16; t = strtok_s(NULL, ",; ", &ctx)) {
            u32 a;
            if (inet_pton(AF_INET, t, &a) == 1)
                peers[n_peers++] = a;
        }
    }
    if (on)
        rt_log("[net] LAN multiplayer on%s%s, %d peer address(es)\n", test_ip ? ", test address " : "",
               test_ip ? dev_getenv("GT_LAN_IP") : "", n_peers);
    lan_state = on;
}
int net_lan_on(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    if (lan_state < 0) {
        BOOL pending;
        InitOnceBeginInitialize(&once, 0, &pending, NULL);
        if (pending) {
            lan_init();
            InitOnceComplete(&once, 0, NULL);
        }
    }
    return lan_state > 0;
}
u32 net_test_ip(void) { return net_lan_on() ? test_ip : 0; }

/* this PC's IPv4 adapters (refreshed every few seconds) */

typedef struct { u32 addr, mask; int gateway; char name[8]; } Adapter;
static Adapter adapters[16];
static int n_adapters;
static DWORD adapters_at;
static SRWLOCK adapters_lock = SRWLOCK_INIT;

static void adapters_refresh(void)
{
    if (n_adapters && GetTickCount() - adapters_at < 5000)
        return;
    adapters_at = GetTickCount();
    Adapter got[16];
    int n = 0;
    if (test_ip) {
        got[0] = (Adapter){test_ip, htonl(0xff000000u), 1, "en0"};
        n = 1;
#ifndef _WIN32
    } else {
        /* Android: the interfaces that are up (wlan, eth and the like first: the ones a LAN game goes over; the
           mobile data interfaces have no broadcast) */
        struct ifaddrs *ifs = NULL;
        if (getifaddrs(&ifs) == 0) {
            for (int pass = 0; pass < 2; pass++)
                for (struct ifaddrs *i = ifs; i && n < 16; i = i->ifa_next) {
                    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
                        (i->ifa_flags & IFF_LOOPBACK))
                        continue;
                    int lan = (i->ifa_flags & IFF_BROADCAST) != 0;
                    if (lan != (pass == 0))
                        continue;
                    Adapter *d = &got[n];
                    d->addr = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
                    d->mask = i->ifa_netmask ? ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr : 0xffffffffu;
                    d->gateway = lan;
                    snprintf(d->name, sizeof d->name, "en%d", n & 15);
                    n++;
                }
            freeifaddrs(ifs);
        }
    }
#else
    } else {
        ULONG len = 16 * 1024;
        IP_ADAPTER_ADDRESSES *aa = malloc(len);
        if (aa && GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER |
                                                    GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
                                       NULL, aa, &len) == ERROR_BUFFER_OVERFLOW) {
            free(aa);
            aa = malloc(len);
            if (aa && GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER |
                                                        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST,
                                           NULL, aa, &len) != NO_ERROR) {
                free(aa);
                aa = NULL;
            }
        }
        for (int pass = 0; pass < 2 && aa; pass++)          /* adapters with a gateway first */
            for (IP_ADAPTER_ADDRESSES *a = aa; a; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    continue;
                if ((a->FirstGatewayAddress != NULL) != (pass == 0))
                    continue;
                for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < 16; u = u->Next) {
                    if (u->Address.lpSockaddr->sa_family != AF_INET)
                        continue;
                    Adapter *d = &got[n];
                    d->addr = ((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr;
                    ULONG m = 0;
                    ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &m);
                    d->mask = m;
                    d->gateway = pass == 0;
                    snprintf(d->name, sizeof d->name, "en%d", n & 15);   /* n < 16 */
                    n++;
                }
            }
        free(aa);
    }
#endif
    AcquireSRWLockExclusive(&adapters_lock);
    memcpy(adapters, got, sizeof got);
    n_adapters = n;
    ReleaseSRWLockExclusive(&adapters_lock);
}
static int adapters_copy(Adapter *out)
{
    adapters_refresh();
    AcquireSRWLockShared(&adapters_lock);
    int n = n_adapters;
    memcpy(out, adapters, sizeof adapters);
    ReleaseSRWLockShared(&adapters_lock);
    return n;
}

static int is_broadcast(u32 a)
{
    if (a == 0xffffffffu)
        return 1;
    Adapter ad[16];
    int n = adapters_copy(ad);
    for (int i = 0; i < n; i++)
        if (ad[i].mask != 0xffffffffu && a == (ad[i].addr | ~ad[i].mask))
            return 1;
    return 0;
}

/* RED_BULL_BOARD (board.c): one of this PC's own addresses (the test address, loopback without one, an adapter) */
int net_own_addr(u32 a)
{
    if (test_ip)
        return a == test_ip;
    if ((ntohl(a) >> 24) == 127 || !a)
        return 1;
    Adapter ad[16];
    int n = adapters_copy(ad);
    for (int i = 0; i < n; i++)
        if (a == ad[i].addr)
            return 1;
    return 0;
}

/* addresses a socket may reach: see the top of the file */
int net_addr_allowed(u32 a)
{
    u32 h = ntohl(a);
    if ((h >> 24) == 10 || (h >> 24) == 127 || (h >> 24) == 26 || (h >> 20) == 0xac1 || (h >> 16) == 0xc0a8 ||
        (h >> 16) == 0xa9fe || (h >> 22) == (0x64400000u >> 22) || h == 0xffffffffu)
        return 1;
    for (int i = 0; i < n_peers; i++)
        if (peers[i] == a)
            return 1;
    Adapter ad[16];
    int n = adapters_copy(ad);
    for (int i = 0; i < n; i++)
        if ((a & ad[i].mask) == (ad[i].addr & ad[i].mask))
            return 1;
    return 0;
}

/* the socket table */

#define SOCK_BASE 900
#define SOCK_MAX 120
typedef struct Dgram { struct Dgram *next; u32 from_ip; u16 from_port; u32 n; u8 data[]; } Dgram;
typedef struct {
    SOCKET s;
    int used, nonblock, type, bound;
    u16 lport;                            /* host order, once bound */
    Dgram *q, *qt;                        /* ONLINE_P2P: datagrams from the tunnel */
    int qn;
} Sock;
static Sock socks[SOCK_MAX];
static SRWLOCK socks_lock = SRWLOCK_INIT;

static Sock *sk(u32 fd)
{
    if (fd < SOCK_BASE || fd >= SOCK_BASE + SOCK_MAX || !socks[fd - SOCK_BASE].used)
        return NULL;
    return &socks[fd - SOCK_BASE];
}
int net_is_socket(u32 fd) { return sk(fd) != NULL; }
/* BACKGROUND_THROTTLE (uikit.c): the game has a socket open (Local over LAN, Online) or the ONLINE_P2P tunnel is
   up: the window keeps its full frame rate */
static volatile int tun_on;
int net_sockets_open(void)
{
    if (tun_on)
        return 1;
    for (int i = 0; i < SOCK_MAX; i++)
        if (socks[i].used)
            return 1;
    return 0;
}

static u32 werr_map(int e)
{
    switch (e) {
    case WSAEWOULDBLOCK: return 35;     /* EAGAIN */
    case WSAEINPROGRESS: return 36;
    case WSAEALREADY: return 37;
    case WSAENOTSOCK: return 38;
    case WSAEDESTADDRREQ: return 39;
    case WSAEMSGSIZE: return 40;
    case WSAEPROTOTYPE: return 41;
    case WSAENOPROTOOPT: return 42;
    case WSAEPROTONOSUPPORT: return 43;
    case WSAEAFNOSUPPORT: return 47;
    case WSAEADDRINUSE: return 48;
    case WSAEADDRNOTAVAIL: return 49;
    case WSAENETDOWN: return 50;
    case WSAENETUNREACH: return 51;
    case WSAENETRESET: return 52;
    case WSAECONNABORTED: return 53;
    case WSAECONNRESET: return 54;
    case WSAENOBUFS: return 55;
    case WSAEISCONN: return 56;
    case WSAENOTCONN: return 57;
    case WSAESHUTDOWN: return 58;
    case WSAETIMEDOUT: return 60;
    case WSAECONNREFUSED: return 61;
    case WSAEHOSTUNREACH: return 65;
    case WSAEACCES: return 13;
    case WSAEFAULT: return 14;
    case WSAEMFILE: return 24;
    default: return 22;                 /* EINVAL */
    }
}
static u32 werr(void) { return fail_with((int)werr_map(WSAGetLastError())); }

/* Darwin's sockaddr_in: u8 sin_len, u8 sin_family, port, address -- Windows: u16 family, port, address */
static int sa_in(u32 ga, u32 len, struct sockaddr_in *w)
{
    const u8 *b = GP(ga);
    if (!ga || len < 8 || b[1] != 2)
        return 0;
    memset(w, 0, sizeof *w);
    w->sin_family = AF_INET;
    memcpy(&w->sin_port, b + 2, 2);
    memcpy(&w->sin_addr, b + 4, 4);
    return 1;
}
static void sa_out(const struct sockaddr_in *w, u32 ga, u32 lenp)
{
    if (!ga)
        return;
    u8 t[16] = {16, 2};
    memcpy(t + 2, &w->sin_port, 2);
    memcpy(t + 4, &w->sin_addr, 4);
    u32 room = lenp ? rd32(lenp) : 16;
    memcpy(GP(ga), t, room < 16 ? room : 16);
    if (lenp)
        wr32(lenp, 16);
}

static void sock_note_port(Sock *s)
{
    struct sockaddr_in w;
    int wl = sizeof w;
    if (!getsockname(s->s, (struct sockaddr *)&w, &wl))
        s->lport = ntohs(w.sin_port);
}
static void sock_bind_any(Sock *s)
{
    /* an unbound socket gets its port now (the tunnel names it; GT_LAN_IP: sends from the test address) */
    if (s->bound)
        return;
    struct sockaddr_in w = {0};
    w.sin_family = AF_INET;
    w.sin_addr.s_addr = test_ip;
    if (!bind(s->s, (struct sockaddr *)&w, sizeof w)) {
        s->bound = 1;
        sock_note_port(s);
    }
}
static void sock_bind_test(Sock *s)
{
    if (test_ip)
        sock_bind_any(s);
}

/* ONLINE_P2P: the game's UDP to players on the internet, through one UDP port
   p2p.c starts it (the Online tile, hosting or joining with a room code).  Each player on the far side is a virtual
   address 10.254.254.<n> to the game; what the game sends there (and its broadcasts) goes to that player's
   address:port in one datagram ("GTP2", kind, the room code's key, source port, destination port, payload; one with another key is dropped), and what comes in is
   queued on the game's socket bound to the destination port, from <virtual address>:<source port>.  Every
   second each player gets a hello, which keeps both routers' mappings open; a player who sends first (the one
   joining with the code) is learnt from its packets, so only the host's port has to be open. */

#define VIP_BASE 0x0afefe00u               /* 10.254.254.0, host order */
#define TPEERS 8
typedef struct { u32 ip; u16 port; int used, fixed; DWORD last_rx, last_tx; } TPeer;
static TPeer tpeers[TPEERS];
static SOCKET tun = INVALID_SOCKET;
static u16 tun_port;
static CRITICAL_SECTION tun_lock;
static INIT_ONCE tun_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK tun_init_once(INIT_ONCE *o, void *p, void **ctx)
{
    (void)o, (void)p, (void)ctx;
    InitializeCriticalSection(&tun_lock);
    return TRUE;
}
static void tun_lock_init(void) { InitOnceExecuteOnce(&tun_once, tun_init_once, NULL, NULL); }

static int vip_index(u32 a_be)
{
    u32 h = ntohl(a_be);
    if ((h & 0xffffff00u) != VIP_BASE)
        return -1;
    int i = (int)(h & 0xff) - 1;
    return i >= 0 && i < TPEERS && tpeers[i].used ? i : -1;
}
static u32 vip_of(int i) { return htonl(VIP_BASE + (u32)i + 1); }

/* "GTP2", kind (1 hello, 2 data), the room code's key (3 bytes), source port, destination port, payload */
#define TUN_HDR 12
static u32 tun_key;

static void tun_raw(int i, int kind, u16 sport, u16 dport, const void *p, u32 n)
{
    u8 b[2048];
    if (n > sizeof b - TUN_HDR)
        return;
    memcpy(b, "GTP2", 4);
    b[4] = (u8)kind;
    b[5] = (u8)(tun_key >> 16), b[6] = (u8)(tun_key >> 8), b[7] = (u8)tun_key;
    b[8] = (u8)(sport >> 8), b[9] = (u8)sport, b[10] = (u8)(dport >> 8), b[11] = (u8)dport;
    if (n)
        memcpy(b + TUN_HDR, p, n);
    struct sockaddr_in w = {0};
    w.sin_family = AF_INET;
    w.sin_addr.s_addr = tpeers[i].ip;
    w.sin_port = tpeers[i].port;
    sendto(tun, (const char *)b, (int)(n + TUN_HDR), 0, (struct sockaddr *)&w, sizeof w);
    tpeers[i].last_tx = GetTickCount();
}

static int tpeer_find(u32 ip, u16 port)
{
    for (int i = 0; i < TPEERS; i++)
        if (tpeers[i].used && tpeers[i].ip == ip && tpeers[i].port == port)
            return i;
    return -1;
}
static int tpeer_add(u32 ip, u16 port, int fixed)
{
    int i = tpeer_find(ip, port);
    if (i >= 0)
        return i;
    for (i = 0; i < TPEERS; i++)
        if (!tpeers[i].used) {
            tpeers[i] = (TPeer){ip, port, 1, fixed, GetTickCount(), 0};
            char a[16];
            inet_ntop(AF_INET, &ip, a, sizeof a);
            rt_log("[p2p] player %s:%u is 10.254.254.%d to the game\n", a, ntohs(port), i + 1);
            return i;
        }
    return -1;
}

static void sock_queue(u16 dport, u32 from_ip, u16 from_port, const u8 *p, u32 n)
{
    AcquireSRWLockExclusive(&socks_lock);
    for (int k = 0; k < SOCK_MAX; k++) {
        Sock *s = &socks[k];
        if (!s->used || s->type != 2 || s->lport != dport)
            continue;
        if (s->qn >= 256)
            break;
        Dgram *d = malloc(sizeof *d + n);
        d->next = NULL, d->from_ip = from_ip, d->from_port = htons(from_port), d->n = n;
        memcpy(d->data, p, n);
        if (s->qt)
            s->qt->next = d;
        else
            s->q = d;
        s->qt = d;
        s->qn++;
        break;
    }
    ReleaseSRWLockExclusive(&socks_lock);
}

/* RED_BULL_BOARD (board.c): a leaderboard to that player, through the tunnel (kind 3) */
void net_p2p_board(int i, const void *p, u32 n)
{
    if (!tun_on || i < 0 || i >= TPEERS)
        return;
    EnterCriticalSection(&tun_lock);
    if (tpeers[i].used)
        tun_raw(i, 3, 0, 0, p, n);
    LeaveCriticalSection(&tun_lock);
}

static void tun_poll(void)
{
    board_poll();                         /* RED_BULL_BOARD: the leaderboards to and from the other players */
    if (!tun_on)
        return;
    p2p_poll();
    EnterCriticalSection(&tun_lock);
    u8 b[2048];
    for (;;) {
        struct sockaddr_in w;
        int wl = sizeof w;
        int n = recvfrom(tun, (char *)b, sizeof b, 0, (struct sockaddr *)&w, &wl);
        if (n < 0)
            break;
        if (n < TUN_HDR || memcmp(b, "GTP2", 4))
            continue;
        if (((u32)b[5] << 16 | (u32)b[6] << 8 | b[7]) != tun_key) {
            static DWORD said;
            if (GetTickCount() - said > 10000) {
                said = GetTickCount();
                rt_log("[p2p] a packet with another room code's key: ignored (an old code, or not our game)\n");
            }
            continue;
        }
        int i = tpeer_find(w.sin_addr.s_addr, w.sin_port);
        if (i < 0)
            i = tpeer_add(w.sin_addr.s_addr, w.sin_port, 0);     /* a player joining with our code */
        if (i < 0)
            continue;
        tpeers[i].last_rx = GetTickCount();
        if (b[4] == 2) {
            sock_queue((u16)(b[10] << 8 | b[11]), vip_of(i), (u16)(b[8] << 8 | b[9]), b + TUN_HDR,
                       (u32)n - TUN_HDR);
            board_peer_tun(i);
        } else if (b[4] == 3) {
            board_rx_tun(i, b + TUN_HDR, (u32)n - TUN_HDR);    /* RED_BULL_BOARD: that player's leaderboard */
        }
    }
    DWORD now = GetTickCount();
    for (int i = 0; i < TPEERS; i++) {
        if (!tpeers[i].used)
            continue;
        if (!tpeers[i].fixed && now - tpeers[i].last_rx > 30000) {
            rt_log("[p2p] player 10.254.254.%d: silent for 30 s, dropped\n", i + 1);
            tpeers[i].used = 0;
            continue;
        }
        if (now - tpeers[i].last_tx >= 1000)
            tun_raw(i, 1, 0, 0, NULL, 0);                         /* hello: keeps the routers' mappings */
    }
    LeaveCriticalSection(&tun_lock);
}

/* p2p.c: open the tunnel on that port (0 = any); returns the port, 0 on failure */
u16 net_p2p_open(u16 port)
{
    if (!net_lan_on())
        return 0;
    tun_lock_init();
    EnterCriticalSection(&tun_lock);
    if (tun != INVALID_SOCKET && (!port || port == tun_port)) {
        LeaveCriticalSection(&tun_lock);
        tun_on = 1;
        return tun_port;
    }
    if (tun != INVALID_SOCKET)
        closesocket(tun);
    tun = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in w = {0};
    w.sin_family = AF_INET;
    w.sin_addr.s_addr = test_ip;
    w.sin_port = htons(port);
    if (bind(tun, (struct sockaddr *)&w, sizeof w)) {
        w.sin_port = 0;                                           /* taken: any port */
        bind(tun, (struct sockaddr *)&w, sizeof w);
    }
#ifdef _WIN32
    BOOL no = FALSE;
    DWORD got;
    WSAIoctl(tun, _WSAIOW(IOC_VENDOR, 12), &no, sizeof no, NULL, 0, &got, NULL, NULL);
#endif
    u_long nb = 1;
    ioctlsocket(tun, FIONBIO, &nb);
    int wl = sizeof w;
    getsockname(tun, (struct sockaddr *)&w, &wl);
    tun_port = ntohs(w.sin_port);
    tun_on = 1;
    LeaveCriticalSection(&tun_lock);
    rt_log("[p2p] tunnel on UDP %u\n", tun_port);
    return tun_port;
}
void net_p2p_reset(void)
{
    tun_lock_init();
    EnterCriticalSection(&tun_lock);
    memset(tpeers, 0, sizeof tpeers);
    LeaveCriticalSection(&tun_lock);
}
void net_p2p_stop(void)
{
    net_p2p_reset();
    tun_on = 0;
}
void net_p2p_set_key(u32 key) { tun_key = key & 0xffffff; }
void net_p2p_add_peer(u32 ip_be, u16 port_be)
{
    tun_lock_init();
    EnterCriticalSection(&tun_lock);
    int i = tpeer_add(ip_be, port_be, 1);
    if (i >= 0)
        tun_raw(i, 1, 0, 0, NULL, 0);
    LeaveCriticalSection(&tun_lock);
}

/* the game sends to a virtual address, or broadcasts: through the tunnel */
static int tun_send(Sock *s, u32 dst, u16 dport_be, const void *p, u32 n, int broadcast)
{
    if (!tun_on || s->type != 2)
        return 0;
    sock_bind_any(s);
    EnterCriticalSection(&tun_lock);
    int sent = 0, i = broadcast ? -1 : vip_index(dst);
    for (int k = 0; k < TPEERS; k++)
        if (tpeers[k].used && (broadcast || k == i)) {
            tun_raw(k, 2, s->lport, ntohs(dport_be), p, n);
            sent = 1;
        }
    LeaveCriticalSection(&tun_lock);
    return sent;
}

static int sock_pop(Sock *s, u32 p, u32 n, u32 a, u32 al)
{
    AcquireSRWLockExclusive(&socks_lock);
    Dgram *d = s->q;
    if (d) {
        s->q = d->next;
        if (!s->q)
            s->qt = NULL;
        s->qn--;
    }
    ReleaseSRWLockExclusive(&socks_lock);
    if (!d)
        return -1;
    u32 m = d->n < n ? d->n : n;
    memcpy(GP(p), d->data, m);
    struct sockaddr_in w = {0};
    w.sin_family = AF_INET;
    w.sin_addr.s_addr = d->from_ip;
    w.sin_port = d->from_port;
    sa_out(&w, a, al);
    free(d);
    return (int)m;
}

static u32 net_socket_new(int type)
{
    if (!net_lan_on())
        return fail();
    SOCKET h = socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, type == 1 ? IPPROTO_TCP : IPPROTO_UDP);
    if (h == INVALID_SOCKET)
        return werr();
    if (type != 1) {
        /* Windows reports an ICMP "port unreachable" as WSAECONNRESET on the next recvfrom; BSD does not */
#ifdef _WIN32
        BOOL no = FALSE;
        DWORD got;
        WSAIoctl(h, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no, sizeof no, NULL, 0, &got, NULL, NULL);
#endif
    }
    AcquireSRWLockExclusive(&socks_lock);
    for (int i = 0; i < SOCK_MAX; i++)
        if (!socks[i].used) {
            socks[i] = (Sock){.s = h, .used = 1, .type = type == 1 ? 1 : 2};
            ReleaseSRWLockExclusive(&socks_lock);
            return SOCK_BASE + (u32)i;
        }
    ReleaseSRWLockExclusive(&socks_lock);
    closesocket(h);
    return fail_with(24);
}

u32 h_socket(u32 dom, u32 type, u32 proto)
{
    (void)proto;
    if (!net_lan_on())
        return fail();
    if (dom != 2)
        return fail_with(47);                /* EAFNOSUPPORT: IPv4 only */
    if (type != 1 && type != 2)
        return fail_with(43);
    /* NO_TCP_WAIT: TCP stays offline (see the header) */
    if (type == 1 && !getenv("GT_LAN_TCP"))
        return fail();
    u32 fd = net_socket_new((int)type);
    if (GT_DEV && GT_DEV && nlog)
        rt_log("[net] socket(type %u) = %d (errno %u)\n", type, (int)fd, rt_thread()->errno_cell);
    return fd;
}

u32 net_close(u32 fd)
{
    AcquireSRWLockExclusive(&socks_lock);
    Sock *s = sk(fd);
    if (!s) {
        ReleaseSRWLockExclusive(&socks_lock);
        return fail_with(9);
    }
    SOCKET h = s->s;
    s->used = 0;
    Dgram *q = s->q;
    s->q = s->qt = NULL;
    s->qn = 0;
    s->lport = 0;
    ReleaseSRWLockExclusive(&socks_lock);
    while (q) {
        Dgram *n = q->next;
        free(q);
        q = n;
    }
    closesocket(h);
    return 0;
}

u32 h_bind(u32 fd, u32 a, u32 n)
{
    Sock *s = sk(fd);
    struct sockaddr_in w;
    if (!s)
        return fail_with(9);
    if (!sa_in(a, n, &w))
        return fail_with(47);
    if (test_ip && w.sin_addr.s_addr == 0)
        w.sin_addr.s_addr = test_ip;
    if (GT_DEV && GT_DEV && nlog)
        rt_log("[net] bind(%u, %s:%u)\n", fd, inet_ntoa(w.sin_addr), ntohs(w.sin_port));
    if (bind(s->s, (struct sockaddr *)&w, sizeof w))
        return werr();
    s->bound = 1;
    sock_note_port(s);
    if (s->type == 2) {
        p2p_bound(s->lport);
    }
    return 0;
}
u32 h_listen(u32 fd, u32 n)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    return listen(s->s, (int)n) ? werr() : 0;
}
u32 h_accept(u32 fd, u32 a, u32 n)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    struct sockaddr_in w;
    int wl = sizeof w;
    SOCKET h = accept(s->s, (struct sockaddr *)&w, &wl);
    if (h == INVALID_SOCKET)
        return werr();
    sa_out(&w, a, n);
    AcquireSRWLockExclusive(&socks_lock);
    for (int i = 0; i < SOCK_MAX; i++)
        if (!socks[i].used) {
            socks[i] = (Sock){.s = h, .used = 1, .type = 1, .bound = 1};
            ReleaseSRWLockExclusive(&socks_lock);
            return SOCK_BASE + (u32)i;
        }
    ReleaseSRWLockExclusive(&socks_lock);
    closesocket(h);
    return fail_with(24);
}
u32 h_connect(u32 fd, u32 a, u32 n)
{
    Sock *s = sk(fd);
    struct sockaddr_in w;
    if (!s)
        return fail_with(9);
    if (!sa_in(a, n, &w))
        return fail_with(47);
    if (!net_addr_allowed(w.sin_addr.s_addr))
        return fail();
    sock_bind_test(s);
    if (connect(s->s, (struct sockaddr *)&w, sizeof w)) {
        int e = WSAGetLastError();
        return fail_with(e == WSAEWOULDBLOCK ? 36 : (int)werr_map(e));      /* EINPROGRESS */
    }
    return 0;
}
u32 h_send(u32 fd, u32 p, u32 n, u32 f)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    int r = send(s->s, GP(p), (int)n, (int)(f & 1));
    return r < 0 ? werr() : (u32)r;
}
u32 h_recv(u32 fd, u32 p, u32 n, u32 f)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    int r = recv(s->s, GP(p), (int)n, (int)(f & 3));
    if (r < 0 && WSAGetLastError() == WSAEMSGSIZE)
        return n;                            /* BSD: the datagram is cut, no error */
    return r < 0 ? werr() : (u32)r;
}

/* a broadcast: every adapter's subnet broadcast (or nothing with GT_LAN_IP), then GT_LAN_PEERS */
int net_broadcast(SOCKET h, const void *p, int n, u16 port_be)
{
    struct sockaddr_in w = {0};
    w.sin_family = AF_INET;
    w.sin_port = port_be;
    int sent = -1;
    if (!test_ip) {
        Adapter ad[16];
        int na = adapters_copy(ad);
        for (int i = 0; i < na; i++) {
            if (ad[i].mask == 0xffffffffu)
                continue;
            w.sin_addr.s_addr = ad[i].addr | ~ad[i].mask;
            int r = sendto(h, p, n, 0, (struct sockaddr *)&w, sizeof w);
            if (r >= 0)
                sent = r;
        }
    }
    for (int i = 0; i < n_peers; i++) {
        if (peers[i] == test_ip)
            continue;
        w.sin_addr.s_addr = peers[i];
        int r = sendto(h, p, n, 0, (struct sockaddr *)&w, sizeof w);
        if (r >= 0)
            sent = r;
    }
    return sent;
}

u32 h_sendto(u32 fd, u32 p, u32 n, u32 f, u32 a, u32 al)
{
    Sock *s = sk(fd);
    struct sockaddr_in w;
    if (!s)
        return fail_with(9);
    if (!a)
        return h_send(fd, p, n, f);
    if (!sa_in(a, al, &w))
        return fail_with(47);
    sock_bind_test(s);
    static int logged;
    if (GT_DEV && nlog && logged < 60) {
        logged++;
        rt_log("[net] sendto(%u, %u bytes, %s:%u)%s\n", fd, n, inet_ntoa(w.sin_addr), ntohs(w.sin_port),
               s->type == 2 && is_broadcast(w.sin_addr.s_addr) ? " broadcast" : "");
    }
    tun_poll();
    if (s->type == 2 && is_broadcast(w.sin_addr.s_addr)) {
        int t = tun_send(s, 0, w.sin_port, GP(p), n, 1);  /* ONLINE_P2P: the internet players too */
        int r = net_broadcast(s->s, GP(p), (int)n, w.sin_port);
        if (r < 0 && (t || (!n_peers && test_ip)))
            return n;                        /* nobody to tell: as if it went out */
        return r < 0 ? werr() : n;
    }
    if (vip_index(w.sin_addr.s_addr) >= 0)
        return tun_send(s, w.sin_addr.s_addr, w.sin_port, GP(p), n, 0) ? n : fail();
    if (!net_addr_allowed(w.sin_addr.s_addr))
        return fail();
    if (s->type == 2 && !net_own_addr(w.sin_addr.s_addr))
        board_peer_lan(w.sin_addr.s_addr);   /* RED_BULL_BOARD: a player the game talks to (Local) */
    int r = sendto(s->s, GP(p), (int)n, (int)(f & 1), (struct sockaddr *)&w, sizeof w);
    if (r < 0 && GT_DEV && nlog)
        rt_log("[net] sendto failed: WSA %d\n", WSAGetLastError());
    return r < 0 ? werr() : (u32)r;
}
u32 h_recvfrom(u32 fd, u32 p, u32 n, u32 f, u32 a, u32 al)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    tun_poll();
    if (s->q) {
        int q = sock_pop(s, p, n, a, al);    /* ONLINE_P2P: from the tunnel first */
        if (q >= 0)
            return (u32)q;
    }
    struct sockaddr_in w = {0};
    int wl = sizeof w;
    int r = recvfrom(s->s, GP(p), (int)n, (int)(f & 3), (struct sockaddr *)&w, &wl);
    if (r < 0 && WSAGetLastError() == WSAEMSGSIZE)
        r = (int)n;
    if (r < 0)
        return werr();
    static int logged;
    if (GT_DEV && nlog && logged < 60) {
        logged++;
        rt_log("[net] recvfrom(%u) = %d from %s:%u\n", fd, r, inet_ntoa(w.sin_addr), ntohs(w.sin_port));
    }
    sa_out(&w, a, al);
    return (u32)r;
}

/* Darwin's fd_set: 1024 bits in u32 words; struct timeval: long seconds, int microseconds */
u32 h_select(u32 nfds, u32 r, u32 wr, u32 e, u32 t)
{
    u32 *sets[3] = {r ? GP(r) : NULL, wr ? GP(wr) : NULL, e ? GP(e) : NULL};
    fd_set win[3];
    u32 fds[3][64];
    int nf[3] = {0, 0, 0}, any = 0;
    SOCKET maxfd = 0;                        /* POSIX's select counts the descriptors (Winsock ignores it) */
    if (nfds > 1024)
        nfds = 1024;
    for (int k = 0; k < 3; k++) {
        FD_ZERO(&win[k]);
        if (!sets[k])
            continue;
        for (u32 fd = SOCK_BASE; fd < nfds && fd < SOCK_BASE + SOCK_MAX; fd++)
            if (sets[k][fd >> 5] >> (fd & 31) & 1) {
                Sock *s = sk(fd);
                if (s && nf[k] < 64) {
                    FD_SET(s->s, &win[k]);
                    if (s->s > maxfd)
                        maxfd = s->s;
                    fds[k][nf[k]++] = fd;
                    any = 1;
                }
            }
    }
    struct timeval tv, *tp = NULL;
    if (t) {
        tv.tv_sec = (long)rd32(t);
        tv.tv_usec = (long)rd32(t + 4);
        tp = &tv;
    }
    /* ONLINE_P2P: a socket with tunnel datagrams waiting is readable now */
    tun_poll();
    int queued = 0;
    for (int i = 0; i < nf[0]; i++) {
        Sock *s = sk(fds[0][i]);
        if (s && s->q)
            queued++;
    }
    if (queued && tp)
        tv.tv_sec = 0, tv.tv_usec = 0;
    else if (queued)
        tv.tv_sec = 0, tv.tv_usec = 0, tp = &tv;
    int got;
    if (!any) {
        if (tp)
            Sleep((DWORD)(tv.tv_sec * 1000 + tv.tv_usec / 1000));
        got = 0;
    } else {
        got = select((int)maxfd + 1, &win[0], &win[1], &win[2], tp);
        if (got < 0)
            return werr();
    }
    for (int k = 0; k < 3; k++) {
        if (!sets[k])
            continue;
        for (u32 i = 0; i < (nfds + 31) / 32; i++)
            sets[k][i] = 0;
        for (int i = 0; i < nf[k]; i++) {
            Sock *s = sk(fds[k][i]);
            if (!s)
                continue;
            int ready = FD_ISSET(s->s, &win[k]);
            if (k == 0 && s->q && !ready) {
                ready = 1;
                got++;
            }
            if (ready)
                sets[k][fds[k][i] >> 5] |= 1u << (fds[k][i] & 31);
        }
    }
    return (u32)got;
}

#ifndef _WIN32
/* Darwin's socket and IP option numbers (Winsock has the same as BSD) as Linux's */
static int so_opt(u32 opt)
{
    switch (opt) {
    case 0x0004: return SO_REUSEADDR;
    case 0x0008: return SO_KEEPALIVE;
    case 0x0010: return SO_DONTROUTE;
    case 0x0020: return SO_BROADCAST;
    case 0x0080: return SO_LINGER;
    case 0x0100: return SO_OOBINLINE;
    case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;
    case 0x1003: return SO_SNDLOWAT;
    case 0x1004: return SO_RCVLOWAT;
    case 0x1005: return SO_SNDTIMEO;
    case 0x1006: return SO_RCVTIMEO;
    case 0x1007: return SO_ERROR;
    case 0x1008: return SO_TYPE;
    }
    return -1;
}
static int ip_opt(u32 level, u32 opt)
{
    if (level != 0)                          /* IPPROTO_IP; TCP_NODELAY (6, 1) is the same number */
        return (int)opt;
    switch (opt) {
    case 3: return IP_TOS;
    case 4: return IP_TTL;
    case 9: return IP_MULTICAST_IF;
    case 10: return IP_MULTICAST_TTL;
    case 11: return IP_MULTICAST_LOOP;
    case 12: return IP_ADD_MEMBERSHIP;
    case 13: return IP_DROP_MEMBERSHIP;
    }
    return (int)opt;
}
#endif

u32 h_setsockopt(u32 fd, u32 level, u32 opt, u32 v, u32 n)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    int iv = v && n >= 4 ? (int)rd32(v) : 0;
    int r;
    if (level == 0xffff) {
        switch (opt) {
        case 0x1022:                         /* SO_NOSIGPIPE: Windows has no SIGPIPE */
            return 0;
        case 0x200:                          /* SO_REUSEPORT */
            opt = SO_REUSEADDR;
            break;
        case 0x80: {                         /* SO_LINGER: two ints on Darwin, two shorts here */
            struct linger l = {(u_short)rd32(v), (u_short)rd32(v + 4)};
            r = setsockopt(s->s, SOL_SOCKET, SO_LINGER, (const char *)&l, sizeof l);
            return r ? werr() : 0;
        }
        case 0x1005:                         /* SO_SNDTIMEO, SO_RCVTIMEO: a timeval there, ms here */
        case 0x1006: {
#ifndef _WIN32
            struct timeval tv = {(time_t)rd32(v), (suseconds_t)rd32(v + 4)};
            r = setsockopt(s->s, SOL_SOCKET, so_opt(opt), &tv, sizeof tv);
#else
            DWORD ms = rd32(v) * 1000 + rd32(v + 4) / 1000;
            r = setsockopt(s->s, SOL_SOCKET, (int)opt, (const char *)&ms, sizeof ms);
#endif
            return r ? werr() : 0;
        }
        }
#ifndef _WIN32
        if (opt == SO_REUSEADDR)             /* SO_REUSEPORT above: already Linux's number */
            opt = 0x0004;
        int lo = so_opt(opt);
        if (lo < 0)
            return 0;                        /* an option Linux has not: as if taken */
        r = setsockopt(s->s, SOL_SOCKET, lo, (const char *)&iv, sizeof iv);
#else
        r = setsockopt(s->s, SOL_SOCKET, (int)opt, (const char *)&iv, sizeof iv);
#endif
    } else {
#ifndef _WIN32
        r = setsockopt(s->s, (int)level, ip_opt(level, opt), GP(v), (socklen_t)n);
#else
        r = setsockopt(s->s, (int)level, (int)opt, GP(v), (int)n);    /* IP_*, TCP_NODELAY: same numbers */
#endif
    }
    if (GT_DEV && GT_DEV && nlog)
        rt_log("[net] setsockopt(%u, 0x%x, 0x%x, %d) = %d\n", fd, level, opt, iv, r ? WSAGetLastError() : 0);
    return r ? werr() : 0;
}
u32 h_getsockopt(u32 fd, u32 level, u32 opt, u32 v, u32 n)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    int iv = 0, il = sizeof iv;
#ifndef _WIN32
    int lo = level == 0xffff ? so_opt(opt) : ip_opt(level, opt);
    if (lo < 0)
        return fail_with(42);                /* ENOPROTOOPT */
    if (getsockopt(s->s, level == 0xffff ? SOL_SOCKET : (int)level, lo, (char *)&iv, (socklen_t *)&il))
        return werr();
#else
    if (getsockopt(s->s, level == 0xffff ? SOL_SOCKET : (int)level, (int)opt, (char *)&iv, &il))
        return werr();
#endif
    if (level == 0xffff && opt == 0x1007 && iv)       /* SO_ERROR */
        iv = (int)werr_map(iv);
    if (v)
        wr32(v, (u32)iv);
    if (n)
        wr32(n, 4);
    return 0;
}
u32 h_shutdown(u32 fd, u32 how)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    return shutdown(s->s, (int)how) ? werr() : 0;
}
u32 h_getpeername(u32 fd, u32 a, u32 n)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    struct sockaddr_in w;
    int wl = sizeof w;
    if (getpeername(s->s, (struct sockaddr *)&w, &wl))
        return werr();
    sa_out(&w, a, n);
    return 0;
}
u32 h_getsockname(u32 fd, u32 a, u32 n)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    struct sockaddr_in w;
    int wl = sizeof w;
    if (getsockname(s->s, (struct sockaddr *)&w, &wl))
        return werr();
    sa_out(&w, a, n);
    return 0;
}

/* fcntl / ioctl on a socket (imp_libc.c hands them over) */
u32 net_fcntl(u32 fd, u32 cmd, u32 arg)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    if (cmd == 3)                            /* F_GETFL */
        return 2u | (s->nonblock ? 4u : 0u);  /* O_RDWR | O_NONBLOCK */
    if (cmd == 4) {                          /* F_SETFL */
        u_long nb = (arg & 4) != 0;
        if (ioctlsocket(s->s, FIONBIO, &nb))
            return werr();
        s->nonblock = (int)nb;
    }
    return 0;
}
u32 net_ioctl(u32 fd, u32 req, u32 argp)
{
    Sock *s = sk(fd);
    if (!s)
        return fail_with(9);
    if (req == 0x8004667eu) {                /* FIONBIO */
        u_long nb = argp && rd32(argp);
        if (ioctlsocket(s->s, FIONBIO, &nb))
            return werr();
        s->nonblock = (int)nb;
        return 0;
    }
    if (req == 0x4004667fu) {                /* FIONREAD */
        u_long avail = 0;
        if (ioctlsocket(s->s, FIONREAD, &avail))
            return werr();
        if (argp)
            wr32(argp, (u32)avail);
        return 0;
    }
    return fail_with(25);                    /* ENOTTY */
}

u32 h_getaddrinfo(u32 node, u32 serv, u32 hints, u32 res)
{
    (void)serv, (void)hints;
    if (res)
        wr32(res, 0);
    if (rt_trace)
        rt_log("[net] getaddrinfo(%s): no network\n", node ? (const char *)GP(node) : "");
    return D_EAI_NONAME;
}
void h_freeaddrinfo(u32 ai) { (void)ai; }
u32 h_gethostbyname(u32 name) { (void)name; return 0; }

/* Darwin's struct ifaddrs (32-bit): next, name, flags, addr, netmask, broadaddr, data -- one block
   holding the list, its addresses and names, so freeifaddrs frees the first entry */
u32 h_getifaddrs(u32 out)
{
    if (!out)
        return fail_with(22);
    wr32(out, 0);
    if (!net_lan_on())
        return 0;
    Adapter ad[16];
    int n = adapters_copy(ad);
    if (!n)
        return 0;
    enum { ENT = 28, SA = 16, NAME = 8, ONE = ENT + 3 * SA + NAME };
    u8 *blk = calloc((size_t)n, ONE);
    if (!blk)
        return fail_with(12);
    for (int i = 0; i < n; i++) {
        u8 *e = blk + i * ONE, *sa = e + ENT, *nm = sa + 3 * SA;
        u32 bc = ad[i].addr | ~ad[i].mask;
        u8 *addr = sa, *mask = sa + SA, *brd = sa + 2 * SA;
        addr[0] = mask[0] = brd[0] = 16;
        addr[1] = mask[1] = brd[1] = 2;
        memcpy(addr + 4, &ad[i].addr, 4);
        memcpy(mask + 4, &ad[i].mask, 4);
        memcpy(brd + 4, &bc, 4);
        snprintf((char *)nm, NAME, "%s", ad[i].name);
        wr32(GA(e) + 0, i + 1 < n ? GA(blk + (i + 1) * ONE) : 0);
        wr32(GA(e) + 4, GA(nm));
        wr32(GA(e) + 8, 0x1u | 0x2u | 0x40u | 0x8000u);   /* UP, BROADCAST, RUNNING, MULTICAST */
        wr32(GA(e) + 12, GA(addr));
        wr32(GA(e) + 16, GA(mask));
        wr32(GA(e) + 20, GA(brd));
    }
    wr32(out, GA(blk));
    static int logged;
    if (!logged++) {
        char a[16];
        inet_ntop(AF_INET, &ad[0].addr, a, sizeof a);
        rt_log("[net] %d IPv4 adapter(s), en0 = %s\n", n, a);
    }
    return 0;
}
void h_freeifaddrs(u32 p)
{
    if (p && net_lan_on())
        free(GP(p));
}
/* en0 exists (index 1), with a fixed, locally administered MAC address: the game's ID code reads it
   through sysctl(NET_RT_IFLIST) and never expects that to fail (uikit.c's sysctl calls net_iflist) */
u32 h_if_nametoindex(u32 name) { return !strcmp(GP(name), "en0") ? 1 : 0; }

u32 net_iflist(u32 index, u32 oldp, u32 oldlenp)
{
    static const u8 mac[6] = {0x02, 0x00, 0x47, 0x54, 0x52, 0x31};
    u8 msg[0x70 + 20];
    memset(msg, 0, sizeof msg);
    msg[0] = sizeof msg;                 /* if_msghdr: ifm_msglen, version, type RTM_IFINFO, ... */
    msg[2] = 5;
    msg[3] = 0xe;
    msg[12] = (u8)index;
    u8 *dl = msg + 0x70;                 /* sockaddr_dl */
    dl[0] = 20;                          /* sdl_len */
    dl[1] = 18;                          /* AF_LINK */
    dl[2] = (u8)index;
    dl[4] = 6;                           /* IFT_ETHER */
    dl[5] = 3;                           /* sdl_nlen */
    dl[6] = 6;                           /* sdl_alen */
    memcpy(dl + 8, "en0", 3);
    memcpy(dl + 11, mac, 6);
    u32 room = oldlenp ? rd32(oldlenp) : 0;
    if (oldlenp)
        wr32(oldlenp, sizeof msg);
    if (!oldp)
        return 0;
    if (room < sizeof msg)
        return (u32)-1;
    memcpy(GP(oldp), msg, sizeof msg);
    return 0;
}

/* addresses (network byte order in the u32, as the game keeps them) */

static int parse4(const char *s, u8 out[4])
{
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return 0;
        unsigned v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (unsigned)(*s++ - '0');
        if (v > 255)
            return 0;
        out[i] = (u8)v;
        if (i < 3 && *s++ != '.')
            return 0;
    }
    return *s == 0;
}
u32 h_inet_addr(u32 s)
{
    u8 b[4];
    u32 v;
    if (!parse4(GP(s), b))
        return 0xffffffffu;
    memcpy(&v, b, 4);
    return v;
}
u32 h_inet_aton(u32 s, u32 out)
{
    u8 b[4];
    if (!parse4(GP(s), b))
        return 0;
    if (out)
        memcpy(GP(out), b, 4);
    return 1;
}
u32 h_inet_ntoa(u32 a)
{
    static __thread char buf[16];
    const u8 *b = (const u8 *)&a;
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return GA(buf);
}
u32 h_inet_ntop(u32 af, u32 src, u32 dst, u32 size)
{
    char tmp[64];
    if (af == 2) {                 /* AF_INET */
        const u8 *b = GP(src);
        snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    } else if (af == 30) {         /* AF_INET6 on Darwin */
        const u8 *b = GP(src);
        int n = 0;
        for (int i = 0; i < 16; i += 2)
            n += snprintf(tmp + n, sizeof tmp - (size_t)n, "%s%x", i ? ":" : "", b[i] << 8 | b[i + 1]);
    } else {
        return 0;
    }
    if (strlen(tmp) + 1 > size)
        return 0;
    strcpy(GP(dst), tmp);
    return dst;
}
u32 h_inet_pton(u32 af, u32 s, u32 dst)
{
    if (af != 2)
        return 0;
    u8 b[4];
    if (!parse4(GP(s), b))
        return 0;
    memcpy(GP(dst), b, 4);
    return 1;
}

/* NSURLRequest, NSURLConnection: every connection fails as offline
   Asynchronously, like iOS without a network: the delegate gets connection:didFailWithError: with
   NSURLErrorNotConnectedToInternet on the run loop of the thread that started it.  SDKs that keep
   a thread per request (Burstly) stop it on that callback. */

#define SELF c->r[0]
#define ARG(i) arg_word(c, 2 + (i))

typedef struct { u32 isa, url, method, headers, body; double timeout; u32 cache; } Req;
typedef struct { u32 isa, request, delegate, started, cancelled; } Conn;
#define RQ(o) ((Req *)GP(o))
#define CN(o) ((Conn *)GP(o))

static void rq_init(id r, id url, double timeout)
{
    RQ(r)->url = objc_retain(url);
    RQ(r)->method = objc_retain(ns_string("GET"));
    RQ(r)->headers = ns_dict_new();
    RQ(r)->timeout = timeout;
}
static void rq_alloc(CPU *c) { ret_i(c, ns_alloc(SELF, sizeof(Req))); }
static void rq_withURL(CPU *c)
{
    id r = ns_alloc(SELF, sizeof(Req));
    rq_init(r, ARG(0), c->r[1] == sel_get("requestWithURL:") ? 60.0 : arg_d(c, 4));
    ret_i(c, objc_autorelease(r));
}
static void rq_initWithURL(CPU *c)
{
    rq_init(SELF, ARG(0), c->r[1] == sel_get("initWithURL:") ? 60.0 : arg_d(c, 4));
    ret_i(c, SELF);
}
static void rq_url(CPU *c) { ret_i(c, RQ(SELF)->url); }
static void rq_method(CPU *c) { ret_i(c, RQ(SELF)->method); }
static void rq_setMethod(CPU *c)
{
    id o = RQ(SELF)->method;
    RQ(SELF)->method = objc_retain(msg0(ARG(0), "copy"));
    objc_release(o);
}
static void rq_body(CPU *c) { ret_i(c, RQ(SELF)->body); }
static void rq_setBody(CPU *c)
{
    id o = RQ(SELF)->body;
    RQ(SELF)->body = objc_retain(ARG(0));
    objc_release(o);
}
static void rq_headers(CPU *c) { ret_i(c, RQ(SELF)->headers); }
static void rq_setHeader(CPU *c)
{
    if (ARG(0) && ARG(1))
        ns_dict_set(RQ(SELF)->headers, ARG(1), ARG(0));
}
static void rq_header(CPU *c) { ret_i(c, ns_dict_get(RQ(SELF)->headers, ARG(0))); }
static void rq_timeout(CPU *c) { ret_d(c, RQ(SELF)->timeout); }
static void rq_setTimeout(CPU *c) { RQ(SELF)->timeout = arg_d(c, 2); }
static void rq_nop(CPU *c) { (void)c; }
static void rq_copy(CPU *c)
{
    id r = ns_alloc(rd32(SELF), sizeof(Req));
    rq_init(r, RQ(SELF)->url, RQ(SELF)->timeout);
    msg1(r, "setHTTPMethod:", RQ(SELF)->method);
    RQ(r)->body = objc_retain(RQ(SELF)->body);
    ret_i(c, r);
}
static void rq_dealloc(CPU *c)
{
    Req *r = RQ(SELF);
    objc_release(r->url);
    objc_release(r->method);
    objc_release(r->headers);
    objc_release(r->body);
    objc_free_instance(SELF);
}

static id offline_error(void) { return ns_error("NSURLErrorDomain", -1009); }

static void cn_alloc(CPU *c) { ret_i(c, ns_alloc(SELF, sizeof(Conn))); }
static void cn_start_now(id conn)
{
    if (CN(conn)->started)
        return;
    CN(conn)->started = 1;
    ns_perform_here(conn, "gtFail", 0, 0.05);
}
static void cn_initWith(CPU *c)
{
    /* initWithRequest:delegate:[startImmediately:] */
    id self = SELF;
    CN(self)->request = objc_retain(ARG(0));
    CN(self)->delegate = objc_retain(ARG(1));
    int now = c->r[1] == sel_get("initWithRequest:delegate:startImmediately:") ? (ARG(2) & 0xff) : 1;
    if (rt_trace || 1) {
        static int n;
        if (n++ < 20)
            rt_log("[net] NSURLConnection to %s: offline\n", ns_describe(msg0(ARG(0), "URL")));
    }
    if (now)
        cn_start_now(self);
    ret_i(c, self);
}
static void cn_with(CPU *c)
{
    /* +connectionWithRequest:delegate: */
    id o = ns_alloc(SELF, sizeof(Conn));
    u32 w[2] = {ARG(0), ARG(1)};
    objc_msg(o, "initWithRequest:delegate:", 2, w);
    ret_i(c, objc_autorelease(o));
}
static void cn_start(CPU *c) { cn_start_now(SELF); }
static void cn_cancel(CPU *c) { CN(SELF)->cancelled = 1; }
static void cn_fail(CPU *c)
{
    id self = SELF;
    Conn *x = CN(self);
    if (x->cancelled || !x->delegate)
        return;
    objc_retain(self);
    if (objc_responds(x->delegate, "connection:didFailWithError:"))
        msg2(x->delegate, "connection:didFailWithError:", self, offline_error());
    objc_release(self);
}
static void cn_sync(CPU *c)
{
    /* +sendSynchronousRequest:returningResponse:error: */
    if (ARG(1))
        wr32(ARG(1), 0);
    if (ARG(2))
        wr32(ARG(2), offline_error());
    ret_i(c, 0);
}
static void cn_async(CPU *c)
{
    /* +sendAsynchronousRequest:queue:completionHandler: -- the handler(response, data, error) later */
    u32 b = gt_block_copy(ARG(2));
    if (b)
        rt_call4(rd32(b + 12), b, 0, 0, offline_error());
}
static void cn_canHandle(CPU *c) { ret_i(c, 1); }
static void cn_dealloc(CPU *c)
{
    objc_release(CN(SELF)->request);
    objc_release(CN(SELF)->delegate);
    objc_free_instance(SELF);
}
static void cookies_shared(CPU *c)
{
    static id jar;
    if (!jar)
        jar = objc_retain(ns_alloc(ns_class("NSHTTPCookieStorage"), 8));
    ret_i(c, jar);
}
static void cookies_none(CPU *c) { ret_i(c, objc_autorelease(ns_array_new(NULL, 0))); }

void gt_net_init(void)
{
    static const NsMethod rq[] = {
        {"alloc", rq_alloc, 1}, {"requestWithURL:", rq_withURL, 1},
        {"requestWithURL:cachePolicy:timeoutInterval:", rq_withURL, 1}, {"initWithURL:", rq_initWithURL, 0},
        {"initWithURL:cachePolicy:timeoutInterval:", rq_initWithURL, 0}, {"URL", rq_url, 0},
        {"HTTPMethod", rq_method, 0}, {"HTTPBody", rq_body, 0}, {"allHTTPHeaderFields", rq_headers, 0},
        {"valueForHTTPHeaderField:", rq_header, 0}, {"timeoutInterval", rq_timeout, 0}, {"copyWithZone:", rq_copy, 0},
        {"mutableCopyWithZone:", rq_copy, 0}, {"dealloc", rq_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("NSURLRequest", rq);
    static const NsMethod mrq[] = {
        {"setHTTPMethod:", rq_setMethod, 0}, {"setHTTPBody:", rq_setBody, 0},
        {"setValue:forHTTPHeaderField:", rq_setHeader, 0}, {"addValue:forHTTPHeaderField:", rq_setHeader, 0},
        {"setTimeoutInterval:", rq_setTimeout, 0}, {"setCachePolicy:", rq_nop, 0}, {"setURL:", rq_nop, 0},
        {"setHTTPShouldHandleCookies:", rq_nop, 0}, {"setAllHTTPHeaderFields:", rq_nop, 0},
        {"setHTTPShouldUsePipelining:", rq_nop, 0}, {"setMainDocumentURL:", rq_nop, 0}, {NULL, NULL, 0}};
    ns_methods("NSMutableURLRequest", mrq);
    static const NsMethod cn[] = {
        {"alloc", cn_alloc, 1}, {"initWithRequest:delegate:", cn_initWith, 0},
        {"initWithRequest:delegate:startImmediately:", cn_initWith, 0}, {"connectionWithRequest:delegate:", cn_with, 1},
        {"start", cn_start, 0}, {"cancel", cn_cancel, 0}, {"gtFail", cn_fail, 0},
        {"scheduleInRunLoop:forMode:", rq_nop, 0}, {"unscheduleFromRunLoop:forMode:", rq_nop, 0},
        {"sendSynchronousRequest:returningResponse:error:", cn_sync, 1},
        {"sendAsynchronousRequest:queue:completionHandler:", cn_async, 1}, {"canHandleRequest:", cn_canHandle, 1},
        {"dealloc", cn_dealloc, 0}, {NULL, NULL, 0}};
    ns_methods("NSURLConnection", cn);
    static const NsMethod jar[] = {
        {"sharedHTTPCookieStorage", cookies_shared, 1}, {"cookies", cookies_none, 0},
        {"cookiesForURL:", cookies_none, 0}, {"setCookie:", rq_nop, 0}, {"deleteCookie:", rq_nop, 0},
        {"setCookieAcceptPolicy:", rq_nop, 0}, {NULL, NULL, 0}};
    ns_methods("NSHTTPCookieStorage", jar);
}

/* SCNetworkReachability: named hosts never reachable; the local Wi-Fi with LAN_MULTIPLAYER */

u32 h_SCNetworkReachabilityCreateWithName(u32 alloc, u32 name)
{
    (void)alloc, (void)name;
    return objc_retain(ns_alloc(ns_class("SCNetworkReachability"), 8));
}
u32 h_SCNetworkReachabilityCreateWithAddress(u32 alloc, u32 addr)
{
    (void)alloc;
    id r = objc_retain(ns_alloc(ns_class("SCNetworkReachability"), 8));
    /* 169.254.0.0: Apple's "reachability for local Wi-Fi", the game's testWifiConnection (0x2f7624) */
    const u8 *b = addr ? GP(addr) : NULL;
    if (b && b[1] == 2 && b[4] == 169 && b[5] == 254 && net_lan_on()) {
        Adapter ad[16];
        if (adapters_copy(ad))
            wr32(r + 4, 0x20002u);           /* kSCNetworkReachabilityFlagsReachable | IsDirect */
    }
    /* 0.0.0.0 from the multiplayer menus' IsNetworkReachable (F_3075d8, called by ServerTypeMenu and the
       network managers only; Host server said "No network found" without it).  The same question from
       anywhere else (ads, Gameloft Live, ServerBasedFacade) still gets "not reachable". */
    if (b && b[1] == 2 && !b[4] && !b[5] && !b[6] && !b[7] && rt_cpu()->r[14] == 0x307618u && net_lan_on()) {
        Adapter ad[16];
        if (adapters_copy(ad))
            wr32(r + 4, 0x2u);               /* Reachable (not WWAN) */
    }
    return r;
}
u32 h_SCNetworkReachabilityGetFlags(u32 r, u32 flags)
{
    if (flags)
        wr32(flags, r ? rd32(r + 4) : 0);
    return 1;
}
u32 h_SCNetworkReachabilitySetCallback(u32 r, u32 cb, u32 ctx) { (void)r, (void)cb, (void)ctx; return 1; }
u32 h_SCNetworkReachabilityScheduleWithRunLoop(u32 r, u32 rl, u32 mode) { (void)r, (void)rl, (void)mode; return 1; }
u32 h_SCNetworkReachabilityUnscheduleFromRunLoop(u32 r, u32 rl, u32 mode) { (void)r, (void)rl, (void)mode; return 1; }
