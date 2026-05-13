#pragma once
#include <thread>
#include <atomic>
#include <vector>
#include "core/broadcast.hpp"
#include "daemon/ws_subscriptions.hpp"

namespace dinero {

class WsBridge {
public:
  explicit WsBridge(Subscriptions* subs);
  ~WsBridge();
  
  void start();
  void stop();

private:
  void run();
  
  std::atomic<bool> running_{false};
  std::thread th_;
  Subscriptions* subs_;
  
  // Metrics
  void increment_metric(const std::string& channel);
};

} // namespace dinero
