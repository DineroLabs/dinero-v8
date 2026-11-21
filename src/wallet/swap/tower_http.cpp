#include "wallet/swap/tower_http.h"

#include <json/json.h>

#include <memory>
#include <stdexcept>

namespace dinero::swap {
namespace {

std::string Compact(const Json::Value& v) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    return Json::writeString(b, v);
}

TowerHttpReply Reply(int status, const Json::Value& body) { return TowerHttpReply{status, Compact(body)}; }

TowerHttpReply Error(int status, const std::string& why) {
    Json::Value v(Json::objectValue);
    v["error"] = why;
    return Reply(status, v);
}

std::optional<Json::Value> ParseObject(const std::string& text) {
    Json::CharReaderBuilder b;
    Json::Value v;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    if (!reader->parse(text.data(), text.data() + text.size(), &v, &errors) || !v.isObject()) return std::nullopt;
    return v;
}

std::string WithoutTrailingSlash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

}  // namespace

TowerHttpReply HandleTowerHttpRequest(const std::string& method, const std::string& target, const std::string& body,
                                      const std::string& inbox_dir, const TowerIntakeLimits& limits, uint32_t now) {
    try {
        if (target == "/v1/health") {
            if (method != "GET") return Error(405, "use GET");
            Json::Value v(Json::objectValue);
            v["ok"] = true;
            return Reply(200, v);
        }
        if (target == "/v1/packages") {
            if (method != "POST") return Error(405, "use POST");
            const TowerIntakeResult r = TowerIntakeStore(inbox_dir, body, limits);
            switch (r.status) {
            case TowerIntakeStatus::Stored:
            case TowerIntakeStatus::AlreadyHeld: {
                Json::Value v(Json::objectValue);
                v["hash"] = r.hash;
                return Reply(200, v);
            }
            case TowerIntakeStatus::TooLarge: return Error(413, "the package is too large");
            case TowerIntakeStatus::Invalid: return Error(400, r.reason);
            case TowerIntakeStatus::Full: return Error(503, "the tower is full");
            case TowerIntakeStatus::Failed: break;
            }
            return Error(500, "the tower could not keep the package");  // the reason stays in the tower's log
        }
        const std::string armed = "/v1/armed/";
        if (target.compare(0, armed.size(), armed) == 0) {
            if (method != "GET") return Error(405, "use GET");
            const std::string hash = target.substr(armed.size());
            if (!IsTowerPackageHash(hash)) return Error(400, "not a package hash");
            Json::Value v(Json::objectValue);
            const auto age = TowerIntakeArmedAge(inbox_dir, hash, now);
            v["armed"] = age.has_value();
            if (age) v["age"] = *age;
            return Reply(200, v);
        }
        return Error(404, "not found");
    } catch (const std::exception&) {
        return Error(500, "the tower could not answer");
    }
}

std::string TowerUrlProblem(const std::string& url) {
    auto after = [&](const std::string& scheme) { return url.substr(scheme.size()); };
    std::string rest;
    bool local_only = false;
    if (url.compare(0, 8, "https://") == 0) {
        rest = after("https://");
    } else if (url.compare(0, 7, "http://") == 0) {
        rest = after("http://");
        local_only = true;
    } else {
        return "a watchtower address starts with https://";
    }
    const std::string host_port = rest.substr(0, rest.find('/'));
    const std::string host = host_port.substr(0, host_port.rfind(':') == std::string::npos ? host_port.size()
                                                                                             : host_port.rfind(':'));
    if (host.empty()) return "the watchtower address has no host";
    for (const char c : url) {
        if (c <= ' ' || c == '"' || c == '\\' || c == '?' || c == '#' || c == '@' || static_cast<unsigned char>(c) > 126) {
            return "the watchtower address has a character that does not belong in it";
        }
    }
    if (local_only && host != "127.0.0.1" && host != "localhost") {
        return "a watchtower on another machine needs https://";
    }
    return {};
}

std::function<void(const std::string&)> MakeHttpTowerSink(std::string url, BtcHttp http) {
    url = WithoutTrailingSlash(std::move(url));
    return [url, http = std::move(http)](const std::string& package_text) {
        const BtcHttpResponse r = http.post(url + "/v1/packages", package_text);
        if (r.status == 0) throw std::runtime_error("the watchtower did not answer");
        const auto v = ParseObject(r.body);
        if (r.status != 200) {
            const std::string why = v && (*v)["error"].isString() ? (*v)["error"].asString().substr(0, 200) : std::string();
            throw std::runtime_error("the watchtower refused the package (" + std::to_string(r.status) +
                                     (why.empty() ? "" : ": " + why) + ")");
        }
        // It must hold exactly what was sent, not merely say yes.
        if (!v || !(*v)["hash"].isString() || (*v)["hash"].asString() != TowerPackageHash(package_text)) {
            throw std::runtime_error("the watchtower's answer does not name this package");
        }
    };
}

std::function<bool(const std::string&, const std::string&)> MakeHttpTowerAck(std::string url, BtcHttp http) {
    url = WithoutTrailingSlash(std::move(url));
    return [url, http = std::move(http)](const std::string&, const std::string& package_hash) {
        if (!IsTowerPackageHash(package_hash)) return false;
        const BtcHttpResponse r = http.get(url + "/v1/armed/" + package_hash);
        if (r.status != 200) return false;
        const auto v = ParseObject(r.body);
        return v && (*v)["armed"].isBool() && (*v)["armed"].asBool();
    };
}

}  // namespace dinero::swap
