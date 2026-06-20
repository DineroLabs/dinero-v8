#pragma once
// Cross-platform networking compatibility header.
// Include this instead of POSIX socket headers directly.

#include <cstring>
#include <string>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <io.h>

  // POSIX compatibility typedefs
  #ifndef _SSIZE_T_DEFINED
    #define _SSIZE_T_DEFINED
    typedef ptrdiff_t ssize_t;
  #endif

  // MSG_NOSIGNAL doesn't exist on Windows (Winsock doesn't generate SIGPIPE)
  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
  #endif

  // POSIX shutdown(2) constants -> Winsock equivalents.
  // POSIX uses SHUT_RD / SHUT_WR / SHUT_RDWR (in <sys/socket.h>); Winsock uses
  // SD_RECEIVE / SD_SEND / SD_BOTH. Map them here so daemon code that calls
  // shutdown(fd, SHUT_RDWR) builds on both platforms without per-callsite
  // ifdefs. The dead-orphan-sweep added shutdown() calls on the graceful-stop
  // path that need this shim on Windows.
  #ifndef SHUT_RD
    #define SHUT_RD   SD_RECEIVE
  #endif
  #ifndef SHUT_WR
    #define SHUT_WR   SD_SEND
  #endif
  #ifndef SHUT_RDWR
    #define SHUT_RDWR SD_BOTH
  #endif

  // Map POSIX close() to Winsock closesocket()
  #ifndef COMPAT_CLOSE_SOCKET
    #define COMPAT_CLOSE_SOCKET(s) closesocket(s)
  #endif

  // Windows doesn't have fcntl; socket non-blocking via ioctlsocket
  inline int compat_set_nonblocking(int fd) {
      u_long mode = 1;
      return ioctlsocket(fd, FIONBIO, &mode);
  }

  // Windows equivalent of FD_CLOEXEC: clear the socket handle's inherit flag so
  // spawned child processes don't inherit the daemon's listen sockets. See the
  // POSIX compat_set_cloexec above for the rationale (#295).
  inline int compat_set_cloexec(int fd) {
      return SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd)),
                                  HANDLE_FLAG_INHERIT, 0) ? 0 : -1;
  }

  // Socket-error helpers — winsock keeps its own error state separate from
  // errno, so callsites that compared errno after a socket call must route
  // through these instead.
  inline int compat_sock_errno() { return WSAGetLastError(); }
  inline bool compat_sock_wouldblock(int err) { return err == WSAEWOULDBLOCK; }
  // Winsock signals "connection in progress" via WSAEWOULDBLOCK on a
  // non-blocking connect(), not via a distinct WSAEINPROGRESS code
  // (WSAEINPROGRESS is the legacy "blocking call in progress" state).
  inline bool compat_sock_inprogress(int err) { return err == WSAEWOULDBLOCK; }
  // Winsock doesn't deliver EINTR on socket I/O — no signal-interrupted
  // restart loop is needed; treat as "no, never".
  inline bool compat_sock_eintr(int /*err*/) { return false; }
  inline std::string compat_sock_strerror(int err) {
      char* msg = nullptr;
      DWORD len = FormatMessageA(
          FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
              FORMAT_MESSAGE_IGNORE_INSERTS,
          nullptr, static_cast<DWORD>(err),
          MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
          reinterpret_cast<LPSTR>(&msg), 0, nullptr);
      std::string out;
      if (len > 0 && msg) {
          // FormatMessage tends to append CRLF; trim it.
          while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r' ||
                              msg[len - 1] == ' ')) {
              --len;
          }
          out.assign(msg, len);
      } else {
          out = "winsock error " + std::to_string(err);
      }
      if (msg) LocalFree(msg);
      return out;
  }

  // One-time WSAStartup. Idempotent across calls (winsock is refcounted
  // and the static guard ensures we only call it once per process).
  // Matches the pattern of multiple-callsite WSAStartup already in
  // src/daemon/p2p_manager.cpp, src/network/udp_socket.cpp, etc.
  inline void compat_net_init_once() {
      static bool initialized = []() {
          WSADATA wsa;
          return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
      }();
      (void)initialized;
  }

  #pragma comment(lib, "ws2_32.lib")

  // Windows headers may define ERROR, DELETE etc. as macros — conflicts with enums
  #ifdef ERROR
    #undef ERROR
  #endif
  #ifdef DELETE
    #undef DELETE
  #endif
#else
  // POSIX
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>

  #ifndef COMPAT_CLOSE_SOCKET
    #define COMPAT_CLOSE_SOCKET(s) ::close(s)
  #endif

  inline int compat_set_nonblocking(int fd) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0) return -1;
      return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  // Prevent child processes from inheriting this socket. Without FD_CLOEXEC, a
  // child the daemon spawns (e.g. dinero-seeder) inherits the daemon's listen
  // sockets; when the daemon exits, the orphaned child keeps those FDs open and
  // squats the RPC/P2P ports, so the next daemon can't bind ("daemon exited
  // before the wallet could connect"). #295.
  inline int compat_set_cloexec(int fd) {
      int flags = fcntl(fd, F_GETFD, 0);
      if (flags < 0) return -1;
      return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }

  inline int compat_sock_errno() { return errno; }
  inline bool compat_sock_wouldblock(int err) {
      return err == EAGAIN || err == EWOULDBLOCK;
  }
  inline bool compat_sock_inprogress(int err) { return err == EINPROGRESS; }
  inline bool compat_sock_eintr(int err) { return err == EINTR; }
  inline std::string compat_sock_strerror(int err) {
      return std::strerror(err);
  }

  // No-op on POSIX (kept so callsites stay portable).
  inline void compat_net_init_once() {}
#endif
