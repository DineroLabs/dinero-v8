#include "wallet/swap/btc_http_curl.h"

#include <curl/curl.h>

#include <mutex>

namespace dinero::swap {
namespace {

struct Sink {
    std::string body;
    size_t limit{0};
    bool overflowed{false};
};

size_t OnBody(char* data, size_t size, size_t count, void* user) {
    auto* sink = static_cast<Sink*>(user);
    const size_t n = size * count;
    const size_t room = sink->limit + 1 - sink->body.size();  // one byte past the limit marks "too large"
    if (n > room) {
        sink->body.append(data, room);
        sink->overflowed = true;
        return 0;  // stops the transfer
    }
    sink->body.append(data, n);
    return n;
}

bool PlainHttpOnThisMachine(const std::string& url) {
    for (const char* prefix : {"http://127.0.0.1:", "http://127.0.0.1/", "http://localhost:", "http://localhost/"}) {
        if (url.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

BtcHttpResponse Perform(const BtcHttpOptions& options, const std::string& url, const std::string* post_body) {
    BtcHttpResponse response;
    const bool https = url.rfind("https://", 0) == 0;
    if (!https && !PlainHttpOnThisMachine(url)) return response;  // never plain http to the outside

    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (!curl) return response;

    Sink sink;
    sink.limit = options.max_body_bytes;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, https ? "https" : "http");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, https ? CURLPROTO_HTTPS : CURLPROTO_HTTP);
#endif
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!options.ca_bundle_path.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, options.ca_bundle_path.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, options.connect_timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, options.total_timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);          // timeouts without signals: safe in a threaded daemon
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   // compressed replies; the size limit counts what comes out
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "btc-light/1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

    curl_slist* headers = nullptr;
    if (post_body) {
        headers = curl_slist_append(headers, "Content-Type: text/plain");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body->data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(post_body->size()));
    }

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // A transfer we cut off for size still has a real status; any other failure
    // (no route, timeout, certificate, cut connection) is "no answer", whatever
    // part of a reply arrived.
    if (result == CURLE_OK || sink.overflowed) {
        response.status = static_cast<int>(status);
        response.body = std::move(sink.body);
    }
    return response;
}

}  // namespace

BtcHttp MakeBtcHttp(BtcHttpOptions options) {
    BtcHttp http;
    http.get = [options](const std::string& url) { return Perform(options, url, nullptr); };
    http.post = [options](const std::string& url, const std::string& body) { return Perform(options, url, &body); };
    return http;
}

}  // namespace dinero::swap
