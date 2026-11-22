/* plat_net.h -- sockets: Winsock on Windows, BSD sockets elsewhere, under the Winsock names the host uses. */
#ifndef GT_PLAT_NET_H
#define GT_PLAT_NET_H

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "plat.h"

typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define ioctlsocket(s, cmd, argp) ioctl((s), (cmd), (argp))
#define WSAGetLastError() errno
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINPROGRESS EINPROGRESS
#define WSAEALREADY EALREADY
#define WSAENOTSOCK ENOTSOCK
#define WSAEDESTADDRREQ EDESTADDRREQ
#define WSAEMSGSIZE EMSGSIZE
#define WSAEPROTOTYPE EPROTOTYPE
#define WSAENOPROTOOPT ENOPROTOOPT
#define WSAEPROTONOSUPPORT EPROTONOSUPPORT
#define WSAEAFNOSUPPORT EAFNOSUPPORT
#define WSAEADDRINUSE EADDRINUSE
#define WSAEADDRNOTAVAIL EADDRNOTAVAIL
#define WSAENETDOWN ENETDOWN
#define WSAENETUNREACH ENETUNREACH
#define WSAENETRESET ENETRESET
#define WSAECONNABORTED ECONNABORTED
#define WSAECONNRESET ECONNRESET
#define WSAENOBUFS ENOBUFS
#define WSAEISCONN EISCONN
#define WSAENOTCONN ENOTCONN
#define WSAESHUTDOWN ESHUTDOWN
#define WSAETIMEDOUT ETIMEDOUT
#define WSAECONNREFUSED ECONNREFUSED
#define WSAEHOSTUNREACH EHOSTUNREACH
#define WSAEACCES EACCES
#define WSAEFAULT EFAULT
#define WSAEMFILE EMFILE
#define WSAEINVAL EINVAL
#define WSAEINTR EINTR
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#define SD_BOTH SHUT_RDWR
/* Winsock's ioctlsocket takes u_long */
typedef unsigned long u_long;
#endif

/* a receive / send timeout in milliseconds (Winsock takes a DWORD of them, POSIX a timeval) */
static inline int gt_sock_timeout(SOCKET s, int opt, unsigned long ms)
{
#ifdef _WIN32
    DWORD v = (DWORD)ms;
    return setsockopt(s, SOL_SOCKET, opt, (const char *)&v, sizeof v);
#else
    struct timeval tv = {(time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000};
    return setsockopt(s, SOL_SOCKET, opt, &tv, sizeof tv);
#endif
}

#endif
