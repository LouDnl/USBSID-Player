/*
 * USBSID-Player: a cycle exact C64 SID player for USBSID-Pico, for command
 * line playback and for embedding on RP2350 (Pico2).
 *
 * net_compat.h
 * Thin BSD sockets / Winsock2 shim for the Network SID Device client
 * (sid_netdevice.cpp) and its loopback test. Header only, inline.
 *
 * This file is part of USBSID-Pico (https://github.com/LouDnl/USBSID-Player)
 * File author: LouD
 *
 * Copyright (c) 2026 LouD
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef US_NET_COMPAT_H
#define US_NET_COMPAT_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace usbsid {

/* Socket handle held as a signed integer: int on POSIX, SOCKET (pointer
 * sized) on Windows. -1 is the invalid value everywhere. */
typedef std::intptr_t net_sock_t;
constexpr net_sock_t kNetInvalidSock = -1;
typedef std::ptrdiff_t net_ssize_t;

/* Winsock needs WSAStartup before any socket call; once per process is enough
 * (the OS refcounts it and reclaims at exit). No-op elsewhere. */
inline bool net_startup(void)
{
#ifdef _WIN32
  static const bool ok = [] {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  return ok;
#else
  return true;
#endif
}

inline net_sock_t net_socket(int family, int type, int protocol)
{
#ifdef _WIN32
  const SOCKET s = ::socket(family, type, protocol);
  return s == INVALID_SOCKET ? kNetInvalidSock : static_cast<net_sock_t>(s);
#else
  return ::socket(family, type, protocol);
#endif
}

/* Connect with an upper bound on the wait. A plain blocking connect to an
 * unreachable host takes ~21 s on Windows and minutes on macOS/Linux. The
 * socket goes non-blocking for the connect, select() waits for the result
 * (writable on success; on Windows failure arrives in exceptfds), SO_ERROR
 * gives the verdict, and blocking mode is restored before returning. */
inline bool net_connect(net_sock_t s, const struct sockaddr * addr, size_t len,
                        int timeout_ms = 3000)
{
#ifdef _WIN32
  const SOCKET fd = static_cast<SOCKET>(s);
  u_long nb = 1;
  if (::ioctlsocket(fd, FIONBIO, &nb) != 0) {
    /* Cannot bound the wait: fall back to a plain blocking connect. */
    return ::connect(fd, addr, static_cast<int>(len)) == 0;
  }
  bool ok = false;
  int rc = ::connect(fd, addr, static_cast<int>(len));
  if (rc == 0) {
    ok = true;
  } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
    fd_set wfds, efds;
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(fd, &wfds);
    FD_SET(fd, &efds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    /* First argument is ignored by Winsock. */
    if (::select(0, nullptr, &wfds, &efds, &tv) > 0 && FD_ISSET(fd, &wfds)) {
      int err = 0;
      int elen = sizeof(err);
      ok = ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char *>(&err), &elen) == 0 && err == 0;
    }
  }
  nb = 0;
  ::ioctlsocket(fd, FIONBIO, &nb);
  return ok;
#else
  const int fd = static_cast<int>(s);
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    /* Cannot bound the wait: fall back to a plain blocking connect. */
    return ::connect(fd, addr, static_cast<socklen_t>(len)) == 0;
  }
  bool ok = false;
  int rc = ::connect(fd, addr, static_cast<socklen_t>(len));
  if (rc == 0) {
    ok = true;
  } else if (errno == EINPROGRESS || errno == EINTR) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int sel;
    do {
      /* Linux select() updates tv with the time left; an EINTR retry
       * does not restart the full timeout there; other systems keep tv. */
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(fd, &wfds);
      sel = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
    } while (sel < 0 && errno == EINTR);
    if (sel > 0) {
      int err = 0;
      socklen_t elen = sizeof(err);
      ok = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0;
    }
  }
  ::fcntl(fd, F_SETFL, flags);
  return ok;
#endif
}

/* Bind to 127.0.0.1 on an OS chosen port and listen; the port comes back in
 * host order. For the loopback test server. */
inline bool net_listen_loopback(net_sock_t s, uint16_t * port)
{
  sockaddr_in a;
  std::memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
#ifdef _WIN32
  const SOCKET fd = static_cast<SOCKET>(s);
  int len = sizeof(a);
#else
  const int fd = static_cast<int>(s);
  socklen_t len = sizeof(a);
#endif
  if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0
      || ::listen(fd, 1) != 0
      || ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len) != 0) return false;
  *port = ntohs(a.sin_port);
  return true;
}

inline net_sock_t net_accept(net_sock_t s)
{
#ifdef _WIN32
  const SOCKET c = ::accept(static_cast<SOCKET>(s), nullptr, nullptr);
  return c == INVALID_SOCKET ? kNetInvalidSock : static_cast<net_sock_t>(c);
#else
  return ::accept(static_cast<int>(s), nullptr, nullptr);
#endif
}

inline void net_close(net_sock_t s)
{
  if (s == kNetInvalidSock) return;
#ifdef _WIN32
  ::closesocket(static_cast<SOCKET>(s));
#else
  ::close(static_cast<int>(s));
#endif
}

/* macOS has no MSG_NOSIGNAL: SO_NOSIGPIPE per socket (net_nosigpipe) instead.
 * Windows has no SIGPIPE. */
inline void net_nosigpipe(net_sock_t s)
{
#if defined(SO_NOSIGPIPE) && !defined(_WIN32)
  const int one = 1;
  ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
  (void)s;
#endif
}

inline net_ssize_t net_send(net_sock_t s, const uint8_t * buf, size_t len)
{
#ifdef _WIN32
  return ::send(static_cast<SOCKET>(s), reinterpret_cast<const char *>(buf),
                static_cast<int>(len), 0);
#elif defined(MSG_NOSIGNAL)
  return ::send(static_cast<int>(s), buf, len, MSG_NOSIGNAL);
#else
  return ::send(static_cast<int>(s), buf, len, 0);
#endif
}

inline net_ssize_t net_recv(net_sock_t s, uint8_t * buf, size_t len)
{
#ifdef _WIN32
  return ::recv(static_cast<SOCKET>(s), reinterpret_cast<char *>(buf),
                static_cast<int>(len), 0);
#else
  return ::recv(static_cast<int>(s), buf, len, 0);
#endif
}

/* Non-blocking read of whatever is already queued; <= 0 when nothing is. */
inline net_ssize_t net_recv_nowait(net_sock_t s, uint8_t * buf, size_t len)
{
#ifdef _WIN32
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(static_cast<SOCKET>(s), &rfds);
  timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  if (::select(0, &rfds, nullptr, nullptr, &tv) <= 0) return 0;
  return ::recv(static_cast<SOCKET>(s), reinterpret_cast<char *>(buf),
                static_cast<int>(len), 0);
#else
  return ::recv(static_cast<int>(s), buf, len, MSG_DONTWAIT);
#endif
}

inline void net_set_nodelay(net_sock_t s)
{
  const int one = 1;
#ifdef _WIN32
  ::setsockopt(static_cast<SOCKET>(s), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char *>(&one), sizeof(one));
#else
  ::setsockopt(static_cast<int>(s), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
}

/* SO_RCVTIMEO / SO_SNDTIMEO: DWORD milliseconds on Windows, timeval elsewhere. */
inline void net_set_timeouts(net_sock_t s, unsigned seconds)
{
#ifdef _WIN32
  const DWORD ms = seconds * 1000u;
  ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&ms), sizeof(ms));
  ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&ms), sizeof(ms));
#else
  struct timeval tv;
  tv.tv_sec = seconds;
  tv.tv_usec = 0;
  ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

} /* namespace usbsid */

#endif /* US_NET_COMPAT_H */
