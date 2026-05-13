#include "rpc/rpc_registry.h"
#include "common/logger.h"

// Global RPC registry instance (in global namespace to match class declaration)
RpcRegistry g_rpcRegistry;

bool RpcRegistry::registerHandler(const std::string& m, RpcHandler h, const std::string& owner) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (handlers_.count(m)) { 
    dinero::g_logger.error("Duplicate RPC method registration: " + m + " (owner: " + owner + ")");
    last_error_ = "duplicate method " + m; 
#ifdef DIN_STRICT_RPC
    std::terminate(); // fail fast in dev/CI
#endif
    return false; 
  }
  handlers_.emplace(m, std::move(h)); owners_.emplace(m, owner); return true;
}

bool RpcRegistry::registerHandler(const std::string& m, RpcHandler h,
                                  const RpcMethodMeta& meta, const std::string& owner) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (handlers_.count(m)) { last_error_ = "duplicate method " + m; return false; }
  handlers_.emplace(m, std::move(h)); meta_.emplace(m, meta); owners_.emplace(m, owner); return true;
}

bool RpcRegistry::registerHandler(const std::string& m, RpcHandler h,
                                  RegisterMode mode, const std::string& owner) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (mode == RegisterMode::IfAbsent && handlers_.count(m)) {
    return true; // Skip registration if method already exists
  }
  handlers_.emplace(m, std::move(h)); owners_.emplace(m, owner); return true;
}

bool RpcRegistry::registerHandler(const std::string& m, RpcHandler h,
                                  const RpcMethodMeta& meta, RegisterMode mode, const std::string& owner) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (mode == RegisterMode::IfAbsent && handlers_.count(m)) {
    return true; // Skip registration if method already exists
  }
  handlers_.emplace(m, std::move(h)); meta_.emplace(m, meta); owners_.emplace(m, owner); return true;
}

std::string RpcRegistry::getMethodOwner(const std::string& m) const {
  std::lock_guard<std::mutex> lk(mtx_); auto it=owners_.find(m); return it==owners_.end()?std::string():it->second;
}

RpcHandler* RpcRegistry::lookup(const std::string& m) {
  std::lock_guard<std::mutex> lk(mtx_); auto it=handlers_.find(m); return it==handlers_.end()?nullptr:&it->second;
}

std::vector<std::string> RpcRegistry::methodNames() const {
  std::lock_guard<std::mutex> lk(mtx_); std::vector<std::string> names;
  for(const auto& p : handlers_) names.push_back(p.first); return names;
}

bool RpcRegistry::has(const std::string& method) const {
  std::lock_guard<std::mutex> lk(mtx_);
  return handlers_.count(method) > 0;
}
