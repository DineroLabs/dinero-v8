#pragma once

#include <cstdint>
#include <string>

namespace dinero::network {

struct EmbeddedTorStatus {
  bool available{false};
  bool running{false};
  uint16_t socks_port{0};
  uint16_t control_port{0};
  std::string message;
};

// Owns a private Tor child for this dinerod only.  It never installs a
// service, changes system Tor configuration, or exposes listeners off-host.
class EmbeddedTorProcess {
public:
  EmbeddedTorProcess(std::string executable, std::string data_directory);
  ~EmbeddedTorProcess();
  EmbeddedTorProcess(const EmbeddedTorProcess &) = delete;
  EmbeddedTorProcess &operator=(const EmbeddedTorProcess &) = delete;

  EmbeddedTorStatus Start();
  void Stop();
  EmbeddedTorStatus status() const { return status_; }

private:
  std::string executable_;
  std::string data_directory_;
  EmbeddedTorStatus status_;
#ifdef _WIN32
  void *process_handle_{nullptr};
#else
  int process_id_{-1};
#endif
};

} // namespace dinero::network
