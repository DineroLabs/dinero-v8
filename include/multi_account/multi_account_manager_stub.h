#pragma once
#include <string>
namespace dinero {
  class MultiAccountManager {
  public:
    bool load(const std::string&) { return true; }
    // add only what daemon files actually use
  };
}

