#pragma once
#include <string>
#include <fstream>
#include <stdexcept>
#include <cstdlib>
#include <iostream>

inline std::string load_cookie_creds(const std::string& hinted_path = "") {
  // Priority: DINERO_COOKIE ("user:pass"), then DINERO_COOKIE_FILE, then hinted_path, then defaults
  if (const char* env = std::getenv("DINERO_COOKIE")) {
    std::cout << "Using DINERO_COOKIE env var" << std::endl;
    return std::string(env);   // must be "user:pass"
  }
  std::string path;
  if (const char* envf = std::getenv("DINERO_COOKIE_FILE")) {
    path = envf;
    std::cout << "Using DINERO_COOKIE_FILE env var: " << path << std::endl;
  } else if (!hinted_path.empty()) {
    path = hinted_path;
    std::cout << "Using hinted path: " << path << std::endl;
  } else {
#ifdef _WIN32
    path = R"(C:\Users\Default\AppData\Roaming\Dinero\.cookie)";
#else
    // Development fallback: $HOME/.dinero/.cookie
    if (const char* home = std::getenv("HOME")) {
      path = std::string(home) + "/.dinero/.cookie";
    } else {
      path = "/var/lib/dinero/.cookie"; // container/seed
    }
#endif
    std::cout << "Using default path: " << path << std::endl;
  }
  
  std::ifstream f(path);
  if (!f.is_open()) throw std::runtime_error("cannot open cookie file: " + path);
  
  // Read entire file content (not just one line)
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  
  // Strip trailing whitespace, newlines, and CR
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }
  
  if (s.find(':') == std::string::npos) throw std::runtime_error("bad cookie format in " + path);
  
  // Debug: show exact length and hex
  std::cout << "Cookie loaded - length: " << s.length() << ", value: '" << s << "'" << std::endl;
  std::cout << "Cookie hex: ";
  for (char c : s) {
    printf("%02x ", (unsigned char)c);
  }
  std::cout << std::endl;
  
  return s; // "__cookie__:abcdef..."
}
