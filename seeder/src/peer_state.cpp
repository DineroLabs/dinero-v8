#include "dinero/seeder/peer_state.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dinero {
namespace seeder {

namespace {

constexpr const char* kStateFileHeader = "# DINERO_SEEDER_PEERS_V1";

int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Encode a string as `<len> <chars>`, where len is decimal digits then a
// space, then the literal bytes, then a trailing space. Avoids needing
// to escape spaces/newlines inside the string (think User-Agent, error
// messages). Reader pairs read length first.
std::string encode_var(const std::string& s) {
    std::ostringstream oss;
    oss << s.size() << ' ';
    oss.write(s.data(), static_cast<std::streamsize>(s.size()));
    oss << ' ';
    return oss.str();
}

// Read a var-string from `iss` (len decimal, space, bytes, space).
bool read_var(std::istringstream& iss, std::string& out) {
    size_t len = 0;
    if (!(iss >> len)) return false;
    char space = 0;
    iss.get(space);
    if (space != ' ') return false;
    out.assign(len, '\0');
    iss.read(out.data(), static_cast<std::streamsize>(len));
    if (static_cast<size_t>(iss.gcount()) != len) return false;
    iss.get(space);  // trailing space (or end of line)
    return true;
}

}  // namespace

void PeerStore::seed(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = ip + ":" + std::to_string(port);
    auto it = peers_.find(key);
    if (it != peers_.end()) return;  // already known
    PeerStats stats;
    stats.ip = ip;
    stats.port = port;
    stats.first_seen_unix = now_unix();
    peers_.emplace(key, std::move(stats));
}

void PeerStore::record_attempt(const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = ip + ":" + std::to_string(port);
    auto& stats = peers_[key];
    if (stats.ip.empty()) {  // fresh entry
        stats.ip = ip;
        stats.port = port;
        stats.first_seen_unix = now_unix();
    }
    stats.last_attempt_unix = now_unix();
    stats.attempts++;
}

void PeerStore::record_success(const std::string& ip,
                                uint16_t port,
                                const std::string& user_agent,
                                uint32_t protocol_version,
                                uint32_t best_height) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = ip + ":" + std::to_string(port);
    auto& stats = peers_[key];
    stats.ip = ip;
    stats.port = port;
    stats.last_success_unix = now_unix();
    stats.successes++;
    stats.last_user_agent = user_agent;
    stats.last_protocol_version = protocol_version;
    stats.last_best_height = best_height;
    stats.last_error.clear();
}

void PeerStore::record_failure(const std::string& ip,
                                uint16_t port,
                                const std::string& error_detail) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string key = ip + ":" + std::to_string(port);
    auto& stats = peers_[key];
    if (stats.ip.empty()) {
        stats.ip = ip;
        stats.port = port;
        stats.first_seen_unix = now_unix();
    }
    stats.last_error = error_detail;
}

std::vector<PeerStats> PeerStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<PeerStats> out;
    out.reserve(peers_.size());
    for (const auto& kv : peers_) out.push_back(kv.second);
    return out;
}

std::vector<PeerStats> PeerStore::select_for_probe(size_t n) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<PeerStats> all;
    all.reserve(peers_.size());
    for (const auto& kv : peers_) all.push_back(kv.second);

    // Newly-seeded peers (attempts == 0) sort to the front; otherwise
    // oldest attempt first. This is the simplest fair-rotation policy:
    // every peer eventually gets probed without starving newcomers.
    std::sort(all.begin(), all.end(),
              [](const PeerStats& a, const PeerStats& b) {
                  if ((a.attempts == 0) != (b.attempts == 0)) {
                      return a.attempts == 0;
                  }
                  return a.last_attempt_unix < b.last_attempt_unix;
              });
    if (all.size() > n) all.resize(n);
    return all;
}

size_t PeerStore::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return peers_.size();
}

bool PeerStore::save(const std::string& path) const {
    // Atomic write: .tmp + rename. Mirrors src/daemon/p2p_manager.cpp's
    // peers.dat hardening (Phase C of v8 peer-discovery work).
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << kStateFileHeader << "\n";

        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& kv : peers_) {
            const auto& s = kv.second;
            f << s.ip << ' ' << s.port << ' '
              << s.first_seen_unix << ' '
              << s.last_attempt_unix << ' '
              << s.last_success_unix << ' '
              << s.attempts << ' '
              << s.successes << ' '
              << s.last_protocol_version << ' '
              << s.last_best_height << ' '
              << encode_var(s.last_user_agent)
              << encode_var(s.last_error)
              << "\n";
        }
        f.flush();
    }
#ifndef _WIN32
    int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool PeerStore::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::lock_guard<std::mutex> lock(mu_);
    peers_.clear();

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        PeerStats s;
        if (!(iss >> s.ip >> s.port >> s.first_seen_unix >> s.last_attempt_unix
                  >> s.last_success_unix >> s.attempts >> s.successes
                  >> s.last_protocol_version >> s.last_best_height)) {
            continue;  // skip malformed
        }
        // Step over the space after the last numeric field.
        char space = 0;
        iss.get(space);

        std::string ua, err;
        if (!read_var(iss, ua) || !read_var(iss, err)) continue;
        s.last_user_agent = std::move(ua);
        s.last_error = std::move(err);

        peers_.emplace(s.key(), std::move(s));
    }
    return true;
}

}  // namespace seeder
}  // namespace dinero
