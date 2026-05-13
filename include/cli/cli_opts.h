#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace cli {
struct Opts {
  // Store multiple values per key to support multiple --addnode flags
  std::unordered_map<std::string, std::vector<std::string>> kv;
  
  bool has(const std::string& k) const { 
    return kv.count(k) && !kv.at(k).empty(); 
  }
  
  // Get last value (for compatibility with single-value flags)
  std::string get(const std::string& k, const std::string& def="") const {
    auto it = kv.find(k);
    return (it == kv.end() || it->second.empty()) ? def : it->second.back();
  }
  
  // Get all values for a key (for multiple --addnode, etc.)
  std::vector<std::string> get_all(const std::string& k) const {
    auto it = kv.find(k);
    return (it == kv.end()) ? std::vector<std::string>{} : it->second;
  }
};

inline Opts parse(int argc, char** argv) {
  Opts o;
  for (int i=1; i<argc; ++i) {
    std::string a(argv[i]);
    if (a.rfind("-",0)==0) {
      // Strip leading dashes (support both - and --)
      size_t dash_count = (a.size() > 1 && a[1] == '-') ? 2 : 1;
      std::string arg = a.substr(dash_count);
      
      // -key=value or --key=value
      auto pos = arg.find('=');
      if (pos != std::string::npos) {
        std::string key = arg.substr(0, pos);
        std::string value = arg.substr(pos+1);
        o.kv[key].push_back(value);
      } else {
        // -key value or --key value or -flag
        if (i+1 < argc && std::string(argv[i+1]).rfind("-",0) != 0) {
          o.kv[arg].push_back(argv[++i]);
        } else {
          o.kv[arg].push_back("1");
        }
      }
    }
  }
  return o;
}
} // namespace cli
