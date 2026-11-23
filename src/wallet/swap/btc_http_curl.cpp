#include "wallet/swap/btc_http_curl.h"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

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

std::atomic<bool> g_interrupted{false};

// Asked several times a second during a transfer: nonzero ends it at once.
int OnProgress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return g_interrupted.load() ? 1 : 0; }

// Handles kept between requests, each with its open connections: the next
// request to the same server skips the TCP and TLS handshakes, which is most
// of the time a short request takes. A wallet reading asks some eighty
// questions in a row. A handle is used by one request at a time and is wiped
// (curl_easy_reset) before each: no option of one request reaches the next.
class Handles {
public:
    ~Handles() {
        for (CURL* h : idle_) curl_easy_cleanup(h);
    }
    CURL* Take() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!idle_.empty()) {
                CURL* h = idle_.back();
                idle_.pop_back();
                curl_easy_reset(h);
                return h;
            }
        }
        return curl_easy_init();
    }
    void Return(CURL* h) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (idle_.size() < 4) {
                idle_.push_back(h);
                return;
            }
        }
        curl_easy_cleanup(h);
    }

    // A server that let a request hang is left alone for a while: a wallet
    // reading asks about forty addresses in a row, and waiting the full time
    // limit for each of them at a server that has gone quiet (or is slowing
    // us down on purpose) turns seconds into minutes. The other servers
    // answer meanwhile; "no answer" from this one means what it always meant.
    bool Resting(const std::string& server) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = resting_.find(server);
        if (it == resting_.end()) return false;
        if (std::chrono::steady_clock::now() < it->second) return true;
        resting_.erase(it);
        return false;
    }
    void Rest(const std::string& server) {
        std::lock_guard<std::mutex> lock(mu_);
        resting_[server] = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }

private:
    std::mutex mu_;
    std::vector<CURL*> idle_;
    std::map<std::string, std::chrono::steady_clock::time_point> resting_;
};

// "https://host:port" of a URL.
std::string ServerOf(const std::string& url) {
    const size_t start = url.find("://");
    const size_t end = start == std::string::npos ? std::string::npos : url.find('/', start + 3);
    return url.substr(0, end);
}

BtcHttpResponse Perform(const BtcHttpOptions& options, Handles& handles, const std::string& url,
                        const std::string* post_body) {
    BtcHttpResponse response;
    const bool https = url.rfind("https://", 0) == 0;
    if (!https && !PlainHttpOnThisMachine(url)) return response;  // never plain http to the outside
    if (g_interrupted.load()) return response;
    // Only questions are held back. Something being sent (a transaction) is always tried.
    const std::string server = ServerOf(url);
    if (!post_body && handles.Resting(server)) return response;
    const auto started = std::chrono::steady_clock::now();

    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = handles.Take();
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
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);

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
    // A transfer that ended badly may leave its connection in any state: drop the handle with it.
    if (result == CURLE_OK) handles.Return(curl); else curl_easy_cleanup(curl);
    // It failed slowly (a refusal on the spot costs nothing to ask again).
    if (result != CURLE_OK && !sink.overflowed && !g_interrupted.load() &&
        std::chrono::steady_clock::now() - started >= std::chrono::seconds(3)) {
        handles.Rest(server);
    }

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
    const auto handles = std::make_shared<Handles>();
    http.get = [options, handles](const std::string& url) { return Perform(options, *handles, url, nullptr); };
    http.post = [options, handles](const std::string& url, const std::string& body) {
        return Perform(options, *handles, url, &body);
    };
    return http;
}

void InterruptBtcHttp(bool interrupted) { g_interrupted.store(interrupted); }

}  // namespace dinero::swap
