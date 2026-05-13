#pragma once
#include "compat/jsoncpp_compat.h"
#include <memory>
#include <string>
#include <string_view>

struct JsonParseResult {
    bool ok = false;
    Json::Value root;
    std::string error;
};

inline JsonParseResult ParseJsonStrict(std::string_view body, size_t max_bytes = 1 << 20) { // 1 MiB cap
    JsonParseResult r;
    if (body.size() > max_bytes) { 
        r.error = "request too large"; 
        return r; 
    }

    Json::CharReaderBuilder b;
    b["collectComments"] = false;
    b["allowSingleQuotes"] = false;
    b["allowNumericKeys"] = false;
    b["strictRoot"] = true;
    b["failIfExtra"] = true;

    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    const char* begin = body.data();
    const char* end = begin + body.size();
    r.ok = reader->parse(begin, end, &r.root, &r.error);
    return r;
}
