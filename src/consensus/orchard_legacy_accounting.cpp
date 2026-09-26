#include "consensus/orchard_legacy_accounting.h"
#include "consensus/orchard_profile.h"
#include "consensus/orchard_state_transition.h"
#include "consensus/merkle_root.h"
#include "storage/archival_block_reader.h"
#include <set>

namespace dinero::consensus {
namespace {
[[noreturn]] void Corrupt() { throw OrchardStateLookupError(Status::Corruption); }
[[noreturn]] void Unavailable() { throw OrchardStateLookupError(Status::Invalid); }
template<class T> T Read(StatusOr<T> value) {
    if (!value.ok()) throw OrchardStateLookupError(value.status());
    return std::move(value.value());
}
uint64_t Add(uint64_t a, uint64_t b) {
    if (a > orchard::kMaxMoneyUna || b > orchard::kMaxMoneyUna - a) Corrupt();
    return a + b;
}
uint64_t Amount(const TxOutput& output) {
    // Unknown is not zero. This adapter does not change CT consensus rules.
    if (output.is_confidential || !output.commitment.empty()) Unavailable();
    if (output.value.GetUna() > orchard::kMaxMoneyUna) Corrupt();
    return output.value.GetUna();
}
class SelectedBodies {
public:
    SelectedBodies(const ChainDB& db, const BlockStorage* blocks, uint32_t tip)
        : db_(db), blocks_(blocks), tip_(tip) {}
    Block Body(const uint256& hash, uint32_t height) const {
        if (height > tip_ || Read(db_.getBlockHashByHeight(int(height))) != hash ||
            Read(db_.getBlockHeight(hash)) != int(height)) Corrupt();
        const auto header = Read(db_.getHeader(hash));
        if (header.GetHash() != hash || (height && header.prev_block_hash !=
            Read(db_.getBlockHashByHeight(int(height - 1))))) Corrupt();
        auto block = Read(storage::ReadArchivalBlock(db_, blocks_, hash));
        bool mutated = false;
        if (block.vtx.empty() || block.header.SerializeForHash() != header.SerializeForHash() ||
            ComputeMerkleRoot(block.vtx, &mutated) != header.merkle_root || mutated ||
            !block.vtx.front().IsCoinbase()) Corrupt();
        // Only txid-authenticated monetary fields are consumed. Witness-only
        // data is neither used to establish an amount nor revalidated here.
        return block;
    }
    uint64_t Previous(const OutPoint& point, const Block& current,
        uint32_t height, uint32_t index) const {
        const auto location = Read(db_.getTxLocation(point.txid.AsUint256()));
        const auto source_height = Read(db_.getBlockHeight(location.first));
        if (source_height < 0 || uint32_t(source_height) > height ||
            (uint32_t(source_height) == height &&
             (location.first != current.GetHash() || location.second >= index))) Corrupt();
        const auto earlier = uint32_t(source_height) == height ? std::optional<Block>{} :
            std::optional<Block>{Body(location.first, uint32_t(source_height))};
        const auto& body = earlier ? *earlier : current;
        if (location.second >= body.vtx.size()) Corrupt();
        const auto& tx = body.vtx[location.second];
        if (tx.GetTxid() != point.txid || point.vout >= tx.vout.size() ||
            tx.IsCoinbase() != (location.second == 0)) Corrupt();
        return Amount(tx.vout[point.vout]);
    }
private:
    const ChainDB& db_;
    const BlockStorage* blocks_;
    uint32_t tip_;
};
}
SelectedLegacyPoolAccounting DeriveSelectedLegacyPoolAccountingUnderLock(
    const ChainDB& db, const BlockStorage* blocks, uint32_t maximum_epoch_blocks) {
    const auto& params = Params();
    const auto activation = params.orchard_activation_height;
    if (!OrchardProfileConfigurationValid(params) || activation == UINT32_MAX ||
        !activation || params.shielded_activation_height >= activation) Unavailable();
    uint256 genesis;
    if (!uint256::FromHex(params.genesis_hash, genesis) || genesis.IsNull()) Unavailable();
    const auto current = Read(db.getTip()), validated = Read(db.getValidatedTip());
    if (current.height != int(activation - 1) || validated.height != current.height ||
        validated.hash != current.hash || Read(db.getBlockHashByHeight(0)) != genesis ||
        Read(db.getBlockHashByHeight(current.height)) != current.hash) Corrupt();
    uint32_t epoch = params.shielded_activation_height;
    bool reset = false;
    for (const uint32_t boundary : {params.shielded_epoch_reset_height,
                                   params.shielded_spend_auth_epoch_reset_height}) {
        if (boundary < activation && boundary >= epoch) { epoch = boundary; reset = true; }
    }
    const uint64_t count = uint64_t(activation) - epoch;
    if (!maximum_epoch_blocks || count > maximum_epoch_blocks) Unavailable();
    const SelectedBodies source(db, blocks, activation - 1);
    SelectedLegacyPoolAccounting result{epoch, activation - 1, current.hash, 0, 0, 0};
    auto previous = epoch ? Read(db.getBlockHashByHeight(int(epoch - 1))) : uint256{};
    for (uint32_t height = epoch; height < activation; ++height) {
        const auto hash = Read(db.getBlockHashByHeight(int(height)));
        const auto body = source.Body(hash, height);
        if (body.header.prev_block_hash != previous) Corrupt();
        previous = hash;
        for (uint32_t index = 0; index < body.vtx.size(); ++index) {
            const auto& tx = body.vtx[index];
            if (tx.IsCoinbase() != (index == 0)) Corrupt();
            // Select the envelope family, not whether its bundle looks empty.
            if (!Transaction::IsShieldedVersion(tx.version)) continue;
            if (index == 0 || (reset && height == epoch)) Corrupt();
            if (!tx.ShieldedBundleCommitsToTxid() || !tx.HasExplicitFee()) Unavailable();
            uint64_t inputs = 0, outputs_and_fee = Add(0, tx.GetExplicitFee());
            std::set<OutPoint> points;
            for (const auto& input : tx.vin) {
                const OutPoint point(input.prevout.txid, input.prevout.vout);
                if (!points.insert(point).second) Corrupt();
                inputs = Add(inputs, source.Previous(point, body, height, index));
            }
            for (const auto& output : tx.vout) outputs_and_fee = Add(outputs_and_fee, Amount(output));
            if (inputs >= outputs_and_fee) {
                result.value_una = Add(result.value_una, inputs - outputs_and_fee);
            } else {
                const auto withdrawal = outputs_and_fee - inputs;
                if (withdrawal > result.value_una) Corrupt();
                result.value_una -= withdrawal;
            }
            ++result.shielded_transactions;
        }
        ++result.blocks_read;
    }
    if (previous != current.hash || Read(db.getTip()).hash != current.hash ||
        Read(db.getValidatedTip()).hash != current.hash) Corrupt();
    return result;
}
} // namespace dinero::consensus
