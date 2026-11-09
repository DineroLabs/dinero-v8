// Exercises the real pool and canonical Orchard decoder. The selected-chain
// validator is a component double: this does not certify an Orchard proof.
#include "daemon/mempool.h"
#include "consensus/chainparams.h"
#include "orchard_transaction.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
using namespace dinero;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct State { bool held = false; bool refuse = true; unsigned validations = 0; unsigned notices = 0; };
class Scan final : public IMempoolScanSink {
    struct Held final : Guard {
        State& state;
        explicit Held(State& value) : state(value) { state.held = true; }
        ~Held() override { state.held = false; }
    };
    State& state_;
public:
    explicit Scan(State& state) : state_(state) {}
    std::unique_ptr<Guard> AcquireEmptyRegistry() override {
        if (state_.refuse) return {};
        return std::make_unique<Held>(state_);
    }
    void ScanMempoolTransaction(const std::string&) override {
        throw std::runtime_error("Orchard body must not enter a historical scanner");
    }
};
class Selected final : public MempoolChainstateReadGuard {
    State& state_;
public:
    explicit Selected(State& state) : state_(state) {}
    MempoolOrchardValidation ValidateOrchard(const MempoolTransaction& body,
        std::span<const MempoolTransaction>) override {
        Require(state_.held, "registry guard must cover validation and publication");
        ++state_.validations;
        return {TxAcceptResult::Accepted(body.GetTxid().AsUint256()), 150000,
            body.ExplicitFee().value(), {}};
    }
};
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "supply canonical Orchard fixture");
        std::ifstream file(argv[1], std::ios::binary);
        Require(file.good(), "canonical fixture unavailable");
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        const auto envelope = dinero::orchard::TransactionEnvelope::DecodeExact(bytes);
        const auto body = dinero::MempoolTransaction::FromOrchard(envelope);
        dinero::SelectParams(dinero::Chain::MAINNET);
        State state;
        dinero::Mempool pool(nullptr, nullptr, false, std::make_unique<Scan>(state));
        pool.setMinFeeRate(0); // Fee policy is not the subject of this fixture.
        pool.setChainstateReadGuardFactory([&] { return std::make_unique<Selected>(state); });
        pool.setTxBodyAcceptedCallback([&](const auto&) {
            Require(!state.held, "registry guard must release before external notification");
            ++state.notices;
        });
        const auto refused = pool.submitBody(body, "scanner-contract", false);
        Require(refused.code == dinero::TxRejectCode::UNAVAILABLE &&
            refused.message == "Configured scanner cannot consume Orchard body",
            "scanner refusal contract changed");
        Require(pool.size() == 0 && state.validations == 0 && state.notices == 0 && !state.held,
            "scanner refusal must precede validation and effects");
        state.refuse = false;
        const auto admitted = pool.submitBody(body, "scanner-contract", false);
        Require(admitted.accepted(), admitted.message.c_str());
        Require(pool.size() == 1 && state.validations == 1 && state.notices == 1 && !state.held,
            "successful selected validation/publication/notification ordering");
        Require(pool.getMempoolEntry(body.GetTxid().AsUint256())->tx.Serialize() == bytes,
            "retain exact canonical Orchard body");
        std::cout << "Orchard scanner sink: refusal and retained-guard lifecycle passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
