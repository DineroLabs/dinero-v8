#pragma once
#include <string>
#include <random>
#include <thread>
#include <sstream>

inline std::string din_generate_trace_id(){
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::ostringstream o; o << std::hex << rng() << rng(); return o.str();
}
inline std::string& din_trace_tls(){ static thread_local std::string id; return id; }
inline void din_set_current_trace_id(std::string v){ din_trace_tls() = std::move(v); }
inline const std::string& din_current_trace_id(){ return din_trace_tls(); }
