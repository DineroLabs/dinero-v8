#pragma once
// Cross-platform networking compatibility header.
// Include this instead of POSIX socket headers directly.

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
#endif
