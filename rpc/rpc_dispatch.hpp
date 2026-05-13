#pragma once
#include <string>
#include "json.hpp"
using json = json::json;

// Implement this in each binary:
// - rpcd: returns mock values
// - daemon: calls real handlers
json rpc_dispatch(const std::string& path,
                  const std::string& method,
                  const json& params,
                  const json& id);
