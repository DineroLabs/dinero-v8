#pragma once

// Which of our own addresses other nodes have proven they can dial.
//
// A node used to advertise any address that 3 peers *reported* seeing us at,
// which behind NAT is the router's address -- usually not dialable at all, or
// (on a shared home connection) another machine entirely. An address is now
// advertised only after verifiers in at least `min_groups` distinct network
// groups have dialed it back and reached THIS node, within `window`. Verified
// addresses are re-checked every `recheck`; `fail_rounds_to_drop` consecutive
// failed rounds un-verify them.
//
// Pure logic: no sockets, no P2PManager; the clock is injected. P2PManager
// asks PlanChecks() which dial-backs to request and feeds results back.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dinero::p2p {

enum class Transport : uint8_t { Tcp = 0, Quic = 1 };
enum class CandidateSource { PeerVote, Stun, PortMapping, ExternalIp };
enum class VerifyState { Unverified, Verifying, Verified };

struct Candidate {
    std::string ip;
    uint16_t port{0};
    Transport transport{Transport::Tcp};
    CandidateSource source{CandidateSource::PeerVote};
};

struct CandidateStatus {
    Candidate c;
    VerifyState state{VerifyState::Unverified};
    uint32_t ok_groups{0};
    std::chrono::system_clock::time_point verified_at{};
};

// A connected, identity-proven peer that answers reachability checks.
struct Verifier {
    std::string peer_key;
    std::string ip;
};

class SelfReachability {
public:
    struct Params {
        uint32_t min_groups = 2;
        std::chrono::minutes window{10};
        std::chrono::minutes recheck{30};
        uint32_t fail_rounds_to_drop = 2;
    };
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    SelfReachability(Params params, Clock clock);

    void AddCandidate(const Candidate& c);
    void RemoveCandidate(const Candidate& c);
    // The (candidate, verifier) checks to send now. Never two verifiers from
    // one network group for a candidate in a round.
    std::vector<std::pair<Candidate, Verifier>> PlanChecks(const std::vector<Verifier>& available);
    // Same, for candidates of one transport only; the others are left alone.
    std::vector<std::pair<Candidate, Verifier>> PlanChecks(const std::vector<Verifier>& available,
                                                           Transport only);
    // result_code 0 = the verifier reached this node at the address.
    void OnResult(const Candidate& c, const Verifier& v, uint8_t result_code);
    // Local address or network changed: nothing stays verified until re-checked.
    void OnNetworkChange();

    bool IsVerified(const Candidate& c) const;
    bool AnyVerified(Transport t) const;
    std::vector<CandidateStatus> Status() const;

private:
    using Key = std::tuple<std::string, uint16_t, uint8_t>;
    struct Entry {
        Candidate c;
        VerifyState state{VerifyState::Unverified};
        std::vector<std::pair<std::string /*group*/, std::chrono::steady_clock::time_point>> oks;
        std::chrono::steady_clock::time_point verified_at{};
        std::chrono::steady_clock::time_point last_round{};
        bool round_open{false};
        size_t round_pending{0};
        bool round_had_ok{false};
        uint32_t failed_rounds{0};
    };
    static Key KeyOf(const Candidate& c);
    std::vector<std::pair<Candidate, Verifier>> Plan(const std::vector<Verifier>& available,
                                                     std::optional<Transport> only);
    uint32_t OkGroups(Entry& e) const;  // also prunes oks outside the window
    void CloseRound(Entry& e);

    Params params_;
    Clock clock_;
    std::map<Key, Entry> entries_;
};

}  // namespace dinero::p2p
