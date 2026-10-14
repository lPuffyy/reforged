// Socket compatibility: Winsock on Windows, BSD sockets elsewhere. Only the handful of differences the network backend touches.
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
inline void NetSockInit() { static bool done = false; if (!done) { done = true; WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); } }
inline void NetSetNonBlocking(SOCKET s, bool on) { u_long v = on ? 1 : 0; ioctlsocket(s, FIONBIO, &v); }
inline void NetSetRecvTimeoutMs(SOCKET s, unsigned ms) { DWORD v = ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof v); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
inline int closesocket(SOCKET s) { return close(s); }
inline void NetSockInit() { static bool done = false; if (!done) { done = true; signal(SIGPIPE, SIG_IGN); } }
inline void NetSetNonBlocking(SOCKET s, bool on) { int f = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK)); }
inline void NetSetRecvTimeoutMs(SOCKET s, unsigned ms) { timeval tv{(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); }
#endif
