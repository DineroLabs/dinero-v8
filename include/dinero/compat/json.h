#pragma once

/**
 * @brief JSON adapter layer for cross-platform compatibility
 * 
 * This header provides a unified interface for JSON operations,
 * allowing the codebase to work with either JsonCpp or nlohmann/json
 * without changing the core logic.
 */

#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  #include <json/json.h>
  namespace djson {
    using value = Json::Value;
    using array = Json::Value;
    using object = Json::Value;
    
    inline value null() { return Json::Value(); }
    inline value parse(const std::string& str) { 
      Json::Value root;
      Json::CharReaderBuilder builder;
      std::string errors;
      std::istringstream stream(str);
      if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        throw std::runtime_error("JSON parse error: " + errors);
      }
      return root;
    }
    inline std::string stringify(const value& v) {
      Json::StreamWriterBuilder builder;
      return Json::writeString(builder, v);
    }
  }
#else
  #include <nlohmann/json.hpp>
  namespace djson {
    using value = nlohmann::json;
    using array = nlohmann::json;
    using object = nlohmann::json;
    
    inline value null() { return nlohmann::json(nullptr); }
    inline value parse(const std::string& str) { 
      return nlohmann::json::parse(str); 
    }
    inline std::string stringify(const value& v) {
      return v.dump();
    }
  }
#endif

// Common JSON operations that work with both libraries
namespace djson {

/**
 * @brief Check if a JSON value is null
 */
inline bool is_null(const value& v) {
#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  return v.isNull();
#else
  return v.is_null();
#endif
}

/**
 * @brief Check if a JSON value is a string
 */
inline bool is_string(const value& v) {
#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  return v.isString();
#else
  return v.is_string();
#endif
}

/**
 * @brief Check if a JSON value is a number
 */
inline bool is_number(const value& v) {
#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  return v.isNumeric();
#else
  return v.is_number();
#endif
}

/**
 * @brief Check if a JSON value is an object
 */
inline bool is_object(const value& v) {
#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  return v.isObject();
#else
  return v.is_object();
#endif
}

/**
 * @brief Check if a JSON value is an array
 */
inline bool is_array(const value& v) {
#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  return v.isArray();
#else
  return v.is_array();
#endif
}

/**
 * @brief Get string value
 */
inline std::string as_string(const value& v) {
#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  return v.asString();
#else
  return v.get<std::string>();
#endif
}

/**
 * @brief Get integer value
 */
inline int64_t as_int(const value& v) {
#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  return v.asInt64();
#else
  return v.get<int64_t>();
#endif
}

/**
 * @brief Get double value
 */
inline double as_double(const value& v) {
#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  return v.asDouble();
#else
  return v.get<double>();
#endif
}

/**
 * @brief Get boolean value
 */
inline bool as_bool(const value& v) {
#if defined(DIN_JSON_JSONCPP) && DIN_JSON_JSONCPP
  return v.asBool();
#else
  return v.get<bool>();
#endif
}

} // namespace djson
