#include "bridge/http_client.h"
#include "common/logger.h"
#include <curl/curl.h>
#include <sstream>

namespace dinero {
namespace bridge {

// CURL write callback - accumulates response body
size_t HttpClient::write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// CURL header callback - parses response headers
size_t HttpClient::header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);

    std::string header(buffer, total_size);
    size_t separator = header.find(':');
    if (separator != std::string::npos) {
        std::string key = header.substr(0, separator);
        std::string value = header.substr(separator + 1);

        // Trim whitespace
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        headers->emplace(key, value);
    }

    return total_size;
}

HttpClient::Response HttpClient::get(const std::string& url,
                                     const std::map<std::string, std::string>& headers,
                                     int timeout_seconds) {
    Response response;
    CURL* curl = curl_easy_init();

    if (!curl) {
        response.error = "Failed to initialize CURL";
        return response;
    }

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Set callback for response body
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    // Set callback for response headers
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    // Set timeout
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // Set custom headers
    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header_str = key + ": " + value;
        header_list = curl_slist_append(header_list, header_str.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        response.status_code = static_cast<int>(http_code);
        response.success = (http_code >= 200 && http_code < 300);
    } else {
        response.error = curl_easy_strerror(res);
        g_logger.error("[HttpClient] GET failed: " + url + " - " + response.error);
    }

    // Cleanup
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);

    return response;
}

HttpClient::Response HttpClient::post(const std::string& url,
                                      const std::string& body,
                                      const std::map<std::string, std::string>& headers,
                                      int timeout_seconds) {
    Response response;
    CURL* curl = curl_easy_init();

    if (!curl) {
        response.error = "Failed to initialize CURL";
        return response;
    }

    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Set POST method
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    // Set POST body
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());

    // Set callback for response body
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    // Set callback for response headers
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    // Set timeout
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // Set custom headers
    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header_str = key + ": " + value;
        header_list = curl_slist_append(header_list, header_str.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        response.status_code = static_cast<int>(http_code);
        response.success = (http_code >= 200 && http_code < 300);
    } else {
        response.error = curl_easy_strerror(res);
        g_logger.error("[HttpClient] POST failed: " + url + " - " + response.error);
    }

    // Cleanup
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    curl_easy_cleanup(curl);

    return response;
}

} // namespace bridge
} // namespace dinero
