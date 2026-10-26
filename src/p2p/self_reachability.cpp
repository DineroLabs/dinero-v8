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
    auto [it, inserted] = entries_.try_emplace(KeyOf(c));
    if (inserted) it->second.c = c;
}

void SelfReachability::RemoveCandidate(const Candidate& c) { entries_.erase(KeyOf(c)); }

uint32_t SelfReachability::OkGroups(Entry& e) const {
    const auto now = clock_();
    std::erase_if(e.oks, [&](const auto& ok) { return now - ok.second > params_.window; });
    std::set<std::string> groups;
    for (const auto& ok : e.oks) groups.insert(ok.first);
    return static_cast<uint32_t>(groups.size());
}

std::vector<std::pair<Candidate, Verifier>> SelfReachability::PlanChecks(
    const std::vector<Verifier>& available) {
    std::vector<std::pair<Candidate, Verifier>> plan;
    const auto now = clock_();
    for (auto& [key, e] : entries_) {
        if (e.round_open) continue;  // results still outstanding
        const bool due = e.state != VerifyState::Verified ||
                         now - e.verified_at >= params_.recheck;
        if (!due) continue;
        // A recheck needs fresh proof: groups that vouched for the previous
        // round do not carry over into this one.
        if (e.state == VerifyState::Verified) e.oks.clear();
        std::set<std::string> used;
        for (const auto& ok : e.oks) used.insert(ok.first);
        const uint32_t have = OkGroups(e);
        const uint32_t need = have >= params_.min_groups ? 0 : params_.min_groups - have;
        std::vector<Verifier> picked;
        for (const auto& v : available) {
            if (picked.size() >= need) break;
            const std::string group = AddrManNetGroup(v.ip);
            if (!used.insert(group).second) continue;  // one per network group
            picked.push_back(v);
        }
        if (picked.empty()) continue;
        if (e.state == VerifyState::Unverified) e.state = VerifyState::Verifying;
        e.round_open = true;
        e.round_pending = picked.size();
        e.round_had_ok = false;
        e.last_round = now;
        for (auto& v : picked) plan.emplace_back(e.c, std::move(v));
    }
    return plan;
}

void SelfReachability::CloseRound(Entry& e) {
    e.round_open = false;
    if (OkGroups(e) >= params_.min_groups) {
        e.failed_rounds = 0;
        return;
    }
    if (!e.round_had_ok) ++e.failed_rounds;
    if (e.state == VerifyState::Verified && e.failed_rounds >= params_.fail_rounds_to_drop) {
        e.state = VerifyState::Unverified;
        e.failed_rounds = 0;
    }
}

void SelfReachability::OnResult(const Candidate& c, const Verifier& v, uint8_t result_code) {
    auto it = entries_.find(KeyOf(c));
    if (it == entries_.end()) return;
    Entry& e = it->second;
    const auto now = clock_();
    if (result_code == 0) {
        e.oks.emplace_back(AddrManNetGroup(v.ip), now);
        e.round_had_ok = true;
        if (OkGroups(e) >= params_.min_groups) {
            e.state = VerifyState::Verified;
            e.verified_at = now;
            e.failed_rounds = 0;
        }
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
        std::set<std::string> groups;
        for (const auto& ok : e.oks) {
            if (steady_now - ok.second <= params_.window) groups.insert(ok.first);
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
