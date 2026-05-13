// include/din_json.h
#pragma once
#if defined(__APPLE__) || defined(_WIN32)
#include <json/json.h>
#else
#include <json/json.h>
#endif
#include <string>
#include <memory>
#include <sstream>

namespace din {
  using Json = ::Json::Value;
  
  inline bool parse(const std::string& s, Json& out) {
    ::Json::CharReaderBuilder builder;
    std::string err;
    std::unique_ptr<::Json::CharReader> reader(builder.newCharReader());
    return reader->parse(s.c_str(), s.c_str() + s.size(), &out, &err);
  }
  
  inline Json parse(const std::string& s) {
    Json out;
    ::Json::CharReaderBuilder builder;
    std::string err;
    std::unique_ptr<::Json::CharReader> reader(builder.newCharReader());
    if (reader->parse(s.c_str(), s.c_str() + s.size(), &out, &err)) {
      return out;
    }
    return Json();
  }
  
  inline std::string dump(const Json& j) { 
    ::Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return ::Json::writeString(builder, j);
  }
  
  inline Json null() {
    return Json(); 
  }

  inline Json obj() {
    return Json(::Json::objectValue); 
  }

  inline Json arr() {
    return Json(::Json::arrayValue); 
  }
}
