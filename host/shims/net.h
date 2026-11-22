/* net.h -- LAN_MULTIPLAYER's sockets (net.c), shared with gk.c (GameKit's GKSession).
   Include after <winsock2.h> for the SOCKET helpers. */
#ifndef GT_NET_H
#define GT_NET_H

#include "rt.h"

int net_lan_on(void);                     /* GT_LAN_MULTIPLAYER (default on), Winsock started */
u32 net_test_ip(void);                    /* GT_LAN_IP in network order, 0 = this PC's adapters */
int net_addr_allowed(u32 addr_be);        /* a local address (see net.c) */
int net_own_addr(u32 addr_be);            /* this PC's own (the test address, an adapter) */
void net_p2p_board(int i, const void *p, u32 n);   /* RED_BULL_BOARD: to tunnel player i (board.c) */
int net_is_socket(u32 fd);                /* one of the game's socket fds (900..1019) */
u32 net_close(u32 fd);
u32 net_fcntl(u32 fd, u32 cmd, u32 arg);
u32 net_ioctl(u32 fd, u32 req, u32 argp);
u32 net_iflist(u32 index, u32 oldp, u32 oldlenp);   /* sysctl NET_RT_IFLIST (uikit.c) */
int net_sockets_open(void);               /* the game has sockets open (BACKGROUND_THROTTLE keeps running) */
int gk_sessions_open(void);               /* gk.c: a GKSession is up */
void gt_net_init(void);
void gt_gk_init(void);
#if defined(_WINSOCKAPI_) || defined(GT_PLAT_NET_H)
int net_broadcast(SOCKET h, const void *p, int n, u16 port_be);   /* every subnet + GT_LAN_PEERS */
#endif

#endif
