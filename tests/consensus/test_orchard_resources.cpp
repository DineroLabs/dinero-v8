#include "consensus/chainparams.h"
#include "consensus/orchard_block_coins.h"
#include "consensus/orchard_resources.h"
#include "orchard_block_coin_test_fixture.h"
#include "orchard_wallet.h"
#include <source_location>
using E = OrchardResourceErrorCode;
template <class F>
void ResourceReject(
    E code, F call,
    std::source_location source = std::source_location::current()) {
  bool rejected = false;
  try {
    call();
  } catch (const OrchardResourceError &e) {
    if (e.Code() != code)
      std::cerr << "resource case line " << source.line() << " expected "
                << int(code) << " actual " << int(e.Code()) << '\n';
    Require(e.Code() == code);
    rejected = true;
  }
  Require(rejected);
}
int main(int argc, char **argv) {
  try {
    Require(argc == 2);
    SelectParams(Chain::REGTEST);
    Require(CountOrchardProfileScriptSigops({Bytes{0x52, 0xae}}, true, false) ==
            2);
    Require(CountOrchardProfileScriptSigops({Bytes{0x52, 0xae}}, false,
                                            false) == 20);
    Require(CountOrchardProfileScriptSigops({Bytes{0x51, 1, 0x53, 0xae}}, true,
                                            false) == 20);
    Require(CountOrchardProfileScriptSigops({Bytes{0xac, 0xad, 0xba}}, true,
                                            false) == 2);
    Require(CountOrchardProfileScriptSigops({Bytes{0xac, 0xad, 0xba}}, true,
                                            true) == 3);
    for (const auto &script :
         std::vector<Bytes>{{1, 0xac},
                            {0x4c, 1, 0xad},
                            {0x4d, 1, 0, 0xae},
                            {0x4e, 1, 0, 0, 0, 0xba},
                            {0x4c},
                            {0x4d, 1},
                            {0x4e, 1, 0},
                            {0x4e, 0xff, 0xff, 0xff, 0xff}})
      Require(CountOrchardProfileScriptSigops(script, true, true) == 0);
    Fixture f(argv[1]);
    f.Sign();
    auto parsed = ParsedTransaction::DecodeExact(
        f.Build().CanonicalBytes(), TransactionReadMode::StagedOrchard);
    OrchardResourceUsage usage;
    for (uint32_t i = 0; i < kOrchardMaxBlockBundles; ++i)
      AccumulateOrchardTransactionResources(parsed, usage);
    Require(usage.bundles == 8 &&
            usage.actions ==
                8 * parsed.Orchard().UnverifiedFacts().action_count);
    const auto previous = usage;
    ResourceReject(E::Bundles, [&] {
      AccumulateOrchardTransactionResources(parsed, usage);
    });
    Require(usage == previous);
    usage = {0, kOrchardMaxBlockActions, 0};
    ResourceReject(E::Actions, [&] {
      AccumulateOrchardTransactionResources(parsed, usage);
    });
    Require(usage == OrchardResourceUsage{0, kOrchardMaxBlockActions, 0});
    usage = {0, 0, kOrchardMaxBlockSigops};
    std::vector<UTXOEntry> coins;
    for (const auto &in : parsed.Orchard().Inputs())
      coins.push_back(f.view.coins.at(Point(in)));
    ResourceReject(E::Sigops, [&] {
      AccumulateOrchardInputResources(parsed, coins, usage);
    });
    Require(usage == OrchardResourceUsage{0, 0, kOrchardMaxBlockSigops});
    usage = {};
    AccumulateOrchardInputResources(parsed, coins, usage);
    Require(usage.sigops == 2);
    ResourceReject(E::InputShape,
                   [&] { AccumulateOrchardInputResources(parsed, {}, usage); });
    // Taproot script path counts the actual script, not a signature or annex.
    auto child = Child(Point(f.inputs[0]), coins[0], f);
    child.vin[0].witness = {Bytes(64, 0), Bytes{0xac, 0xba, 0xad},
                            Bytes(33, 0xc0), Bytes{0x50, 0xac}};
    auto scriptTx = ParsedTransaction::DecodeExact(
        Wire(child), TransactionReadMode::StagedOrchard);
    usage = {};
    AccumulateOrchardInputResources(scriptTx, std::span(coins).first(1), usage);
    Require(usage.sigops == 3);
    // Output script budget is exact and transactional, including the final tx.
    auto large = child;
    large.vout.clear();
    for (size_t i = 0; i < 8; ++i)
      large.vout.emplace_back(AmountUna::Una(1), Bytes(10000, 0xac));
    auto largeParsed = ParsedTransaction::DecodeExact(
        Wire(large), TransactionReadMode::StagedOrchard);
    usage = {};
    AccumulateOrchardTransactionResources(largeParsed, usage);
    Require(usage.sigops == kOrchardMaxBlockSigops);
    ResourceReject(E::Sigops, [&] {
      AccumulateOrchardTransactionResources(largeParsed, usage);
    });
    Require(usage.sigops == kOrchardMaxBlockSigops);
    // Body-wide work preflight must run before ANY parent coin lookup.
    class NoLookup final : public ChainStateView {
    public:
      StatusOr<UTXOEntry> getCoin(const OutPoint &) const override {
        throw std::runtime_error("resource preflight reached coins");
      }
      bool hasCoin(const OutPoint &) const override {
        throw std::runtime_error("resource preflight reached coins");
      }
      uint32_t getHeight() const override { return 20000; }
    } none;
    OrchardBlockContext context{20001, H(2), H(1), 20001, f.domain};
    const auto rejectBlock = [&](std::vector<Bytes> txs, E code) {
      auto block = CandidateWires(context, std::move(txs));
      auto c = context;
      c.block_hash = block.Header().GetHash();
      ResourceReject(code, [&] {
        (void)PrepareOrchardBlockCoinsUnderChainstateLock(block, c, none, {},
                                                          true);
      });
    };
    // Unique framed txids; no claim of authorization for resource-only
    // fixtures.
    std::vector<Bytes> many;
    for (uint32_t i = 0; i < 9; ++i)
      many.push_back(
          TransactionEnvelope::Create(i, f.inputs, f.outputs, f.fee, f.bundle)
              .CanonicalBytes());
    rejectBlock(many, E::Bundles);
    auto keys = WalletKeys::FromSeed(std::array<uint8_t, 64>{7}, 0);
    std::vector<WalletPayment> payments(
        8, WalletPayment{625, keys.Receiver(WalletScope::External, {})});
    auto plan = WalletBundlePlan::PrepareShield(keys, payments);
    std::vector<ResolvedInput> resolved;
    for (const auto &in : f.inputs) {
      const auto &coin = f.view.coins.at(Point(in));
      resolved.push_back({in.txid_wire, in.output_index, in.sequence,
                          coin.value.GetUna(), coin.scriptPubKey});
    }
    auto outputs = f.outputs;
    outputs[1].amount_una = 51000;
    auto bundle = std::move(plan).Prove(
        SigningContext::Create(f.domain, f.lock, resolved, outputs, f.fee));
    auto eight = TransactionEnvelope::Create(f.lock, f.inputs, outputs, f.fee,
                                             bundle.Bytes());
    Require(eight.UnverifiedFacts().action_count == 8);
    auto eightParsed = ParsedTransaction::DecodeExact(
        eight.CanonicalBytes(), TransactionReadMode::StagedOrchard);
    usage = {};
    for (size_t i = 0; i < 4; ++i)
      AccumulateOrchardTransactionResources(eightParsed, usage);
    Require(usage.bundles == 4 && usage.actions == 32);
    ResourceReject(E::Actions, [&] {
      AccumulateOrchardTransactionResources(eightParsed, usage);
    });
    many.clear();
    for (uint32_t i = 0; i < 5; ++i)
      many.push_back(TransactionEnvelope::Create(i, f.inputs, outputs, f.fee,
                                                 bundle.Bytes())
                         .CanonicalBytes());
    rejectBlock(many, E::Actions);
    auto excessive = large;
    excessive.vout.emplace_back(AmountUna::Una(1), Bytes{0xac});
    rejectBlock({Wire(excessive)}, E::Sigops);
    std::cout << "PASS: draft aggregate Orchard proof work, resolved input "
                 "sigops, push parsing, exact limits and preflight ordering\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
