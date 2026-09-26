#include "orchard_test_fixture.h"
#include "wallet/orchard_proof_jobs.h"
#include <chrono>
#include <iostream>
#include <thread>
using dinero::wallet::OrchardOperationQueue;
using dinero::wallet::OrchardProofJobs;
using State = OrchardProofJobs::State;
using namespace std::chrono_literals;
template<class F> void JobReject(F run) {
    bool failed = false; try { run(); } catch (const std::exception&) { failed = true; } Require(failed);
}
State Terminal(OrchardProofJobs& jobs, const Hash& id) {
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    for (;;) {
        const auto state = jobs.Query(id); Require(bool(state));
        if (*state == State::Succeeded || *state == State::Failed || *state == State::Cancelled) return *state;
        Require(std::chrono::steady_clock::now() < deadline);
        (void)jobs.WaitForChange(id, *state, 1s);
    }
}
int main(int argc, char** argv) { try {
    Require(argc == 2); Fixture f(argv[1]);
    f.outputs[0].script_pub_key = f.view.coins.at(Point(f.inputs[0])).scriptPubKey;
    f.outputs[1].script_pub_key = f.view.coins.at(Point(f.inputs[1])).scriptPubKey;
    f.outputs[1].amount_una = 51000;
    auto keys = WalletKeys::FromSeed(std::array<uint8_t,64>{7}, 0);
    auto recipient = WalletKeys::FromSeed(std::array<uint8_t,64>{9}, 0);
    std::vector<WalletPayment> payments{{5000, recipient.Receiver(WalletScope::External, {})}};
    std::vector<ResolvedInput> resolved;
    for (const auto& input : f.inputs) {
        const auto& coin = f.view.coins.at(Point(input));
        resolved.push_back({input.txid_wire, input.output_index, input.sequence, coin.value.GetUna(), coin.scriptPubKey});
    }
    const auto context = SigningContext::Create(f.domain, f.lock, resolved, f.outputs, f.fee);
    auto durable = OrchardOperationQueue::Empty(f.domain);
    auto prepare = [&](uint8_t n) {
        auto inputs = resolved; inputs[0].txid_wire[0] ^= n; inputs[1].txid_wire[0] ^= n;
        return SigningContext::Create(f.domain, f.lock, inputs, f.outputs, f.fee);
    };
    OrchardProofJobs jobs;
    Require(!jobs.Query(Hash{1}));
    JobReject([&] { (void)jobs.TakeResult(Hash{1}); });
    auto unreserved = WalletBundlePlan::PrepareShield(keys, payments);
    JobReject([&] { jobs.Submit(Hash{1}, durable, std::move(unreserved), context); });
    auto primary = WalletBundlePlan::PrepareShield(keys, payments);
    const auto primary_intent = primary.Intent(context);
    durable = durable.Reserve(Hash{1}, primary_intent);
    auto mismatched = WalletBundlePlan::PrepareShield(keys, payments);
    JobReject([&] { jobs.Submit(Hash{1}, durable, std::move(mismatched), context); });
    jobs.Submit(Hash{1}, durable, std::move(primary), context);
    Require(jobs.Query(Hash{1}) == State::Queued);
    JobReject([&] { jobs.Forget(Hash{1}); });
    for (uint8_t n = 2; n <= 4; ++n) {
        auto plan = WalletBundlePlan::PrepareShield(keys, payments); const auto c = prepare(n);
        durable = durable.Reserve(Hash{n}, plan.Intent(c)); jobs.Submit(Hash{n}, durable, std::move(plan), c);
    }
    auto overflow = WalletBundlePlan::PrepareShield(keys, payments); const auto c5 = prepare(5);
    durable = durable.Reserve(Hash{5}, overflow.Intent(c5));
    JobReject([&] { jobs.Submit(Hash{5}, durable, std::move(overflow), c5); });
    Require(!jobs.Query(Hash{5}));
    Require(jobs.Cancel(Hash{2}) && jobs.Query(Hash{2}) == State::Cancelled);
    Require(jobs.Cancel(Hash{2}));
    Require(durable.Entries().at(Hash{2}).phase == OrchardOperationQueue::Phase::Reserved);
    JobReject([&] { (void)jobs.TakeResult(Hash{2}); });
    // A cancelled entry still occupies a bounded result/status slot until the
    // caller explicitly collects it. Queued cancellation never runs the prover.
    auto overflow2 = WalletBundlePlan::PrepareShield(keys, payments);
    durable = durable.CancelReserved(Hash{5}).Reserve(Hash{5}, overflow2.Intent(c5));
    JobReject([&] { jobs.Submit(Hash{5}, durable, std::move(overflow2), c5); });
    jobs.Forget(Hash{2}); Require(!jobs.Query(Hash{2}));
    Require(jobs.Cancel(Hash{3}) && jobs.Cancel(Hash{4}));
    jobs.Forget(Hash{3}); jobs.Forget(Hash{4});
    jobs.Start(); JobReject([&] { jobs.Start(); });
    Require(Terminal(jobs, Hash{1}) == State::Succeeded);
    Require(!jobs.Cancel(Hash{1})); JobReject([&] { jobs.Forget(Hash{1}); });
    auto proof = jobs.TakeResult(Hash{1}); Require(bool(proof) && !jobs.Query(Hash{1}));
    Require(proof->Authorization().SigningDigest() == primary_intent.Message());
    f.bundle = proof->Bytes(); f.Sign();
    const auto authorized = VerifyOrchardAuthorizations(f.Snapshot(), f.domain, f.view.height + 1, {});
    const auto ready = durable.SetReady(Hash{1}, authorized);
    Require(ready.Entries().at(Hash{1}).transaction == f.Build().CanonicalBytes());
    Require(durable.Entries().at(Hash{1}).phase == OrchardOperationQueue::Phase::Reserved);
    jobs.Shutdown(); jobs.Shutdown(); JobReject([&] { jobs.Start(); });
    auto stopped = WalletBundlePlan::PrepareShield(keys, payments);
    durable = durable.CancelReserved(Hash{5}).Reserve(Hash{5}, stopped.Intent(c5));
    JobReject([&] { jobs.Submit(Hash{5}, durable, std::move(stopped), c5); });

    // Real active cancellation plus shutdown. There is no fake proof executor
    // or callback capable of returning an unverified proof result.
    OrchardProofJobs stopping;
    for (uint8_t n = 6; n <= 7; ++n) {
        auto plan = WalletBundlePlan::PrepareShield(keys, payments); const auto c = prepare(n);
        durable = durable.Reserve(Hash{n}, plan.Intent(c)); stopping.Submit(Hash{n}, durable, std::move(plan), c);
    }
    stopping.Start();
    const auto started = stopping.WaitForChange(Hash{6}, State::Queued, 30s);
    Require(started == State::Running);
    Require(stopping.Query(Hash{7}) == State::Queued); // Exactly one worker.
    Require(stopping.Cancel(Hash{6}));
    Require(stopping.Query(Hash{6}) == State::CancelRequested || stopping.Query(Hash{6}) == State::Cancelled);
    JobReject([&] { (void)stopping.TakeResult(Hash{6}); });
    stopping.RequestStop();
    Require(stopping.Query(Hash{7}) == State::Cancelled);
    std::thread first([&] { stopping.Shutdown(); });
    std::thread second([&] { stopping.Shutdown(); }); first.join(); second.join();
    Require(stopping.Query(Hash{6}) == State::Cancelled);
    stopping.Forget(Hash{6}); stopping.Forget(Hash{7});
    Require(durable.Entries().at(Hash{6}).phase == OrchardOperationQueue::Phase::Reserved);
    JobReject([&] { (void)stopping.WaitForChange(Hash{6}, State::Queued, 31s); });
    std::cout << "PASS: bounded proof jobs, reservation binding, real authorization, queued/active cancellation and concurrent shutdown\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
