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
  handlers_.emplace(m, std::move(h)); owners_.emplace(m, owner);

  // AUTO-ALIAS: Register flat version (everything after last '.')
  auto dot = m.find('.');
  if (dot != std::string::npos) {
    std::string flat = m.substr(dot + 1);
    if (!handlers_.count(flat)) {
      handlers_[flat] = handlers_[m];
      owners_[flat] = owner + " [alias]";
    }
  }

  return true;
}

bool RpcRegistry::registerHandler(const std::string& m, RpcHandler h,
                                  const RpcMethodMeta& meta, const std::string& owner) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (handlers_.count(m)) { last_error_ = "duplicate method " + m; return false; }
  handlers_.emplace(m, std::move(h)); meta_.emplace(m, meta); owners_.emplace(m, owner);

  // AUTO-ALIAS: Register flat version (everything after last '.')
  auto dot = m.find('.');
  if (dot != std::string::npos) {
    std::string flat = m.substr(dot + 1);
    if (!handlers_.count(flat)) {
      handlers_[flat] = handlers_[m];
      meta_[flat] = meta;
      owners_[flat] = owner + " [alias]";
    }
  }

  return true;
}

bool RpcRegistry::registerHandler(const std::string& m, RpcHandler h,
                                  RegisterMode mode, const std::string& owner) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (mode == RegisterMode::IfAbsent && handlers_.count(m)) {
    return true; // Skip registration if method already exists
  }
  // FIX: Use assignment for Overwrite mode (emplace doesn't replace existing keys!)
  handlers_[m] = std::move(h); owners_[m] = owner;

  // 🔧 AUTO-ALIAS: Register flat version (everything after last '.')
  // Example: "wallet.getbalance" → also register "getbalance"
  auto dot = m.find('.');
  if (dot != std::string::npos) {
    std::string flat = m.substr(dot + 1);
    // Only create alias if flat name doesn't exist (avoid conflicts)
    if (!handlers_.count(flat)) {
      // FIX: Use handlers_[m] (not h, which was moved from!)
      handlers_[flat] = handlers_[m];
      owners_[flat] = owner + " [alias]";
    }
  }

  return true;
}

bool RpcRegistry::registerHandler(const std::string& m, RpcHandler h,
                                  const RpcMethodMeta& meta, RegisterMode mode, const std::string& owner) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (mode == RegisterMode::IfAbsent && handlers_.count(m)) {
    return true; // Skip registration if method already exists
  }
  // FIX: Use assignment for Overwrite mode (emplace doesn't replace existing keys!)
  handlers_[m] = std::move(h); meta_[m] = meta; owners_[m] = owner;

  // 🔧 AUTO-ALIAS: Register flat version (everything after last '.')
  auto dot = m.find('.');
  if (dot != std::string::npos) {
    std::string flat = m.substr(dot + 1);
    if (!handlers_.count(flat)) {
      // FIX: Use handlers_[m] (not h, which was moved from!)
      handlers_[flat] = handlers_[m];
      meta_[flat] = meta;
      owners_[flat] = owner + " [alias]";
    }
  }

  return true;
}

std::string RpcRegistry::getMethodOwner(const std::string& m) const {
  std::lock_guard<std::mutex> lk(mtx_); auto it=owners_.find(m); return it==owners_.end()?std::string():it->second;
}

void RpcRegistry::registerAlias(const std::string& alias, const std::string& target) {
  std::lock_guard<std::mutex> lk(mtx_);
  aliases_[alias] = target;

  // Store deprecation metadata
  RpcAliasInfo info;
  info.canonical_name = target;
  info.message = "Use " + target;
  alias_info_[alias] = info;

  dinero::g_logger.info("[RpcRegistry] ✅ Registered alias: " + alias + " → " + target);
}

RpcHandler* RpcRegistry::lookup(const std::string& m) {
  std::lock_guard<std::mutex> lk(mtx_);

  // Direct method lookup
  auto it = handlers_.find(m);
  if (it != handlers_.end()) {
    dinero::g_logger.debug("[RpcRegistry] Direct hit for method: " + m);
    return &it->second;
  }

  // Alias resolution: if m is an alias, look up the target
  auto alias_it = aliases_.find(m);
  if (alias_it != aliases_.end()) {
    dinero::g_logger.info("[RpcRegistry] Alias found: " + m + " → " + alias_it->second);
    auto target_it = handlers_.find(alias_it->second);
    if (target_it != handlers_.end()) {
      dinero::g_logger.info("[RpcRegistry] Alias resolved successfully");
      return &target_it->second;
    }
    dinero::g_logger.error("[RpcRegistry] Alias target not found: " + alias_it->second);
  } else {
    dinero::g_logger.error("[RpcRegistry] Method not found (no direct or alias match): " + m);
  }

  return nullptr;
}

std::vector<std::string> RpcRegistry::methodNames() const {
  std::lock_guard<std::mutex> lk(mtx_); std::vector<std::string> names;
  for(const auto& p : handlers_) names.push_back(p.first); return names;
}

bool RpcRegistry::has(const std::string& method) const {
  std::lock_guard<std::mutex> lk(mtx_);

  // Check direct method
  if (handlers_.count(method) > 0) {
    return true;
  }

  // Check if it's an alias
  auto alias_it = aliases_.find(method);
  if (alias_it != aliases_.end()) {
    return handlers_.count(alias_it->second) > 0;
  }

  return false;
}

const RpcMethodMeta* RpcRegistry::getMethodMeta(const std::string& method) const {
  std::lock_guard<std::mutex> lk(mtx_);
  auto it = meta_.find(method);
  return it == meta_.end() ? nullptr : &it->second;
}

// Alias introspection methods
bool RpcRegistry::isAlias(const std::string& method) const {
  std::lock_guard<std::mutex> lk(mtx_);
  return aliases_.count(method) > 0;
}

const RpcAliasInfo* RpcRegistry::getAliasInfo(const std::string& alias) const {
  std::lock_guard<std::mutex> lk(mtx_);
  auto it = alias_info_.find(alias);
  return it == alias_info_.end() ? nullptr : &it->second;
}

std::vector<std::string> RpcRegistry::getAliases(const std::string& canonical_method) const {
  std::lock_guard<std::mutex> lk(mtx_);
  std::vector<std::string> result;
  for (const auto& pair : aliases_) {
    if (pair.second == canonical_method) {
      result.push_back(pair.first);
    }
  }
  return result;
}
