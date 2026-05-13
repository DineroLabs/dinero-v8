#pragma once
#include <string>
#include <optional>
#include "compat/jsoncpp_compat.h"
#include <curl/curl.h>
#include <fstream>
#include "NodeInfo.h"

class DaemonRpc {
public:
    explicit DaemonRpc(const NodeInfo& ni) : ni_(ni) {}

    std::optional<Json::Value> call(const std::string& method, const Json::Value& params, std::string* err = nullptr) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            if (err) *err = "Failed to initialize curl";
            return std::nullopt;
        }

        // Build JSON-RPC payload
        Json::Value payload;
        payload["jsonrpc"] = "2.0";
        payload["id"] = 1;
        payload["method"] = method;
        payload["params"] = params;
        
        Json::StreamWriterBuilder builder;
        std::string jsonData = Json::writeString(builder, payload);

        // Read cookie for auth
        std::ifstream cookieFile(ni_.cookiePath);
        if (!cookieFile.is_open()) {
            if (err) *err = "Failed to read cookie file";
            curl_easy_cleanup(curl);
            return std::nullopt;
        }
        std::string credentials;
        std::getline(cookieFile, credentials);
        cookieFile.close();

        // Set up curl
        curl_easy_setopt(curl, CURLOPT_URL, ni_.rpcUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD, credentials.c_str());
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        // Response handling
        std::string response;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            if (err) *err = curl_easy_strerror(res);
            return std::nullopt;
        }

        // Parse response
        Json::Value responseJson;
        Json::Reader reader;
        if (!reader.parse(response, responseJson)) {
            if (err) *err = "Failed to parse JSON response";
            return std::nullopt;
        }

        if (responseJson.isMember("error") && !responseJson["error"].isNull()) {
            if (err) {
                Json::StreamWriterBuilder builder;
                *err = Json::writeString(builder, responseJson["error"]);
            }
            return std::nullopt;
        }

        return responseJson.get("result", Json::Value());
    }

private:
    NodeInfo ni_;
    
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        size_t totalSize = size * nmemb;
        userp->append((char*)contents, totalSize);
        return totalSize;
    }
};
