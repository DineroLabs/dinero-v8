#include "core/broadcast.hpp"
using namespace dinero;

BroadcastBus& BroadcastBus::instance() {
  static BroadcastBus bus;
  return bus;
}

void BroadcastBus::post(std::string channel, std::string json) {
  std::unique_lock<std::mutex> lk(mu_);
  if (stopping_) return;
  if (q_.size() >= max_queue_) { 
    dropped.fetch_add(1, std::memory_order_relaxed); 
    q_.pop_front(); 
  }
  q_.append(BusEvent{std::move(channel), std::move(json)});
  lk.unlock();
  cv_.notify_one();
}

bool BroadcastBus::try_pop(BusEvent& out) {
  std::lock_guard<std::mutex> lk(mu_);
  if (q_.empty()) return false;
  out = std::move(q_.front());
  q_.pop_front();
  return true;
}

std::size_t BroadcastBus::pop_all(std::vector<BusEvent>& out, std::size_t max) {
  std::lock_guard<std::mutex> lk(mu_);
  std::size_t n = 0;
  while (!q_.empty() && n < max) {
    out.push_back(std::move(q_.front()));
    q_.pop_front();
    ++n;
  }
  return n;
}

void BroadcastBus::wait_for_event() {
  std::unique_lock<std::mutex> lk(mu_);
  if (!q_.empty() || stopping_) return;
  cv_.wait(lk, [&]{ return stopping_ || !q_.empty(); });
}

void BroadcastBus::stop() {
  std::lock_guard<std::mutex> lk(mu_);
  stopping_ = true;
  cv_.notify_all();
}
