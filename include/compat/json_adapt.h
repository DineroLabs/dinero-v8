#pragma once
#ifdef __APPLE__
#include <json/json.h>
#else
#include <jsoncpp/json/json.h>
#endif
namespace dinjson {
  using value = Json::Value;
  inline value null()  { return Json::Value(); }
  inline value array() { return value(Json::arrayValue); }
  inline std::string as_string(const value& v){ return v.asString(); }
  inline uint64_t    as_u64(const value& v){ return v.isUInt64()? v.asUInt64() : (uint64_t)v.asLargestUInt(); }
  inline bool        as_bool(const value& v){ return v.asBool(); }
}

