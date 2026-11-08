// SepTxValidationBinds (Task 2.6). Two jobs:
//  1. Compile-time: the three tx_validation entry points must bind BOTH a
//     CoinsViewCache& and a const ChainStateView& (std::is_invocable_v).
//     Against the pre-2.6 header the ChainStateView assertions fail to
//     compile — that is this gate's RED.
//  2. Runtime: one coinbase-spending transaction validates through a plain
//     test ChainStateView (no CoinsDB, no RocksDB), proving the interface
//     is sufficient for real validation. Exit-nonzero checks — no assert().

#include "consensus/chain_state_view.h"
#include "consensus/coins_db.h"   // CoinsViewCache side of the dual-bind assertions
#include "consensus/tx_validation.h"
#include "consensus/utxo_entry.h"
#include "primitives/transaction.h"

#include <cstdio>
#include <map>
#include <string>
#include <type_traits>

namespace {

using dinero::consensus::ChainStateView;
using dinero::consensus::CoinsViewCache;
using dinero::consensus::TxValidationContext;

// The three entry points must accept both view types.
static_assert(std::is_invocable_v<decltype(&dinero::consensus::validateTransaction),
                                  const dinero::Transaction&, CoinsViewCache&,
                                  const TxValidationContext&, bool>,
              "validateTransaction must bind CoinsViewCache&");
static_assert(std::is_invocable_v<decltype(&dinero::consensus::validateTransaction),
                                  const dinero::Transaction&, const ChainStateView&,
                                  const TxValidationContext&, bool>,
              "validateTransaction must bind const ChainStateView&");
static_assert(std::is_invocable_v<decltype(&dinero::consensus::validateInputs),
                                  const dinero::Transaction&, CoinsViewCache&,
                                  const TxValidationContext&, dinero::AmountUna&>,
              "validateInputs must bind CoinsViewCache&");
static_assert(std::is_invocable_v<decltype(&dinero::consensus::validateInputs),
                                  const dinero::Transaction&, const ChainStateView&,
                                  const TxValidationContext&, dinero::AmountUna&>,
              "validateInputs must bind const ChainStateView&");
static_assert(std::is_invocable_v<decltype(&dinero::consensus::checkSequenceLocks),
                                  const dinero::Transaction&, CoinsViewCache&,
                                  const TxValidationContext&>,
              "checkSequenceLocks must bind CoinsViewCache&");
static_assert(std::is_invocable_v<decltype(&dinero::consensus::checkSequenceLocks),
                                  const dinero::Transaction&, const ChainStateView&,
                                  const TxValidationContext&>,
              "checkSequenceLocks must bind const ChainStateView&");

int g_failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

// Map-backed read-only view: exactly the ChainStateView surface.
class TestChainStateView final : public ChainStateView {
public:
    dinero::StatusOr<dinero::consensus::UTXOEntry> getCoin(
            const dinero::OutPoint& out) const override {
        auto it = coins_.find(out);
        if (it == coins_.end()) return dinero::Status::NotFound;
        return it->second;
    }
    bool hasCoin(const dinero::OutPoint& out) const override {
        return coins_.count(out) != 0;
    }
    uint32_t getHeight() const override { return height_; }

    std::map<dinero::OutPoint, dinero::consensus::UTXOEntry> coins_;
    uint32_t height_ = 200;
};

} // namespace

int main() {
    using namespace dinero;
    // One mature coinbase coin, spent by a simple transaction.
    Transaction coinbase;
    coinbase.version = 2;
    TxInput cb_in;
    cb_in.prevout.vout = UINT32_MAX;
    cb_in.scriptSig = {1};
    coinbase.vin.push_back(cb_in);
    TxOutput cb_out;
    cb_out.value = AmountUna::Una(50000);
    cb_out.scriptPubKey = {0x51};  // OP_TRUE-ish placeholder script
    coinbase.vout.push_back(cb_out);

    TestChainStateView view;
    OutPoint funded(coinbase.GetTxid(), 0);
    consensus::UTXOEntry coin;
    coin.value = cb_out.value;
    coin.scriptPubKey = cb_out.scriptPubKey;
    coin.height = 1;            // long mature at view height 200
    coin.isCoinbase = true;
    view.coins_[funded] = coin;

    Transaction spend;
    spend.version = 2;
    TxInput in;
    in.prevout = TxOutPoint(funded.txid, funded.vout);
    in.sequence = 0xffffffff;
    spend.vin.push_back(in);
    TxOutput out;
    out.value = AmountUna::Una(40000);
    out.scriptPubKey = {0x51};
    spend.vout.push_back(out);

    consensus::TxValidationContext ctx{};
    ctx.block_height = view.getHeight();
    ctx.median_time_past = 1700000000;
    ctx.check_sequence_locks = true;
    ctx.skip_script_verification = true;  // placeholder scripts; binding gate,
                                          // not a script-engine test

    AmountUna total_in = AmountUna::Una(0);
    const auto inputs = consensus::validateInputs(spend, view, ctx, total_in);
    Check(inputs == consensus::TxValidationResult::OK,
          "validateInputs accepts a funded spend through a plain ChainStateView");
    Check(total_in == AmountUna::Una(50000), "total_in reflects the funded coin");
    Check(consensus::checkSequenceLocks(spend, view, ctx),
          "final sequence passes BIP68 through the view");
    const auto result = consensus::validateTransaction(spend, view, ctx, false);
    Check(result.result == consensus::TxValidationResult::OK,
          "validateTransaction validates the coinbase spend through the view");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("SepTxValidationBinds: all checks passed");
    return 0;
}
