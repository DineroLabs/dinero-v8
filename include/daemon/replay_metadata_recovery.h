#pragma once

#include "consensus/utreexo_accumulator.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dinero {

// Ephemeral transport bookkeeping for repairing durable CSN replay records.
// This class never sends, reads storage, validates consensus or calls a caller
// callback. Every returned value is owned, and the mutex is released before
// return. Callers reconstruct needs from durable legacy records after restart
// and recheck original_record before committing a verified replacement.
class ReplayMetadataRecoveryQueue {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr size_t kMaxRecords = 4;
    static constexpr size_t kMaxOriginalRecordBytes = 4 * 1024 * 1024;
    // Bound decoded storage, including vector element objects, rather than
    // trusting the wire frame's length or serializing another large copy.
    static constexpr size_t kMaxProofBytes = 1024 * 1024;
    static constexpr auto kRequestTimeout = std::chrono::seconds(30);
    static constexpr auto kRetryDelay = std::chrono::seconds(30);

    struct Attempt {
        uint256 hash;
        uint32_t height{0};
        std::string peer;
        uint64_t sequence{0};
        std::string original_record;
    };

    struct Ready {
        Attempt attempt;
        consensus::BlockUtreexoData proof_data;
        consensus::UtreexoHash root_after;
    };

    // Repeated observations preserve the current request/backoff. Changed
    // height or durable bytes supersede the old request and its completion.
    // False means no state changed because an explicit capacity was exceeded.
    bool Need(const uint256& hash, uint32_t height,
              const std::string& original_record, TimePoint now) {
        if (original_record.size() > kMaxOriginalRecordBytes) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(hash);
        if (it != entries_.end() && it->second.attempt.height == height &&
            it->second.attempt.original_record == original_record) {
            return true;
        }
        if (it == entries_.end() && entries_.size() >= kMaxRecords) return false;
        Entry replacement;
        replacement.attempt.hash = hash;
        replacement.attempt.height = height;
        replacement.attempt.original_record = original_record;
        replacement.retry_at = now;
        if (it == entries_.end()) {
            entries_.emplace(hash, std::move(replacement));
        } else {
            it->second = std::move(replacement);
        }
        return true;
    }

    // Returns at most one request; callers may drain up to the fixed capacity.
    // A timeout itself has already waited 30 seconds, so it retries immediately
    // on the next call. No peers leaves the need pending without consuming an
    // attempt. Rotation follows the last peer identity across reordered lists.
    std::optional<Attempt> NextRequest(TimePoint now,
                                       const std::vector<std::string>& peers) {
        std::vector<std::string> eligible;
        eligible.reserve(peers.size());
        for (const auto& peer : peers) {
            if (!peer.empty() &&
                std::find(eligible.begin(), eligible.end(), peer) == eligible.end()) {
                eligible.push_back(peer);
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* candidate = nullptr;
        for (auto& [hash, entry] : entries_) {
            Expire(entry, now);
            if (entry.state == State::Waiting && now >= entry.retry_at &&
                (!candidate || entry.attempt.sequence < candidate->attempt.sequence)) {
                candidate = &entry;
            }
        }
        if (!candidate || eligible.empty()) return std::nullopt;
        size_t peer_index = 0;
        const auto last = std::find(eligible.begin(), eligible.end(), candidate->attempt.peer);
        if (last != eligible.end()) {
            peer_index = (static_cast<size_t>(std::distance(eligible.begin(), last)) + 1) % eligible.size();
        }
        candidate->attempt.peer = eligible[peer_index];
        candidate->attempt.sequence = ++sequence_;
        candidate->deadline = now + kRequestTimeout;
        candidate->state = State::InFlight;
        return candidate->attempt;
    }

    // Transport identity and memory checks only. The caller must authenticate
    // this proof against local headers, exact legacy targets and a scratch
    // forest before calling Finish(success=true). Malformed/wrong responses
    // leave the valid request alive until its ordinary timeout.
    bool QueueResponse(const std::string& peer, const uint256& hash,
                       uint32_t height, const consensus::BlockUtreexoData& proof_data,
                       const consensus::UtreexoHash& root_after, TimePoint now) {
        if (!ProofFits(proof_data, root_after)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(hash);
        if (it == entries_.end()) return false;
        auto& entry = it->second;
        Expire(entry, now);
        if (entry.state != State::InFlight || entry.attempt.peer != peer ||
            entry.attempt.height != height) return false;
        entry.response = Payload{proof_data, root_after};
        entry.state = State::Ready;
        return true;
    }

    // Transfers proof ownership exactly once. The entry remains occupied while
    // validation runs, so another reply cannot race or timeout the owned work.
    std::optional<Ready> TakeResponse() {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* candidate = nullptr;
        for (auto& [hash, entry] : entries_) {
            if (entry.state == State::Ready &&
                (!candidate || entry.attempt.sequence < candidate->attempt.sequence)) {
                candidate = &entry;
            }
        }
        if (!candidate) return std::nullopt;
        Ready ready{candidate->attempt,
                    std::move(candidate->response->proof_data),
                    std::move(candidate->response->root_after)};
        candidate->response.reset();
        candidate->state = State::Processing;
        return ready;
    }

    // Failure also handles a synchronous send failure before any response.
    // Success requires TakeResponse first. A stale worker cannot erase or
    // postpone a newer request, a superseded record or a cancelled need.
    bool Finish(const Attempt& attempt, bool success, TimePoint now) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(attempt.hash);
        if (it == entries_.end()) return false;
        auto& entry = it->second;
        if (entry.state == State::Waiting || !Matches(entry.attempt, attempt) ||
            (success && entry.state != State::Processing)) return false;
        if (success) {
            entries_.erase(it);
        } else {
            Defer(entry, now);
        }
        return true;
    }

    bool NotFound(const std::string& peer, const uint256& hash, TimePoint now) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(hash);
        if (it == entries_.end()) return false;
        auto& entry = it->second;
        Expire(entry, now);
        if (entry.state != State::InFlight || entry.attempt.peer != peer) return false;
        Defer(entry, now);
        return true;
    }

    bool Cancel(const uint256& hash) {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.erase(hash) != 0;
    }

    bool HasPending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !entries_.empty();
    }

    // Lets the ingress route consume unmatched replies for a repair hash
    // without offering them to the normal forward/reorg block queues.
    bool Contains(const uint256& hash) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.find(hash) != entries_.end();
    }

    // Owned snapshot for pruning obsolete durable records/branches. Entries
    // that have not yet been requested have an empty peer and sequence zero.
    // No queue lock is retained while the caller inspects storage or ancestry.
    std::vector<Attempt> Needs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Attempt> needs;
        needs.reserve(entries_.size());
        for (const auto& [hash, entry] : entries_) needs.push_back(entry.attempt);
        return needs;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    enum class State { Waiting, InFlight, Ready, Processing };

    struct Payload {
        consensus::BlockUtreexoData proof_data;
        consensus::UtreexoHash root_after;
    };

    struct Entry {
        Attempt attempt;
        State state{State::Waiting};
        TimePoint retry_at{};
        TimePoint deadline{};
        std::optional<Payload> response;
    };

    static bool Matches(const Attempt& current, const Attempt& given) {
        return current.sequence == given.sequence && current.peer == given.peer &&
               current.height == given.height && current.original_record == given.original_record;
    }

    static void Expire(Entry& entry, TimePoint now) {
        if (entry.state == State::InFlight && now >= entry.deadline) {
            entry.state = State::Waiting;
            entry.retry_at = now;
        }
    }

    static void Defer(Entry& entry, TimePoint now) {
        entry.response.reset();
        entry.state = State::Waiting;
        entry.retry_at = now + kRetryDelay;
    }

    static bool ProofFits(const consensus::BlockUtreexoData& data,
                          const consensus::UtreexoHash& root_after) {
        if (root_after.size() != 32 || data.accumulator_root_before.size() != 32 ||
            data.spend_proof.targets.size() != data.spend_proof.positions.size()) return false;
        size_t remaining = kMaxProofBytes;
        const auto consume = [&remaining](size_t size) {
            if (size > remaining) return false;
            remaining -= size;
            return true;
        };
        const auto consume_elements = [&remaining](size_t count, size_t element_size) {
            if (count > remaining / element_size) return false;
            remaining -= count * element_size;
            return true;
        };
        if (!consume(sizeof(Payload) + 64) ||
            !consume_elements(data.spend_proof.targets.size(), sizeof(consensus::UtreexoHash)) ||
            !consume_elements(data.spend_proof.positions.size(), sizeof(uint64_t)) ||
            !consume_elements(data.spend_proof.proof_hashes.size(), sizeof(consensus::UtreexoHash)) ||
            !consume_elements(data.spent_outputs.size(), sizeof(consensus::SpentOutputData))) return false;
        for (const auto& hash : data.spend_proof.targets) {
            if (hash.size() != 32 || !consume(hash.size())) return false;
        }
        for (const auto& hash : data.spend_proof.proof_hashes) {
            if (hash.size() != 32 || !consume(hash.size())) return false;
        }
        for (const auto& spent : data.spent_outputs) {
            if (!consume(spent.scriptPubKey.size()) || !consume(spent.commitment.size())) return false;
        }
        return true;
    }

    mutable std::mutex mutex_;
    std::map<uint256, Entry> entries_;
    uint64_t sequence_{0};
};

} // namespace dinero
