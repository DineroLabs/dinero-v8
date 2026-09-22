#include "daemon/replay_metadata_recovery.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using Queue = dinero::ReplayMetadataRecoveryQueue;
using dinero::consensus::BlockUtreexoData;
using dinero::consensus::UtreexoHash;
using namespace std::chrono_literals;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

dinero::uint256 Hash(uint8_t byte) {
    dinero::uint256 hash;
    hash.data[0] = byte;
    return hash;
}

BlockUtreexoData Proof() {
    BlockUtreexoData proof;
    proof.accumulator_root_before.assign(32, 1);
    proof.spend_proof.targets.emplace_back(32, 2);
    proof.spend_proof.positions.push_back(0);
    proof.spend_proof.proof_hashes.emplace_back(32, 3);
    proof.spent_outputs.emplace_back(100, std::vector<uint8_t>{0x51}, 1, false);
    return proof;
}

const Queue::TimePoint start{};
const std::vector<std::string> peers{"bridge-a", "bridge-b", "bridge-c"};
const UtreexoHash root(32, 4);

void TestNoPeerAndDuplicateNeedPreserveBackoff() {
    Queue queue;
    Require(queue.Need(Hash(1), 7, "legacy", start), "initial need accepted");
    Require(!queue.NextRequest(start, {}), "no peers means no attempt");
    Require(queue.HasPending() && queue.Size() == 1, "no-peer wait retains need");
    auto attempt = queue.NextRequest(start, peers);
    Require(attempt && attempt->peer == "bridge-a", "first eligible peer selected");
    Require(attempt->height == 7 && attempt->original_record == "legacy",
            "attempt retains exact evidence");
    Require(queue.NotFound(attempt->peer, attempt->hash, start + 1s),
            "matching notfound handled");
    Require(queue.Need(Hash(1), 7, "legacy", start + 2s), "duplicate need accepted");
    Require(!queue.NextRequest(start + 30s, peers), "duplicate need cannot reset backoff");
    auto retry = queue.NextRequest(start + 31s, peers);
    Require(retry && retry->peer == "bridge-b", "retry rotates peer identity");
    Require(retry->sequence != attempt->sequence, "each attempt has distinct sequence");
}

void TestResponseIdentityDuplicatesAndCompletion() {
    Queue queue;
    Require(!queue.Contains(Hash(1)), "empty queue does not intercept a normal block");
    queue.Need(Hash(1), 7, "legacy", start);
    Require(queue.Contains(Hash(1)) && !queue.Contains(Hash(2)),
            "contains routes only the pending repair hash");
    auto attempt = queue.NextRequest(start, peers);
    Require(attempt.has_value(), "request available");
    const auto proof = Proof();
    Require(!queue.QueueResponse("wrong-peer", Hash(1), 7, proof, root, start + 1s),
            "wrong peer rejected");
    Require(!queue.QueueResponse(attempt->peer, Hash(2), 7, proof, root, start + 1s),
            "wrong hash rejected");
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 8, proof, root, start + 1s),
            "wrong height rejected");
    Require(queue.Contains(Hash(1)), "unmatched responses remain in the repair lane");
    Require(!queue.Finish(*attempt, true, start + 1s), "unsolicited success cannot erase request");
    Require(queue.QueueResponse(attempt->peer, Hash(1), 7, proof, root, start + 1s),
            "matching response queued");
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, proof, root, start + 2s),
            "duplicate response rejected");
    Require(!queue.NotFound(attempt->peer, Hash(1), start + 2s),
            "late notfound cannot erase queued response");
    Require(!queue.NextRequest(start + 90s, peers), "queued response does not timeout");
    Require(queue.Contains(Hash(1)), "queued proof retains repair routing");
    auto ready = queue.TakeResponse();
    Require(ready && ready->attempt.sequence == attempt->sequence,
            "ready preserves request generation");
    Require(ready->root_after == root && ready->proof_data.spent_outputs.size() == 1,
            "ready owns complete bounded proof");
    Require(!queue.TakeResponse(), "response taken exactly once");
    Require(queue.Contains(Hash(1)), "in-progress validation retains repair routing");
    Require(!queue.NextRequest(start + 120s, peers), "validation in progress does not timeout");
    Require(queue.Finish(ready->attempt, true, start + 120s), "validated result retires need");
    Require(!queue.HasPending() && queue.Size() == 0, "success clears queue");
    Require(!queue.Contains(Hash(1)), "success releases repair routing");
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, proof, root, start + 121s),
            "late completed response rejected");
    Require(!queue.Finish(*attempt, true, start + 121s), "duplicate completion harmless");
}

void TestTimeoutRotationAndStaleFinish() {
    Queue queue;
    queue.Need(Hash(1), 7, "legacy", start);
    auto first = queue.NextRequest(start, peers);
    Require(first.has_value(), "first request available");
    Require(!queue.NextRequest(start + 29s, peers), "no request before timeout");
    Require(!queue.QueueResponse(first->peer, Hash(1), 7, Proof(), root, start + 30s),
            "response at timeout boundary is stale");
    // Reordered peer snapshots must still rotate by last identity.
    auto second = queue.NextRequest(start + 30s, {"bridge-c", "bridge-a", "bridge-b"});
    Require(second && second->peer == "bridge-b", "timeout rotates by identity");
    Require(!queue.Finish(*first, false, start + 31s), "stale finish cannot delay new attempt");
    Require(!queue.NotFound(first->peer, Hash(1), start + 31s),
            "late negative response cannot clear newer peer");
    Require(!queue.QueueResponse(first->peer, Hash(1), 7, Proof(), root, start + 31s),
            "late old peer proof rejected");
    Require(queue.QueueResponse(second->peer, Hash(1), 7, Proof(), root, start + 31s),
            "new peer proof accepted");
    auto ready = queue.TakeResponse();
    Require(ready && queue.Finish(ready->attempt, false, start + 32s),
            "failed validation schedules retry");
    Require(!queue.NextRequest(start + 61s, peers), "validation failure backs off");
    Require(queue.NextRequest(start + 62s, peers).has_value(), "retry resumes after backoff");
}

void TestTimeoutWithoutIncomingResponse() {
    Queue queue;
    queue.Need(Hash(1), 7, "legacy", start);
    auto first = queue.NextRequest(start, peers);
    auto second = queue.NextRequest(start + 30s, peers);
    Require(first && second && second->peer == "bridge-b", "timer-only retry rotates peer");
    Require(queue.Finish(*second, false, start + 31s), "synchronous send failure is handled");
    Require(!queue.NextRequest(start + 60s, peers), "send failure respects backoff");
    Require(queue.NextRequest(start + 61s, peers).has_value(), "send failure eventually retries");
}

void TestSupersededRecordsAndCancellation() {
    Queue queue;
    queue.Need(Hash(1), 7, "old", start);
    auto old = queue.NextRequest(start, peers);
    Require(old.has_value(), "old request available");
    Require(queue.QueueResponse(old->peer, Hash(1), 7, Proof(), root, start + 1s),
            "old response queued");
    auto taken = queue.TakeResponse();
    Require(taken.has_value(), "old response taken");
    Require(queue.Need(Hash(1), 7, "new", start + 2s), "new evidence supersedes old");
    Require(queue.Size() == 1, "replacement does not add an entry");
    auto fresh = queue.NextRequest(start + 2s, peers);
    Require(fresh && fresh->original_record == "new" && fresh->sequence != old->sequence,
            "replacement has fresh evidence and sequence");
    Require(!queue.Finish(taken->attempt, true, start + 3s), "old worker cannot retire replacement");
    Require(queue.Cancel(Hash(1)), "cancel removes pending evidence");
    Require(!queue.Contains(Hash(1)), "cancel releases repair routing");
    Require(!queue.Cancel(Hash(1)), "repeat cancel harmless");
    Require(!queue.Finish(*fresh, false, start + 4s), "cancel invalidates in-flight completion");
    Require(!queue.HasPending(), "cancel clears all state");
    queue.Need(Hash(1), 7, "new", start + 5s);
    auto after_cancel = queue.NextRequest(start + 5s, peers);
    Require(after_cancel && after_cancel->sequence != fresh->sequence,
            "cancel and re-add cannot reuse sequence");
}

void TestRecordAndProofBounds() {
    Queue queue;
    const std::string maximum(Queue::kMaxOriginalRecordBytes, 'x');
    Require(queue.Need(Hash(1), 7, maximum, start), "maximum record accepted");
    Require(!queue.Need(Hash(1), 7, maximum + 'x', start), "oversized replacement rejected");
    auto attempt = queue.NextRequest(start, peers);
    Require(attempt && attempt->original_record.size() == maximum.size(),
            "oversized replacement preserves original");
    for (uint8_t i = 2; i <= Queue::kMaxRecords; ++i) {
        Require(queue.Need(Hash(i), 7 + i, "legacy", start), "entry within cap accepted");
    }
    Require(!queue.Need(Hash(5), 12, "legacy", start), "record count cap enforced");
    Require(queue.Size() == Queue::kMaxRecords, "bounded pending count");
    auto oversized = Proof();
    oversized.spent_outputs.front().scriptPubKey.resize(Queue::kMaxProofBytes);
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, oversized, root, start + 1s),
            "oversized proof rejected before queue copy");
    oversized = Proof();
    oversized.spent_outputs.front().commitment.resize(Queue::kMaxProofBytes);
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, oversized, root, start + 1s),
            "oversized commitment rejected");
    auto malformed = Proof();
    malformed.spend_proof.targets.front().resize(31);
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, malformed, root, start + 1s),
            "malformed hash length rejected");
    malformed = Proof();
    malformed.spend_proof.positions.clear();
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, malformed, root, start + 1s),
            "target-position mismatch rejected");
    malformed = Proof();
    malformed.spent_outputs.resize(Queue::kMaxProofBytes / sizeof(dinero::consensus::SpentOutputData) + 1);
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, malformed, root, start + 1s),
            "decoded object overhead also bounded");
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, Proof(), UtreexoHash(31), start + 1s),
            "malformed root rejected");
    Require(queue.QueueResponse(attempt->peer, Hash(1), 7, Proof(), root, start + 1s),
            "invalid inputs do not consume a valid response slot");
}

BlockUtreexoData ManyTinyOutputs(uint8_t version) {
    auto proof = Proof();
    proof.spend_proof.format_version = version;
    proof.spend_proof.numLeaves = 8192;
    proof.spend_proof.targets.resize(8192, UtreexoHash(32, 2));
    proof.spend_proof.positions.resize(8192);
    for (size_t i = 0; i < 8192; ++i) proof.spend_proof.positions[i] = i;
    proof.spend_proof.proof_hashes.clear();
    proof.spent_outputs.resize(8192, proof.spent_outputs.front());
    return proof;
}

void TestSupportedWireProofMemoryBound() {
    constexpr size_t wire_limit = 1024 * 1024; // Existing utxoblk proof frame limit.
    Require(Queue::kMaxProofWireBytes == wire_limit, "queue preserves existing proof wire limit");
    for (uint8_t version : {4, 5, 6}) {
        const auto proof = ManyTinyOutputs(version);
        const auto serialized = proof.serialize();
        Require(proof.spend_proof.targets.size() <= dinero::consensus::MAX_PROOF_TARGETS,
                "many tiny outputs remain within the normal proof target count limit");
        const size_t spent_wire_bytes = 13 + (version >= 5 ? 5 : 0) + (version >= 6 ? 5 : 0);
        Require(serialized.size() == 53 + 8192 * (40 + spent_wire_bytes),
                "manual v4/v5/v6 wire size agrees with the actual serializer");
        const size_t decoded_elements = proof.spent_outputs.size() *
            (sizeof(dinero::consensus::SpentOutputData) + 1) +
            proof.spend_proof.targets.size() * (sizeof(UtreexoHash) + 32 + sizeof(uint64_t));
        Require(serialized.size() < wire_limit && decoded_elements > wire_limit,
                "supported wire proof exceeds the former decoded-memory cap");
        std::cout << "wire v" << unsigned(version) << ": " << serialized.size()
                  << " bytes; decoded elements: " << decoded_elements << " bytes\n";
        Queue queue;
        queue.Need(Hash(1), 7, "legacy", start);
        auto attempt = queue.NextRequest(start, peers);
        Require(queue.QueueResponse(attempt->peer, Hash(1), 7, proof, root, start + 1s),
                "admissible wire proof with many tiny outputs must remain recoverable");
        auto ready = queue.TakeResponse();
        Require(ready && ready->proof_data.serialize() == serialized,
                "queued large proof preserves actual serializer bytes");
        Require(queue.Finish(ready->attempt, true, start + 2s), "large proof completes");
    }
}

void TestProofWireBoundaries() {
    constexpr size_t wire_limit = 1024 * 1024;
    for (uint8_t version : {4, 5, 6}) {
        auto proof = ManyTinyOutputs(version);
        if (version >= 5) {
            proof.spent_outputs.front().is_confidential = true;
            proof.spent_outputs.front().commitment.assign(33, 0x42);
        }
        size_t remaining = wire_limit - proof.serialize().size();
        for (auto& spent : proof.spent_outputs) {
            const size_t added = std::min(remaining, size_t{9999});
            spent.scriptPubKey.resize(1 + added, 0x51);
            remaining -= added;
            if (remaining == 0) break;
        }
        Require(remaining == 0 && proof.serialize().size() == wire_limit,
                "real v4/v5/v6 serializer reaches exact wire boundary");
        Queue queue;
        queue.Need(Hash(1), 7, "legacy", start);
        auto attempt = queue.NextRequest(start, peers);
        proof.spent_outputs.back().scriptPubKey.push_back(0x51);
        Require(proof.serialize().size() == wire_limit + 1,
                "one added script byte crosses actual wire boundary");
        Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, proof, root, start + 1s),
                "wire-oversized proof rejected despite fitting decoded-memory cap");
        proof.spent_outputs.back().scriptPubKey.pop_back();
        Require(queue.QueueResponse(attempt->peer, Hash(1), 7, proof, root, start + 1s),
                "exact wire-limit proof remains admissible");
    }

    Queue queue;
    queue.Need(Hash(1), 7, "legacy", start);
    auto attempt = queue.NextRequest(start, peers);
    for (uint8_t version : {0, 1, 2, 3, 7, 255}) {
        auto proof = Proof();
        proof.spend_proof.format_version = version;
        Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, proof, root, start + 1s),
                "unsupported proof version rejected before storage");
    }
    auto malformed = Proof();
    malformed.accumulator_root_before.resize(31);
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, malformed, root, start + 1s),
            "malformed before-root length rejected");
    malformed = Proof();
    malformed.spend_proof.proof_hashes.front().resize(31);
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, malformed, root, start + 1s),
            "malformed proof-hash length rejected");
    // v4 does not encode commitments, but defensive accounting must still
    // reject an oversized decoded object supplied through the queue API.
    auto decoded_oversized = Proof();
    decoded_oversized.spend_proof.format_version = 4;
    decoded_oversized.spent_outputs.front().commitment.resize(Queue::kMaxProofBytes);
    Require(decoded_oversized.serialize().size() < wire_limit,
            "v4 commitment bytes do not contribute to serialized size");
    Require(!queue.QueueResponse(attempt->peer, Hash(1), 7, decoded_oversized, root, start + 1s),
            "decoded-memory cap remains enforced independently of wire size");
}

void TestFairnessAndRestartReconstruction() {
    Queue queue;
    for (uint8_t i = 1; i <= Queue::kMaxRecords; ++i) queue.Need(Hash(i), i, "legacy", start);
    std::vector<Queue::Attempt> attempts;
    for (size_t i = 0; i < Queue::kMaxRecords; ++i) {
        auto attempt = queue.NextRequest(start, {"", "bridge-a", "bridge-a", "bridge-b"});
        Require(attempt && !attempt->peer.empty(), "each pending need dispatched once");
        for (const auto& previous : attempts) Require(previous.hash != attempt->hash, "no duplicate in-flight hash");
        attempts.push_back(*attempt);
    }
    Require(!queue.NextRequest(start, peers), "all entries are in flight");
    Queue restarted;
    Require(!restarted.HasPending(), "pending queue is deliberately ephemeral");
    for (const auto& attempt : attempts) restarted.Need(attempt.hash, attempt.height, attempt.original_record, start);
    Require(restarted.Size() == Queue::kMaxRecords && restarted.NextRequest(start, peers).has_value(),
            "caller can rebuild needs from durable legacy records after restart");
}

void TestNeedsSnapshotOwnership() {
    Queue queue;
    Require(queue.Needs().empty(), "empty queue has no need snapshots");
    queue.Need(Hash(1), 7, "old-record", start);
    queue.Need(Hash(2), 8, "second-record", start);
    auto waiting = queue.Needs();
    Require(waiting.size() == 2 && waiting[0].peer.empty() && waiting[0].sequence == 0,
            "snapshot includes undispatched needs");
    auto attempt = queue.NextRequest(start, peers);
    Require(attempt.has_value(), "snapshot test request dispatched");
    auto dispatched = queue.Needs();
    Require(dispatched[0].sequence == attempt->sequence && dispatched[0].peer == attempt->peer,
            "snapshot preserves outstanding request identity");
    waiting[0].original_record = "caller-owned-change";
    Require(queue.Needs()[0].original_record == "old-record", "snapshot mutation cannot change queue evidence");
    queue.Need(Hash(1), 9, "replacement", start + 1s);
    queue.Cancel(Hash(2));
    Require(dispatched.size() == 2 && dispatched[0].height == 7 &&
                dispatched[0].original_record == "old-record" && dispatched[1].original_record == "second-record",
            "owned snapshot survives replacement and cancellation");
    const auto current = queue.Needs();
    Require(current.size() == 1 && current[0].height == 9 && current[0].original_record == "replacement",
            "fresh snapshot reflects current needs");
}

template <typename Action>
void Concurrently(Action action) {
    std::atomic<size_t> arrived{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            arrived.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            action();
        });
    }
    while (arrived.load() != threads.size()) std::this_thread::yield();
    go.store(true);
    for (auto& thread : threads) thread.join();
}

void TestConcurrentDuplicateDelivery() {
    Queue queue;
    Concurrently([&] { queue.Need(Hash(1), 7, "legacy", start); });
    Require(queue.Size() == 1, "concurrent duplicate needs coalesce");
    Concurrently([&] {
        Require(queue.Contains(Hash(1)) && !queue.Contains(Hash(2)),
                "concurrent contains calls preserve exact routing");
    });
    std::atomic<size_t> dispatched{0};
    Queue::Attempt attempt;
    std::mutex attempt_mutex;
    Concurrently([&] {
        if (auto next = queue.NextRequest(start, peers)) {
            std::lock_guard<std::mutex> lock(attempt_mutex);
            attempt = *next;
            dispatched.fetch_add(1);
        }
    });
    Require(dispatched.load() == 1, "one caller owns the in-flight attempt");
    const auto proof = Proof();
    std::atomic<size_t> queued{0};
    Concurrently([&] {
        if (queue.QueueResponse(attempt.peer, Hash(1), 7, proof, root, start + 1s)) queued.fetch_add(1);
    });
    Require(queued.load() == 1, "one concurrent response accepted");
    std::atomic<size_t> taken{0};
    Concurrently([&] {
        if (auto ready = queue.TakeResponse()) {
            taken.fetch_add(1);
            Require(queue.Finish(ready->attempt, true, start + 2s), "worker can finish without queue reentrancy");
        }
    });
    Require(taken.load() == 1 && !queue.HasPending(), "one worker completes the response");
}
} // namespace

int main() {
    TestNoPeerAndDuplicateNeedPreserveBackoff();
    TestResponseIdentityDuplicatesAndCompletion();
    TestTimeoutRotationAndStaleFinish();
    TestTimeoutWithoutIncomingResponse();
    TestSupersededRecordsAndCancellation();
    TestSupportedWireProofMemoryBound();
    TestProofWireBoundaries();
    TestRecordAndProofBounds();
    TestFairnessAndRestartReconstruction();
    TestNeedsSnapshotOwnership();
    TestConcurrentDuplicateDelivery();
    std::cout << "PASS: 11 replay metadata recovery queue scenarios\n";
}
