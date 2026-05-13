#pragma once
#include <mutex>
#include <string>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace din {

struct LogLine {
  std::string ts, level, comp, msg, trace_id;
  std::string extra; // already JSON: `"height":123,"peer":"1.2.3.4"`
};

class JsonLogger {
public:
  static JsonLogger& get(){ static JsonLogger L; return L; }
  void log(const LogLine& ln){
    std::lock_guard<std::mutex> g(mu_);
    std::ostringstream o;
    o << "{\"ts\":\"" << ln.ts << "\","
      << "\"level\":\"" << ln.level << "\","
      << "\"comp\":\"" << ln.comp << "\","
      << "\"msg\":\""  << escape(ln.msg) << "\","
      << "\"trace_id\":\"" << ln.trace_id << "\"";
    if(!ln.extra.empty()) o << "," << ln.extra;
    o << ",\"rpc_schema\":\"din.rpc.v1\"}\n";
    std::fwrite(o.str().data(), 1, o.str().size(), stdout);
    std::fflush(stdout);
  }
  static std::string nowIso(){
    using namespace std::chrono;
    auto t = system_clock::now();
    auto s = time_point_cast<milliseconds>(t).time_since_epoch().count();
    std::ostringstream o; o << s; return o.str(); // ms since epoch (compact, parseable)
  }
private:
  static std::string escape(const std::string& in){
    std::string out; out.reserve(in.size());
    for(char c: in){ if(c=='"'||c=='\\') out.push_back('\\'); out.push_back(c); }
    return out;
  }
  std::mutex mu_;
};

} // namespace din

// Convenience macros
#define DIN_LOG_INFO(COMP, MSG, EXTRA_JSON) din::JsonLogger::get().log({din::JsonLogger::nowIso(),"INFO",COMP,MSG,din_current_trace_id(),EXTRA_JSON})
#define DIN_LOG_WARN(COMP, MSG, EXTRA_JSON) din::JsonLogger::get().log({din::JsonLogger::nowIso(),"WARN",COMP,MSG,din_current_trace_id(),EXTRA_JSON})
#define DIN_LOG_ERR(COMP,  MSG, EXTRA_JSON) din::JsonLogger::get().log({din::JsonLogger::nowIso(),"ERROR",COMP,MSG,din_current_trace_id(),EXTRA_JSON})
