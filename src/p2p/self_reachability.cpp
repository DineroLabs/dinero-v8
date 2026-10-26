#include "p2p/self_reachability.h"

#include "p2p/addrman.h"  // AddrManNetGroup

#include <set>

namespace dinero::p2p {

SelfReachability::SelfReachability(Params params, Clock clock)
    : params_(params), clock_(std::move(clock)) {}

SelfReachability::Key SelfReachability::KeyOf(const Candidate& c) {
    return {c.ip, c.port, static_cast<uint8_t>(c.transport)};
}

void SelfReachability::AddCandidate(const Candidate& c) {
    if (entries_.size() >= params_.max_candidates && !entries_.count(KeyOf(c))) return;
    auto [it, inserted] = entries_.try_emplace(KeyOf(c));
    if (inserted) it->second.c = c;
}

void SelfReachability::RemoveCandidate(const Candidate& c) { entries_.erase(KeyOf(c)); }

namespace {

constexpr std::chrono::seconds kBackoffBase{30};
constexpr std::chrono::seconds kBackoffMax{30 * 60};

bool IsV6(const std::string& ip) { return ip.find(':') != std::string::npos; }

// 1 = verifier busy, 4 = transport unsupported: no evidence either way.
bool IsNeutral(uint8_t code) { return code == 1 || code == 4; }

}  // namespace

uint32_t SelfReachability::OkGroups(Entry& e) const {
    const auto now = clock_();
    std::erase_if(e.oks, [&](const Ok& ok) { return now - ok.at > params_.window; });
    std::set<std::string> groups, nodes;
    for (const auto& ok : e.oks) {
        if (!ok.node_id.empty() && !nodes.insert(ok.node_id).second) continue;
        groups.insert(ok.group);
    }
    return static_cast<uint32_t>(groups.size());
}

std::vector<std::pair<Candidate, Verifier>> SelfReachability::PlanChecks(
    const std::vector<Verifier>& available) {
    return Plan(available, std::nullopt);
}

std::vector<std::pair<Candidate, Verifier>> SelfReachability::PlanChecks(
    const std::vector<Verifier>& available, Transport only) {
    return Plan(available, only);
}

std::vector<std::pair<Candidate, Verifier>> SelfReachability::Plan(
    const std::vector<Verifier>& available, std::optional<Transport> only) {
    std::vector<std::pair<Candidate, Verifier>> plan;
    const auto now = clock_();
    for (auto& [key, e] : entries_) {
        if (only && e.c.transport != *only) continue;
        if (e.round_open) continue;  // results still outstanding
        if (now < e.next_attempt) continue;  // backing off after a failed round
        const bool due = e.state != VerifyState::Verified ||
                         now - e.verified_at >= params_.recheck;
        if (!due) continue;
        // A recheck needs fresh proof: groups that vouched for the previous
        // round do not carry over into this one.
        if (e.state == VerifyState::Verified) e.oks.clear();
        const uint32_t have = OkGroups(e);
        const uint32_t need = have >= params_.min_groups ? 0 : params_.min_groups - have;
        std::set<std::string> used_groups, used_nodes;
        for (const auto& ok : e.oks) {
            used_groups.insert(ok.group);
            if (!ok.node_id.empty()) used_nodes.insert(ok.node_id);
        }
        std::vector<Verifier> picked;
        // Fresh verifiers first; ones that just failed this candidate only if
        // nobody else is left.
        for (const bool retry_failed : {false, true}) {
            for (const auto& v : available) {
                if (picked.size() >= need) break;
                if (IsV6(v.ip) != IsV6(e.c.ip)) continue;  // it would dial the other family
                if (e.recent_failed.count(v.peer_key) != retry_failed) continue;
                if (!v.node_id.empty() && used_nodes.count(v.node_id)) continue;
                if (!used_groups.insert(AddrManNetGroup(v.ip)).second) continue;  // one per group
                if (!v.node_id.empty()) used_nodes.insert(v.node_id);
                picked.push_back(v);
            }
        }
        if (picked.empty()) continue;
        if (e.state == VerifyState::Unverified && e.failed_rounds < params_.fail_rounds_to_drop) {
            e.state = VerifyState::Verifying;
        }
        e.round_open = true;
        e.round_pending = picked.size();
        e.round_had_ok = false;
        e.round_had_failure = false;
        e.last_round = now;
        for (auto& v : picked) plan.emplace_back(e.c, std::move(v));
    }
    return plan;
}

void SelfReachability::CloseRound(Entry& e) {
    e.round_open = false;
    const auto now = clock_();
    if (OkGroups(e) >= params_.min_groups) {
        e.failed_rounds = 0;
        e.backoff = std::chrono::seconds{0};
        e.next_attempt = {};
        return;
    }
    if (e.round_had_failure && !e.round_had_ok) {
        ++e.failed_rounds;
        e.backoff = std::min(kBackoffMax, e.backoff.count() == 0 ? kBackoffBase : e.backoff * 2);
    }
    e.next_attempt = now + std::max(kBackoffBase, e.backoff);
    if (e.failed_rounds >= params_.fail_rounds_to_drop) {
        if (e.state == VerifyState::Verified) {
            e.state = VerifyState::Unverified;
            e.failed_rounds = 0;
        } else if (e.state == VerifyState::Verifying) {
            e.state = VerifyState::Unverified;
        }
    }
}

void SelfReachability::OnResult(const Candidate& c, const Verifier& v, uint8_t result_code) {
    auto it = entries_.find(KeyOf(c));
    if (it == entries_.end()) return;
    Entry& e = it->second;
    const auto now = clock_();
    if (result_code == 0) {
        e.oks.push_back(Ok{AddrManNetGroup(v.ip), v.node_id, now});
        e.round_had_ok = true;
        e.recent_failed.erase(v.peer_key);
        if (OkGroups(e) >= params_.min_groups) {
            e.state = VerifyState::Verified;
            e.verified_at = now;
            e.failed_rounds = 0;
            e.backoff = std::chrono::seconds{0};
        }
    } else if (!IsNeutral(result_code)) {
        e.round_had_failure = true;
        e.recent_failed.insert(v.peer_key);
    }
    if (e.round_open && e.round_pending > 0 && --e.round_pending == 0) {
        CloseRound(e);
    }
}

void SelfReachability::OnNetworkChange() {
    for (auto& [key, e] : entries_) {
        if (e.state == VerifyState::Verified) e.state = VerifyState::Verifying;
        e.oks.clear();
        e.round_open = false;
        e.failed_rounds = 0;
        e.backoff = std::chrono::seconds{0};
        e.next_attempt = {};
        e.recent_failed.clear();
    }
}

bool SelfReachability::IsVerified(const Candidate& c) const {
    auto it = entries_.find(KeyOf(c));
    return it != entries_.end() && it->second.state == VerifyState::Verified;
}

bool SelfReachability::AnyVerified(Transport t) const {
    for (const auto& [key, e] : entries_) {
        if (e.c.transport == t && e.state == VerifyState::Verified) return true;
    }
    return false;
}

std::vector<CandidateStatus> SelfReachability::Status() const {
    std::vector<CandidateStatus> out;
    const auto steady_now = clock_();
    const auto sys_now = std::chrono::system_clock::now();
    for (const auto& [key, e] : entries_) {
        CandidateStatus st;
        st.c = e.c;
        st.state = e.state;
        std::set<std::string> groups, nodes;
        for (const auto& ok : e.oks) {
            if (steady_now - ok.at > params_.window) continue;
            if (!ok.node_id.empty() && !nodes.insert(ok.node_id).second) continue;
            groups.insert(ok.group);
        }
        st.ok_groups = static_cast<uint32_t>(groups.size());
        if (e.state == VerifyState::Verified) {
            st.verified_at = sys_now - std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                           steady_now - e.verified_at);
        }
        out.push_back(st);
    }
    return out;
}

}  // namespace dinero::p2p
