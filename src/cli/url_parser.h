#pragma once
#include <string>

struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port;
    std::string path;
    bool valid = false;
};

ParsedUrl parseUrl(const std::string& url);
