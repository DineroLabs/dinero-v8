#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include "din_json.h"

struct ExecutionContext {
    std::string walletName;
    std::string user;
    std::string cookie;
    std::unordered_map<std::string, std::string> metadata;
};

using RpcHandler = std::function<din::Json(const ExecutionContext&, const din::Json&)>;

struct RpcParamMeta {
    std::string name;
    std::string type;
    std::string desc;
    bool required = false;
};

struct RpcResultMeta {
    std::string type;
    std::string desc;
};

struct RpcMethodMeta { 
    std::string name;
    std::string ns;
    std::string description;
    std::vector<RpcParamMeta> params;
    RpcResultMeta result;
    std::string help; 
};

enum class RegisterMode { IfAbsent, Overwrite };

class RpcRegistry {
public:
  bool registerHandler(const std::string& method, RpcHandler fn,
                       const std::string& owner="");
  bool registerHandler(const std::string& method, RpcHandler fn,
                       const RpcMethodMeta& meta, const std::string& owner="");
  bool registerHandler(const std::string& method, RpcHandler fn,
                       RegisterMode mode, const std::string& owner="");
  bool registerHandler(const std::string& method, RpcHandler fn,
                       const RpcMethodMeta& meta, RegisterMode mode, const std::string& owner="");

  std::string getMethodOwner(const std::string& method) const;
  RpcHandler* lookup(const std::string& method);
  std::vector<std::string> methodNames() const;
  bool has(const std::string& method) const;

private:
  mutable std::mutex mtx_;
  std::unordered_map<std::string, RpcHandler>    handlers_;
  std::unordered_map<std::string, RpcMethodMeta> meta_;
  std::unordered_map<std::string, std::string>   owners_;
  std::string last_error_;
};
