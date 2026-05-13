#pragma once

#include <string>

namespace dinero {
namespace cli {

// Doctor command for comprehensive CLI diagnostics
// Returns: 0=success, 1=issues detected, 2=invalid config, 3=auth failure, 4=connection failure
int cmd_doctor(const std::string& rpc_url, const std::string& cookie_path, 
               const std::string& nodeinfo_path);

} // namespace cli
} // namespace dinero
