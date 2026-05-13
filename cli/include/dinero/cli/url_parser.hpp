#pragma once

#include <string>

namespace dinero {
namespace cli {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port;
    std::string path;
    bool valid = false;
    std::string error_message;
};

// Single source of truth - declaration only
ParsedUrl ParseUrl(const std::string& url);

} // namespace cli
} // namespace dinero
