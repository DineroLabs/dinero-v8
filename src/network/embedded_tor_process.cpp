#include "network/embedded_tor_process.h"

#include <chrono>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace dinero::network {
namespace {

#ifndef _WIN32
uint16_t AvailableLoopbackPort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return 0;
  }
  socklen_t size = sizeof(address);
  const bool ok =
      ::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) == 0;
  ::close(fd);
  return ok ? ntohs(address.sin_port) : 0;
}

bool CanConnect(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  const bool ok = ::connect(fd, reinterpret_cast<sockaddr *>(&address),
                            sizeof(address)) == 0;
  ::close(fd);
  return ok;
}
#endif

} // namespace

EmbeddedTorProcess::EmbeddedTorProcess(std::string executable,
                                       std::string data_directory)
    : executable_(std::move(executable)),
      data_directory_(std::move(data_directory)) {}

EmbeddedTorProcess::~EmbeddedTorProcess() { Stop(); }

EmbeddedTorStatus EmbeddedTorProcess::Start() {
  if (status_.running)
    return status_;
#ifdef _WIN32
  status_.message =
      "embedded Tor process ownership is unavailable on this platform";
  return status_;
#else
  struct stat executable_status{};
  if (::lstat(executable_.c_str(), &executable_status) != 0 ||
      !S_ISREG(executable_status.st_mode) ||
      ::access(executable_.c_str(), X_OK) != 0) {
    status_.message = "verified embedded Tor executable is unavailable";
    return status_;
  }
  status_.available = true;
  std::error_code ec;
  std::filesystem::create_directories(data_directory_, ec);
  if (ec || ::chmod(data_directory_.c_str(), S_IRWXU) != 0) {
    status_.message = "could not protect embedded Tor data directory";
    return status_;
  }
  const uint16_t socks = AvailableLoopbackPort();
  const uint16_t control = AvailableLoopbackPort();
  if (socks == 0 || control == 0 || socks == control) {
    status_.message = "could not allocate localhost ports for embedded Tor";
    return status_;
  }
  const std::string socks_endpoint = "127.0.0.1:" + std::to_string(socks);
  const std::string control_endpoint = "127.0.0.1:" + std::to_string(control);
  const std::filesystem::path binary(executable_);
  const auto bundle_root = binary.parent_path().parent_path();
  const std::string geoip = (bundle_root / "data" / "geoip").string();
  const std::string geoip6 = (bundle_root / "data" / "geoip6").string();

  process_id_ = ::fork();
  if (process_id_ == 0) {
    const int null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      ::dup2(null_fd, STDIN_FILENO);
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO)
        ::close(null_fd);
    }
    ::execl(executable_.c_str(), executable_.c_str(), "--RunAsDaemon", "0",
            "--DataDirectory", data_directory_.c_str(), "--SocksPort",
            socks_endpoint.c_str(), "--ControlPort", control_endpoint.c_str(),
            "--CookieAuthentication", "1", "--GeoIPFile", geoip.c_str(),
            "--GeoIPv6File", geoip6.c_str(), "--ClientOnly", "1", "--ExitRelay",
            "0", "--ORPort", "0", static_cast<char *>(nullptr));
    _exit(127);
  }
  if (process_id_ < 0) {
    status_.message = "could not start embedded Tor";
    return status_;
  }
  for (int attempt = 0; attempt < 100; ++attempt) {
    int child_status = 0;
    if (::waitpid(process_id_, &child_status, WNOHANG) == process_id_) {
      process_id_ = -1;
      status_.message = "embedded Tor exited during startup";
      return status_;
    }
    if (CanConnect(control)) {
      status_.running = true;
      status_.socks_port = socks;
      status_.control_port = control;
      status_.message = "embedded Tor is running";
      return status_;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Stop();
  status_.available = true;
  status_.message = "embedded Tor startup timed out";
  return status_;
#endif
}

void EmbeddedTorProcess::Stop() {
#ifndef _WIN32
  if (process_id_ > 0) {
    ::kill(process_id_, SIGTERM);
    for (int attempt = 0; attempt < 50; ++attempt) {
      int child_status = 0;
      if (::waitpid(process_id_, &child_status, WNOHANG) == process_id_) {
        process_id_ = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (process_id_ > 0) {
      ::kill(process_id_, SIGKILL);
      (void)::waitpid(process_id_, nullptr, 0);
      process_id_ = -1;
    }
  }
#endif
  status_.running = false;
  status_.socks_port = 0;
  status_.control_port = 0;
}

} // namespace dinero::network
