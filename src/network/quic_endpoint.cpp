#include "network/quic_endpoint.h"

#include <algorithm>

namespace dinero::network {

namespace {

std::array<uint8_t, 17> IpKey(const UdpAddr& a) {
    std::array<uint8_t, 17> k{};
    k[0] = static_cast<uint8_t>(a.family);
    const size_t n = a.family == UdpAddr::Family::V4 ? 4 : 16;
    std::copy_n(a.ip.begin(), n, k.begin() + 1);
    return k;
}

// Unknown-source bookkeeping is bounded: past this many tracked IPs the table
// is reset (a flood of distinct IPs then only ever meets the global bucket).
constexpr size_t kMaxTrackedIps = 4096;
// A session that never became active (junk Initial, refused handshake) stops
// receiving routing after this long, so its remote can try again.
constexpr auto kDeadSessionGrace = std::chrono::seconds(10);

}  // namespace

struct QuicEndpoint::Shared {
    UdpSocket socket;
    std::atomic<bool> alive{false};
};

QuicEndpoint::QuicEndpoint() : shared_(std::make_shared<Shared>()) {}

QuicEndpoint::~QuicEndpoint() { Stop(); }

bool QuicEndpoint::LooksLikeClientInitial(const uint8_t* data, size_t len) {
    // RFC 9000 §17.2.2: long header (0x80) + fixed bit (0x40), packet type 0
    // (Initial) in bits 4-5, version 1; client Initials are padded to >= 1200.
    if (len < 1200) return false;
    if ((data[0] & 0xC0) != 0xC0) return false;
    if (((data[0] >> 4) & 0x03) != 0x00) return false;
    return data[1] == 0 && data[2] == 0 && data[3] == 0 && data[4] == 1;
}

bool QuicEndpoint::Start(uint16_t port, QuicSessionOptions server_opts, AcceptFn accept,
                         InboundFn inbound) {
    return Start(port, std::move(server_opts), std::move(accept), std::move(inbound), Limits{});
}

bool QuicEndpoint::Start(uint16_t port, QuicSessionOptions server_opts, AcceptFn accept,
                         InboundFn inbound, Limits limits) {
#if !(defined(DINERO_HAVE_NGTCP2) && defined(DINERO_HAVE_NGTCP2_CRYPTO_OSSL))
    (void)port; (void)server_opts; (void)accept; (void)inbound; (void)limits;
    return false;
#else
    if (shared_->alive.load()) return false;
    server_opts_ = std::move(server_opts);
    accept_ = std::move(accept);
    inbound_ = std::move(inbound);
    limits_ = limits;
    global_bucket_ = {static_cast<double>(limits_.initials_per_sec_global),
                      std::chrono::steady_clock::now()};
    shared_->socket.OnReceive([this](const UdpAddr& src, std::vector<uint8_t> data) {
        OnDatagram(src, std::move(data));
    });
    if (!shared_->socket.Bind(port)) return false;
    shared_->alive.store(true);
    return true;
#endif
}

UdpAddr QuicEndpoint::LocalAddr() const {
    const uint8_t any[4] = {0, 0, 0, 0};
    return UdpAddr::FromIPv4(any, shared_->socket.bound_port());
}

std::shared_ptr<QuicSession> QuicEndpoint::MakeSession(const UdpAddr& remote) {
    // The writer holds the shared socket state, never `this`: a session thread
    // can outlive the endpoint by a few packets during shutdown.
    std::weak_ptr<Shared> weak = shared_;
    return std::make_shared<QuicSession>([weak, remote](std::vector<uint8_t> bytes) {
        if (auto s = weak.lock(); s && s->alive.load()) {
            s->socket.SendTo(remote, bytes.data(), bytes.size());
        }
    });
}

std::shared_ptr<QuicSession> QuicEndpoint::Connect(const UdpAddr& remote,
                                                   QuicSessionOptions client_opts) {
    if (!shared_->alive.load()) return nullptr;
    const auto key = std::make_pair(IpKey(remote), remote.port);
    std::shared_ptr<QuicSession> session;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (sessions_.count(key)) return nullptr;
        session = MakeSession(remote);
        sessions_[key] = {session, std::chrono::steady_clock::now()};
    }
    if (!session->StartClient(LocalAddr(), remote, client_opts)) {
        Forget(remote);
        return nullptr;
    }
    return session;
}

void QuicEndpoint::Forget(const UdpAddr& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_.erase(std::make_pair(IpKey(remote), remote.port));
}

bool QuicEndpoint::TakeToken(Bucket& b, uint32_t per_sec,
                             std::chrono::steady_clock::time_point now) {
    const double elapsed = std::chrono::duration<double>(now - b.last).count();
    b.tokens = std::min<double>(per_sec, b.tokens + elapsed * per_sec);
    b.last = now;
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
}

void QuicEndpoint::OnDatagram(const UdpAddr& src, std::vector<uint8_t> data) {
    if (data.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    const auto key = std::make_pair(IpKey(src), src.port);
    std::shared_ptr<QuicSession> session;
    bool created = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(key);
        if (it != sessions_.end()) {
            const bool dead = !it->second.session->active() &&
                              now - it->second.created > kDeadSessionGrace;
            if (dead && LooksLikeClientInitial(data.data(), data.size())) {
                sessions_.erase(it);  // the remote is re-dialing a dead session
            } else {
                ++counters_.datagrams_routed;
                session = it->second.session;
            }
        }
        if (!session) {
            if (!LooksLikeClientInitial(data.data(), data.size())) {
                ++counters_.datagrams_dropped;
                return;
            }
            if (ip_buckets_.size() >= kMaxTrackedIps) ip_buckets_.clear();
            auto [bit, inserted] = ip_buckets_.try_emplace(
                IpKey(src), Bucket{static_cast<double>(limits_.initials_per_sec_per_ip), now});
            if (!TakeToken(bit->second, limits_.initials_per_sec_per_ip, now) ||
                !TakeToken(global_bucket_, limits_.initials_per_sec_global, now)) {
                ++counters_.initials_rate_limited;
                ++counters_.datagrams_dropped;
                return;
            }
            if (accept_ && !accept_(src)) {
                ++counters_.datagrams_dropped;
                return;
            }
            session = MakeSession(src);
            sessions_[key] = {session, now};
            ++counters_.sessions_accepted;
            created = true;
        }
    }
    if (created) {
        session->StartServer(LocalAddr(), src, server_opts_);
    }
    session->EnqueueIncomingPacket(std::move(data));
    if (created && inbound_) {
        inbound_(session, src);
    }
}

void QuicEndpoint::Stop() {
    if (!shared_->alive.exchange(false)) return;
    shared_->socket.Stop();
    std::map<std::pair<std::array<uint8_t, 17>, uint16_t>, Entry> sessions;
    {
        std::lock_guard<std::mutex> lock(mu_);
        sessions.swap(sessions_);
    }
    for (auto& [k, e] : sessions) e.session->Close();
}

bool QuicEndpoint::Listening() const { return shared_->alive.load(); }

uint16_t QuicEndpoint::Port() const { return shared_->socket.bound_port(); }

size_t QuicEndpoint::SessionCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return sessions_.size();
}

QuicEndpoint::Counters QuicEndpoint::Stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return counters_;
}

}  // namespace dinero::network
