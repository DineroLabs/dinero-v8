#pragma once
#include <string>
#include <map>
#include <optional>

namespace dinero {
namespace bridge {

/**
 * Simple HTTP client for bridge API requests
 * Uses libcurl for cross-platform HTTP/HTTPS support
 */
class HttpClient {
public:
    struct Response {
        int status_code = 0;
        std::string body;
        std::map<std::string, std::string> headers;
        bool success = false;
        std::string error;
    };

    /**
     * Perform HTTP GET request
     * @param url Full URL to request
     * @param headers Optional HTTP headers
     * @param timeout_seconds Request timeout (default: 30s)
     */
    static Response get(const std::string& url,
                       const std::map<std::string, std::string>& headers = {},
                       int timeout_seconds = 30);

    /**
     * Perform HTTP POST request
     * @param url Full URL to request
     * @param body Request body (JSON or form data)
     * @param headers Optional HTTP headers
     * @param timeout_seconds Request timeout (default: 30s)
     */
    static Response post(const std::string& url,
                        const std::string& body,
                        const std::map<std::string, std::string>& headers = {},
                        int timeout_seconds = 30);

private:
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata);
};

} // namespace bridge
} // namespace dinero
