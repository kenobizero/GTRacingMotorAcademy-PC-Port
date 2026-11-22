/* gk.c -- GameKit's GKSession over the local network: the game's Bluetooth multiplayer on PC.

   LAN_MULTIPLAYER (net.c; GT_LAN_MULTIPLAYER=0 turns both Local and Bluetooth off).
   The game's Bluetooth mode (NetworkManagerBT, BTController 0x2ad364..0x2adc2c)
   talks to GameKit only: GKSession initWithSessionID:@"GTRacing" displayName:sessionMode:, setAvailable:,
   connectToPeer:withTimeout:, acceptConnectionFromPeer:error:, sendData:toPeers:withDataMode:error:,
   displayNameForPeer:, disconnectFromAllPeers, and the delegate's session:peer:didChangeState: /
   didReceiveConnectionRequestFromPeer: / connectionWithPeerFailed:withError: / didFailWithError:, with
   the data handler's receiveData:fromPeer:inSession:context:.  Peer IDs are numeric strings (the game
   takes their intValue).  Windows has no GameKit (and Bluetooth between PCs is rare), so the same
   session runs over the network the Local mode uses:
     - a session that is available in server or peer mode sends a beacon every second to UDP port 28191
       (net_broadcast: every subnet + GT_LAN_PEERS), with its peer ID, mode, session ID, display name and
       its TCP port;  a client- or peer-mode session with the same session ID reports the sender as
       Available, and as Unavailable when beacons stop for 3.5 s;
     - connectToPeer opens TCP to that port and sends HELLO; the other side's delegate gets the
       connection request, acceptConnectionFromPeer answers ACCEPT, and both report Connected;
     - data goes as frames on that connection (reliable and unreliable alike: TCP on a LAN);
       a closed connection reports Disconnected.
   Everything runs from a perform on the thread that made the session (like GameKit's main run loop
   callbacks), every 15 ms while a session lives: no extra thread, no locks.  With the network off the
   session fails at once (session:didFailWithError:), which the game shows as "Bluetooth is off". */
#include "plat_net.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "net.h"
#include "game_hooks.h"

#define GK_UDP_PORT 28191
#define BEACON_MS 1000
#define LOST_MS 3500
#define MAX_SESS 8
#define MAX_SEEN 32
#define MAX_CONN 16

enum { GKS_AVAILABLE, GKS_UNAVAILABLE, GKS_CONNECTED, GKS_DISCONNECTED, GKS_CONNECTING };
enum { M_SERVER, M_CLIENT, M_PEER };
enum { F_HELLO = 1, F_ACCEPT, F_DENY, F_DATA, F_BYE };
enum { C_FREE, C_DIALING, C_WAIT_ACCEPT, C_INCOMING, C_CONNECTED };

typedef struct { u32 id, ip; u16 port_be; char name[64]; DWORD seen; } Seen;
typedef struct {
    int state, tell_connected;
    SOCKET s;
    u32 peer;
    char name[64];
    DWORD deadline;
    u8 *rx, *tx;
    u32 rxn, rxcap, txn, txcap;
} Conn;
typedef struct {
    id obj;
    u32 me;
    char sid[64], name[64];
    int mode, available, failed_told;
    id delegate, handler;
    u32 ctx;
    Seen seen[MAX_SEEN];
    int n_seen;
    Conn conns[MAX_CONN];
    DWORD beacon_at;
} Sess;
typedef struct { u32 isa; Sess *s; } GkObj;
#define GS(o) (((GkObj *)GP(o))->s)

static Sess *sessions[MAX_SESS];
static SOCKET udp = INVALID_SOCKET, tcp_listen = INVALID_SOCKET;
static u16 tcp_port_be;
static SOCKET pending[8];                 /* accepted, HELLO not read yet */
static u8 pending_buf[8][256];
static u32 pending_n[8];
static id pump_obj;
static int pump_on;

/* BACKGROUND_THROTTLE (uikit.c): a Bluetooth (GKSession) game is open: the window keeps its full frame rate */
int gk_sessions_open(void)
{
    for (int i = 0; i < MAX_SESS; i++)
        if (sessions[i])
            return 1;
    return 0;
}

/* helpers */

static void log_gk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_gk(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    rt_log("[gk] %s\n", b);
}

static u32 new_peer_id(void)
{
    static u64 x;
    if (!x) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        x = (u64)q.QuadPart ^ ((u64)GetCurrentProcessId() << 32) ^ 0x9e3779b97f4a7c15ull;
    }
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return (u32)(x % 2000000000u) + 100000000u;     /* ten digits like an iPad's */
}

static id peer_new(u32 pid)        /* +1 */
{
    char b[16];
    snprintf(b, sizeof b, "%u", pid);
    return ns_string_new(b, (u32)strlen(b));
}
static u32 peer_of(id s) { return s ? (u32)strtoul(ns_utf8(s), NULL, 10) : 0; }

static void nonblock(SOCKET s)
{
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
}

static int transport_up(void)
{
    if (udp != INVALID_SOCKET)
        return 1;
    if (!net_lan_on())
        return 0;
    u32 ip = net_test_ip();
    struct sockaddr_in w = {0};
    w.sin_family = AF_INET;
    w.sin_addr.s_addr = ip;
    w.sin_port = htons(GK_UDP_PORT);
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
    if (u == INVALID_SOCKET || bind(u, (struct sockaddr *)&w, sizeof w)) {
        log_gk("UDP port %d busy (%d): no discovery", GK_UDP_PORT, WSAGetLastError());
        if (u != INVALID_SOCKET)
            closesocket(u);
        return 0;
    }
#ifdef _WIN32
    BOOL no = FALSE;
    DWORD got;
    WSAIoctl(u, _WSAIOW(IOC_VENDOR, 12), &no, sizeof no, NULL, 0, &got, NULL, NULL);
#endif
    nonblock(u);
    SOCKET t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    w.sin_port = 0;
    int wl = sizeof w;
    if (t == INVALID_SOCKET || bind(t, (struct sockaddr *)&w, sizeof w) || listen(t, 8) ||
        getsockname(t, (struct sockaddr *)&w, &wl)) {
        log_gk("TCP listen failed (%d)", WSAGetLastError());
        closesocket(u);
        if (t != INVALID_SOCKET)
            closesocket(t);
        return 0;
    }
    nonblock(t);
    udp = u;
    tcp_listen = t;
    tcp_port_be = w.sin_port;
    for (int i = 0; i < 8; i++)
        pending[i] = INVALID_SOCKET;
    log_gk("transport up: UDP %d, TCP %d", GK_UDP_PORT, ntohs(tcp_port_be));
    return 1;
}

/* frames */

static void conn_close(Conn *k)
{
    if (k->s != INVALID_SOCKET)
        closesocket(k->s);
    free(k->rx);
    free(k->tx);
    memset(k, 0, sizeof *k);
    k->s = INVALID_SOCKET;
    k->state = C_FREE;
}

static void conn_flush(Conn *k)
{
    while (k->txn) {
        int r = send(k->s, (const char *)k->tx, (int)k->txn, 0);
        if (r <= 0)
            return;                          /* would block (or broken: recv sees it) */
        memmove(k->tx, k->tx + r, k->txn - (u32)r);
        k->txn -= (u32)r;
    }
}

static void conn_put(Conn *k, int type, const void *p, u32 n)
{
    u32 need = k->txn + 5 + n;
    if (need > k->txcap) {
        k->txcap = need + 4096;
        k->tx = realloc(k->tx, k->txcap);
    }
    u8 *d = k->tx + k->txn;
    u32 len = htonl(n + 1);
    memcpy(d, &len, 4);
    d[4] = (u8)type;
    if (n)
        memcpy(d + 5, p, n);
    k->txn = need;
    conn_flush(k);
}

/* HELLO: from, to, session ID, display name */
static void send_hello(Sess *s, Conn *k)
{
    u8 b[160];
    u32 n = 0, from = htonl(s->me), to = htonl(k->peer);
    memcpy(b, &from, 4), memcpy(b + 4, &to, 4), n = 8;
    u8 sl = (u8)strlen(s->sid), nl = (u8)strlen(s->name);
    b[n++] = sl, memcpy(b + n, s->sid, sl), n += sl;
    b[n++] = nl, memcpy(b + n, s->name, nl), n += nl;
    conn_put(k, F_HELLO, b, n);
}

/* delegate calls (the session kept alive across each) */

static void tell_state(Sess *s, u32 peer, int state)
{
    if (!s->delegate || !objc_responds(s->delegate, "session:peer:didChangeState:"))
        return;
    id p = peer_new(peer);
    u32 w[3] = {s->obj, p, (u32)state};
    log_gk("peer %u -> state %d", peer, state);
    objc_msg(s->delegate, "session:peer:didChangeState:", 3, w);
    objc_release(p);
}
static void tell_request(Sess *s, u32 peer)
{
    if (!s->delegate || !objc_responds(s->delegate, "session:didReceiveConnectionRequestFromPeer:"))
        return;
    id p = peer_new(peer);
    log_gk("connection request from %u", peer);
    msg2(s->delegate, "session:didReceiveConnectionRequestFromPeer:", s->obj, p);
    objc_release(p);
}
static void tell_failed(Sess *s, u32 peer)
{
    if (!s->delegate || !objc_responds(s->delegate, "session:connectionWithPeerFailed:withError:"))
        return;
    id p = peer_new(peer);
    u32 w[3] = {s->obj, p, ns_error("com.apple.gamekit.GKSessionErrorDomain", 30509)};
    log_gk("connection with %u failed", peer);
    objc_msg(s->delegate, "session:connectionWithPeerFailed:withError:", 3, w);
    objc_release(p);
}
static void tell_data(Sess *s, u32 peer, const u8 *p, u32 n)
{
    if (!s->handler)
        return;
    id d = ns_data_new(p, n), ps = peer_new(peer);
    u32 w[4] = {d, ps, s->obj, s->ctx};
    objc_msg(s->handler, "receiveData:fromPeer:inSession:context:", 4, w);
    objc_release(ps);
    objc_release(d);
}

/* the pump */

static Sess *sess_find(u32 me)
{
    for (int i = 0; i < MAX_SESS; i++)
        if (sessions[i] && sessions[i]->me == me)
            return sessions[i];
    return NULL;
}
static int sess_alive(Sess *s)
{
    for (int i = 0; i < MAX_SESS; i++)
        if (sessions[i] == s)
            return 1;
    return 0;
}
static Seen *seen_find(Sess *s, u32 id_)
{
    for (int i = 0; i < s->n_seen; i++)
        if (s->seen[i].id == id_)
            return &s->seen[i];
    return NULL;
}
static Conn *conn_find(Sess *s, u32 peer)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (s->conns[i].state != C_FREE && s->conns[i].peer == peer)
            return &s->conns[i];
    return NULL;
}
static Conn *conn_new(Sess *s)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (s->conns[i].state == C_FREE) {
            memset(&s->conns[i], 0, sizeof s->conns[i]);
            s->conns[i].s = INVALID_SOCKET;
            return &s->conns[i];
        }
    return NULL;
}

static void beacons_send(void)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_SESS; i++) {
        Sess *s = sessions[i];
        if (!s || !s->available || s->mode == M_CLIENT || now - s->beacon_at < BEACON_MS)
            continue;
        s->beacon_at = now;
        u8 b[160];
        u32 n = 0, me = htonl(s->me);
        memcpy(b, "GKS1", 4), n = 4;
        b[n++] = 1;
        memcpy(b + n, &me, 4), n += 4;
        b[n++] = (u8)s->mode;
        memcpy(b + n, &tcp_port_be, 2), n += 2;
        u8 sl = (u8)strlen(s->sid), nl = (u8)strlen(s->name);
        b[n++] = sl, memcpy(b + n, s->sid, sl), n += sl;
        b[n++] = nl, memcpy(b + n, s->name, nl), n += nl;
        net_broadcast(udp, b, (int)n, htons(GK_UDP_PORT));
    }
}

static void beacons_read(void)
{
    u8 b[512];
    struct sockaddr_in from;
    for (;;) {
        int fl = sizeof from;
        int n = recvfrom(udp, (char *)b, sizeof b, 0, (struct sockaddr *)&from, &fl);
        if (n < 0)
            break;
        if (n < 14 || memcmp(b, "GKS1", 4) || b[4] != 1)
            continue;
        u32 id_;
        u16 port;
        memcpy(&id_, b + 5, 4);
        id_ = ntohl(id_);
        memcpy(&port, b + 10, 2);
        u32 sl = b[12];
        if (13 + sl >= (u32)n)
            continue;
        char sid[64], name[64];
        snprintf(sid, sizeof sid, "%.*s", (int)sl, (const char *)b + 13);
        u32 nl = b[13 + sl];
        if (14 + sl + nl > (u32)n)
            continue;
        snprintf(name, sizeof name, "%.*s", (int)nl, (const char *)b + 14 + sl);
        if (sess_find(id_))
            continue;                        /* one of ours */
        for (int i = 0; i < MAX_SESS; i++) {
            Sess *s = sessions[i];
            if (!s || !s->available || s->mode == M_SERVER || strcmp(s->sid, sid))
                continue;
            Seen *x = seen_find(s, id_);
            int fresh = !x;
            if (!x) {
                if (s->n_seen >= MAX_SEEN)
                    continue;
                x = &s->seen[s->n_seen++];
                x->id = id_;
            }
            x->ip = from.sin_addr.s_addr;
            x->port_be = port;
            snprintf(x->name, sizeof x->name, "%s", name);
            x->seen = GetTickCount();
            if (fresh && !conn_find(s, id_)) {
                char a[16];
                inet_ntop(AF_INET, &x->ip, a, sizeof a);
                log_gk("found player %u at %s", id_, a);    /* no names in the log (privacy) */
                tell_state(s, id_, GKS_AVAILABLE);
                if (!sess_alive(s))
                    break;
            }
        }
    }
}

static void seen_expire(Sess *s)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < s->n_seen; i++)
        if (now - s->seen[i].seen > LOST_MS) {
            u32 id_ = s->seen[i].id;
            s->seen[i] = s->seen[--s->n_seen];
            if (!conn_find(s, id_)) {
                tell_state(s, id_, GKS_UNAVAILABLE);
                return;                      /* one per pump: the session may be gone */
            }
        }
}

static void accept_new(void)
{
    for (;;) {
        struct sockaddr_in w;
        int wl = sizeof w;
        SOCKET a = accept(tcp_listen, (struct sockaddr *)&w, &wl);
        if (a == INVALID_SOCKET)
            return;
        nonblock(a);
        BOOL yes = TRUE;
        setsockopt(a, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);
        int i;
        for (i = 0; i < 8 && pending[i] != INVALID_SOCKET; i++)
            ;
        if (i == 8) {
            closesocket(a);
            continue;
        }
        pending[i] = a;
        pending_n[i] = 0;
    }
}

/* an accepted connection's HELLO names the session it is for */
static void pending_read(void)
{
    for (int i = 0; i < 8; i++) {
        if (pending[i] == INVALID_SOCKET)
            continue;
        int r = recv(pending[i], (char *)pending_buf[i] + pending_n[i], (int)(sizeof pending_buf[i] - pending_n[i]), 0);
        if (r == 0 || (r < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
            closesocket(pending[i]);
            pending[i] = INVALID_SOCKET;
            continue;
        }
        if (r > 0)
            pending_n[i] += (u32)r;
        u8 *b = pending_buf[i];
        if (pending_n[i] < 5)
            continue;
        u32 len;
        memcpy(&len, b, 4);
        len = ntohl(len);
        if (len < 10 || len > 200 || b[4] != F_HELLO) {
            closesocket(pending[i]);
            pending[i] = INVALID_SOCKET;
            continue;
        }
        if (pending_n[i] < 4 + len)
            continue;
        u32 from, to;
        memcpy(&from, b + 5, 4), memcpy(&to, b + 9, 4);
        from = ntohl(from), to = ntohl(to);
        u32 sl = b[13];
        char name[64] = "";
        if (14 + sl < 4 + len) {
            u32 nl = b[14 + sl];
            snprintf(name, sizeof name, "%.*s", (int)nl, (const char *)b + 15 + sl);
        }
        Sess *s = sess_find(to);
        Conn *k = s ? conn_new(s) : NULL;
        if (!k || !s->delegate) {
            static const u8 deny[5] = {0, 0, 0, 1, F_DENY};
            send(pending[i], (const char *)deny, 5, 0);
            closesocket(pending[i]);
            pending[i] = INVALID_SOCKET;
            continue;
        }
        k->s = pending[i];
        k->peer = from;
        k->state = C_INCOMING;
        snprintf(k->name, sizeof k->name, "%s", name);
        /* anything after the HELLO is the peer's data */
        u32 rest = pending_n[i] - (4 + len);
        if (rest) {
            k->rxcap = rest + 4096;
            k->rx = malloc(k->rxcap);
            memcpy(k->rx, b + 4 + len, rest);
            k->rxn = rest;
        }
        pending[i] = INVALID_SOCKET;
        tell_request(s, from);
        return;                              /* the session may be gone now */
    }
}

/* one session's connections: dialing, frames in; returns 0 when a callback ended the pass */
static int conns_step(Sess *s)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < MAX_CONN; i++) {
        Conn *k = &s->conns[i];
        if (k->state == C_FREE)
            continue;
        if (k->tell_connected) {
            k->tell_connected = 0;
            tell_state(s, k->peer, GKS_CONNECTED);
            return 0;
        }
        if (k->state == C_DIALING) {
            fd_set wf, ef;
            FD_ZERO(&wf);
            FD_ZERO(&ef);
            FD_SET(k->s, &wf);
            FD_SET(k->s, &ef);
            struct timeval tv = {0, 0};
            int r = select(0, NULL, &wf, &ef, &tv);
            if (r > 0 && FD_ISSET(k->s, &wf)) {
                k->state = C_WAIT_ACCEPT;
                send_hello(s, k);
            } else if ((r > 0 && FD_ISSET(k->s, &ef)) || (int)(now - k->deadline) > 0) {
                u32 p = k->peer;
                conn_close(k);
                tell_failed(s, p);
                return 0;
            }
            continue;
        }
        if (k->state == C_WAIT_ACCEPT && (int)(now - k->deadline) > 0) {
            u32 p = k->peer;
            conn_close(k);
            tell_failed(s, p);
            return 0;
        }
        conn_flush(k);
        /* frames in */
        for (;;) {
            if (k->rxcap - k->rxn < 4096) {
                k->rxcap = k->rxcap * 2 + 8192;
                k->rx = realloc(k->rx, k->rxcap);
            }
            int r = recv(k->s, (char *)k->rx + k->rxn, (int)(k->rxcap - k->rxn), 0);
            if (r > 0) {
                k->rxn += (u32)r;
                continue;
            }
            if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK) {
                u32 p = k->peer;
                int was = k->state;
                conn_close(k);
                if (was == C_CONNECTED)
                    tell_state(s, p, GKS_DISCONNECTED);
                else if (was == C_WAIT_ACCEPT)
                    tell_failed(s, p);
                return 0;
            }
            break;
        }
        while (k->rxn >= 5) {
            u32 len;
            memcpy(&len, k->rx, 4);
            len = ntohl(len);
            if (len < 1 || len > (16u << 20)) {
                u32 p = k->peer;
                conn_close(k);
                tell_state(s, p, GKS_DISCONNECTED);
                return 0;
            }
            if (k->rxn < 4 + len)
                break;
            int type = k->rx[4];
            u8 *body = malloc(len - 1 + 1);
            memcpy(body, k->rx + 5, len - 1);
            memmove(k->rx, k->rx + 4 + len, k->rxn - (4 + len));
            k->rxn -= 4 + len;
            u32 p = k->peer;
            if (type == F_ACCEPT && k->state == C_WAIT_ACCEPT) {
                k->state = C_CONNECTED;
                free(body);
                tell_state(s, p, GKS_CONNECTED);
                return 0;
            }
            if (type == F_DENY) {
                free(body);
                conn_close(k);
                tell_failed(s, p);
                return 0;
            }
            if (type == F_BYE) {
                free(body);
                int was = k->state;
                conn_close(k);
                if (was == C_CONNECTED)
                    tell_state(s, p, GKS_DISCONNECTED);
                return 0;
            }
            if (type == F_DATA && k->state == C_CONNECTED) {
                tell_data(s, p, body, len - 1);
                free(body);
                if (!sess_alive(s) || k->state != C_CONNECTED || k->peer != p)
                    return 0;
                continue;
            }
            free(body);
        }
    }
    return 1;
}

static void pump(CPU *c)
{
    (void)c;
    int any = 0;
    for (int i = 0; i < MAX_SESS; i++)
        if (sessions[i])
            any = 1;
    if (!any) {
        pump_on = 0;
        return;
    }
    if (transport_up()) {
        accept_new();
        pending_read();
        beacons_send();
        beacons_read();
        for (int i = 0; i < MAX_SESS; i++) {
            Sess *s = sessions[i];
            if (!s)
                continue;
            objc_retain(s->obj);
            seen_expire(s);
            if (sess_alive(s))
                for (int n = 0; n < 64 && sess_alive(s) && !conns_step(s); n++)
                    ;
            /* RED_BULL_BOARD (board.c): each connected player's address gets our leaderboard */
            for (int k = 0; sess_alive(s) && k < MAX_CONN; k++) {
                if (s->conns[k].state != C_CONNECTED)
                    continue;
                struct sockaddr_in w;
                int wl = sizeof w;
                if (!getpeername(s->conns[k].s, (struct sockaddr *)&w, &wl) && !net_own_addr(w.sin_addr.s_addr))
                    board_peer_lan(w.sin_addr.s_addr);
            }
            objc_release(s->obj);
        }
        board_poll();
    } else {
        /* no network: the session fails, like GameKit with Bluetooth off */
        for (int i = 0; i < MAX_SESS; i++) {
            Sess *s = sessions[i];
            if (!s || !s->available || s->failed_told || !s->delegate)
                continue;
            s->failed_told = 1;
            if (objc_responds(s->delegate, "session:didFailWithError:")) {
                objc_retain(s->obj);
                msg2(s->delegate, "session:didFailWithError:", s->obj,
                     ns_error("com.apple.gamekit.GKSessionErrorDomain", 30509));
                objc_release(s->obj);
            }
            break;
        }
    }
    ns_perform_here(pump_obj, "gtPump", 0, 0.015);
}

static void pump_start(void)
{
    if (pump_on)
        return;
    if (!pump_obj)
        pump_obj = objc_retain(ns_alloc(ns_class("GTGameKitPump"), 8));
    pump_on = 1;
    ns_perform_here(pump_obj, "gtPump", 0, 0.015);
}

/* GKSession */

static void s_alloc(CPU *c) { ret_i(c, ns_alloc(SELF, sizeof(GkObj))); }

static void s_init(CPU *c)
{
    /* initWithSessionID:displayName:sessionMode: */
    id self = SELF;
    Sess *s = calloc(1, sizeof *s);
    s->obj = self;
    s->me = new_peer_id();
    snprintf(s->sid, sizeof s->sid, "%s", ARG(0) ? ns_utf8(ARG(0)) : "com.gameloft.GTRacing");
    if (ARG(1) && *ns_utf8(ARG(1))) {
        snprintf(s->name, sizeof s->name, "%s", ns_utf8(ARG(1)));
    } else {
        /* GameKit would use the device's name; the PC's name is not sent to other players */
        snprintf(s->name, sizeof s->name, "GT Racing PC");
    }
    s->mode = (int)ARG(2);
    for (int i = 0; i < MAX_CONN; i++)
        s->conns[i].s = INVALID_SOCKET;
    int i;
    for (i = 0; i < MAX_SESS && sessions[i]; i++)
        ;
    if (i == MAX_SESS) {
        free(s);
        ret_i(c, 0);
        return;
    }
    sessions[i] = s;
    GS(self) = s;
    log_gk("session \"%s\", mode %d, peer %u", s->sid, s->mode, s->me);
    pump_start();
    ret_i(c, self);
}

static void bye_all(Sess *s)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (s->conns[i].state != C_FREE) {
            if (s->conns[i].state != C_DIALING)
                conn_put(&s->conns[i], F_BYE, NULL, 0);
            conn_close(&s->conns[i]);
        }
}

static void s_dealloc(CPU *c)
{
    Sess *s = GS(SELF);
    if (s) {
        bye_all(s);
        for (int i = 0; i < MAX_SESS; i++)
            if (sessions[i] == s)
                sessions[i] = NULL;
        log_gk("session %u released", s->me);
        free(s);
    }
    objc_free_instance(SELF);
}

static void s_setDelegate(CPU *c) { if (GS(SELF)) GS(SELF)->delegate = ARG(0); }
static void s_delegate(CPU *c) { ret_i(c, GS(SELF) ? GS(SELF)->delegate : 0); }
static void s_setHandler(CPU *c)
{
    if (GS(SELF)) {
        GS(SELF)->handler = ARG(0);
        GS(SELF)->ctx = ARG(1);
    }
}
static void s_setAvailable(CPU *c)
{
    Sess *s = GS(SELF);
    if (!s)
        return;
    s->available = (ARG(0) & 0xff) != 0;
    s->beacon_at = GetTickCount() - BEACON_MS;      /* the first beacon at once */
    if (!s->available)
        s->n_seen = 0;
    pump_start();
}
static void s_isAvailable(CPU *c) { ret_i(c, GS(SELF) ? GS(SELF)->available : 0); }
static void s_peerID(CPU *c) { ret_i(c, GS(SELF) ? objc_autorelease(peer_new(GS(SELF)->me)) : 0); }
static void s_displayName(CPU *c) { ret_i(c, GS(SELF) ? ns_string(GS(SELF)->name) : 0); }
static void s_sessionID(CPU *c) { ret_i(c, GS(SELF) ? ns_string(GS(SELF)->sid) : 0); }
static void s_sessionMode(CPU *c) { ret_i(c, GS(SELF) ? (u32)GS(SELF)->mode : 0); }

static void s_displayNameForPeer(CPU *c)
{
    Sess *s = GS(SELF);
    u32 p = peer_of(ARG(0));
    const char *n = NULL;
    if (s) {
        if (p == s->me)
            n = s->name;
        Conn *k = conn_find(s, p);
        Seen *x = seen_find(s, p);
        if (!n && k && k->name[0])
            n = k->name;
        if (!n && x)
            n = x->name;
    }
    ret_i(c, n ? ns_string(n) : 0);
}

static void s_connect(CPU *c)
{
    /* connectToPeer:withTimeout: */
    Sess *s = GS(SELF);
    u32 p = peer_of(ARG(0));
    double t = arg_d(c, 3);
    Seen *x = s ? seen_find(s, p) : NULL;
    if (!s || !x || conn_find(s, p) || !transport_up()) {
        if (s)
            log_gk("connect to %u: not available", p);
        return;
    }
    Conn *k = conn_new(s);
    if (!k)
        return;
    k->peer = p;
    memcpy(k->name, x->name, sizeof k->name);       /* both 64 bytes, NUL-ended */
    k->name[sizeof k->name - 1] = 0;
    k->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    nonblock(k->s);
    BOOL yes = TRUE;
    setsockopt(k->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);
    if (net_test_ip()) {
        struct sockaddr_in me = {0};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = net_test_ip();
        bind(k->s, (struct sockaddr *)&me, sizeof me);
    }
    struct sockaddr_in w = {0};
    w.sin_family = AF_INET;
    w.sin_addr.s_addr = x->ip;
    w.sin_port = x->port_be;
    connect(k->s, (struct sockaddr *)&w, sizeof w);
    k->state = C_DIALING;
    k->deadline = GetTickCount() + (DWORD)((t > 0 && t < 120 ? t : 20) * 1000);
    log_gk("connecting to player %u", p);
}

static void s_cancelConnect(CPU *c)
{
    Sess *s = GS(SELF);
    Conn *k = s ? conn_find(s, peer_of(ARG(0))) : NULL;
    if (k && k->state != C_CONNECTED)
        conn_close(k);
}

static void s_accept(CPU *c)
{
    /* acceptConnectionFromPeer:error: */
    Sess *s = GS(SELF);
    Conn *k = s ? conn_find(s, peer_of(ARG(0))) : NULL;
    if (!k || k->state != C_INCOMING) {
        if (ARG(1))
            wr32(ARG(1), ns_error("com.apple.gamekit.GKSessionErrorDomain", 30500));
        ret_i(c, 0);
        return;
    }
    conn_put(k, F_ACCEPT, NULL, 0);
    k->state = C_CONNECTED;
    k->tell_connected = 1;                   /* reported from the pump, like GameKit */
    log_gk("accepted %u", k->peer);
    ret_i(c, 1);
}

static void s_deny(CPU *c)
{
    Sess *s = GS(SELF);
    Conn *k = s ? conn_find(s, peer_of(ARG(0))) : NULL;
    if (k && k->state == C_INCOMING) {
        conn_put(k, F_DENY, NULL, 0);
        conn_close(k);
    }
}

static int send_to(Sess *s, u32 p, id data)
{
    Conn *k = conn_find(s, p);
    if (!k || k->state != C_CONNECTED)
        return 0;
    u32 n = 0;
    const u8 *b = ns_data_bytes(data, &n);
    conn_put(k, F_DATA, b, n);
    return 1;
}

static void s_sendData(CPU *c)
{
    /* sendData:toPeers:withDataMode:error: */
    Sess *s = GS(SELF);
    id data = ARG(0), list = ARG(1);
    int ok = s && data && list;
    if (ok)
        for (u32 i = 0, n = ns_array_count(list); i < n; i++)
            ok &= send_to(s, peer_of(ns_array_at(list, i)), data);
    if (!ok && ARG(3))
        wr32(ARG(3), ns_error("com.apple.gamekit.GKSessionErrorDomain", 30501));
    ret_i(c, ok ? 1 : 0);
}

static void s_sendAll(CPU *c)
{
    /* sendDataToAllPeers:withDataMode:error: */
    Sess *s = GS(SELF);
    int ok = s && ARG(0);
    if (ok)
        for (int i = 0; i < MAX_CONN; i++)
            if (s->conns[i].state == C_CONNECTED)
                send_to(s, s->conns[i].peer, ARG(0));
    ret_i(c, ok ? 1 : 0);
}

static void s_disconnectAll(CPU *c)
{
    Sess *s = GS(SELF);
    if (s) {
        log_gk("session %u disconnects from all", s->me);
        bye_all(s);
    }
}

static void s_disconnectPeer(CPU *c)
{
    Sess *s = GS(SELF);
    Conn *k = s ? conn_find(s, peer_of(ARG(0))) : NULL;
    if (k) {
        conn_put(k, F_BYE, NULL, 0);
        conn_close(k);
    }
}

static void s_peersWithState(CPU *c)
{
    Sess *s = GS(SELF);
    int st = (int)ARG(0);
    id out = ns_array_new(NULL, 0);
    if (s) {
        if (st == GKS_CONNECTED) {
            for (int i = 0; i < MAX_CONN; i++)
                if (s->conns[i].state == C_CONNECTED) {
                    id p = peer_new(s->conns[i].peer);
                    ns_array_add(out, p);
                    objc_release(p);
                }
        } else if (st == GKS_AVAILABLE) {
            for (int i = 0; i < s->n_seen; i++)
                if (!conn_find(s, s->seen[i].id)) {
                    id p = peer_new(s->seen[i].id);
                    ns_array_add(out, p);
                    objc_release(p);
                }
        }
    }
    ret_i(c, objc_autorelease(out));
}

static void s_nop(CPU *c) { (void)c; }
static void s_timeout(CPU *c) { ret_d(c, 20.0); }

void gt_gk_init(void)
{
    static const NsMethod m[] = {
        {"alloc", s_alloc, 1},
        {"initWithSessionID:displayName:sessionMode:", s_init, 0},
        {"dealloc", s_dealloc, 0},
        {"setDelegate:", s_setDelegate, 0}, {"delegate", s_delegate, 0},
        {"setDataReceiveHandler:withContext:", s_setHandler, 0},
        {"setAvailable:", s_setAvailable, 0}, {"isAvailable", s_isAvailable, 0}, {"available", s_isAvailable, 0},
        {"peerID", s_peerID, 0}, {"displayName", s_displayName, 0}, {"sessionID", s_sessionID, 0},
        {"sessionMode", s_sessionMode, 0},
        {"displayNameForPeer:", s_displayNameForPeer, 0},
        {"connectToPeer:withTimeout:", s_connect, 0}, {"cancelConnectToPeer:", s_cancelConnect, 0},
        {"acceptConnectionFromPeer:error:", s_accept, 0}, {"denyConnectionFromPeer:", s_deny, 0},
        {"sendData:toPeers:withDataMode:error:", s_sendData, 0},
        {"sendDataToAllPeers:withDataMode:error:", s_sendAll, 0},
        {"disconnectFromAllPeers", s_disconnectAll, 0}, {"disconnectPeerFromAllPeers:", s_disconnectPeer, 0},
        {"peersWithConnectionState:", s_peersWithState, 0},
        {"setDisconnectTimeout:", s_nop, 0}, {"disconnectTimeout", s_timeout, 0},
        {NULL, NULL, 0}};
    ns_methods("GKSession", m);
    static const NsMethod p[] = {{"gtPump", pump, 0}, {NULL, NULL, 0}};
    ns_methods("GTGameKitPump", p);
}
