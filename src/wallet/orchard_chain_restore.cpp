#include "wallet/orchard_chain_restore.h"
#include "consensus/orchard_block_staging.h"
#include "consensus/merkle_root.h"
#include "storage/archival_block_reader.h"
#include <algorithm>
#include <map>

namespace dinero::wallet {
namespace {
using namespace consensus;
using namespace orchard;
[[noreturn]] void Corrupt() { throw OrchardStateLookupError(Status::Corruption); }
template<class T> T Read(StatusOr<T> value) {
    if (!value.ok()) throw OrchardStateLookupError(value.status());
    return std::move(value.value());
}
uint256 Hash256(const Hash& value) {
    uint256 out; std::copy(value.begin(), value.end(), out.begin()); return out;
}
class HistoricalInputs final : public ChainStateView {
public:
    explicit HistoricalInputs(uint32_t height) : height_(height) {}
    std::map<OutPoint, UTXOEntry> coins;
    StatusOr<UTXOEntry> getCoin(const OutPoint& point) const override {
        const auto it = coins.find(point);
        return it == coins.end() ? StatusOr<UTXOEntry>(Status::NotFound) : StatusOr<UTXOEntry>(it->second);
    }
    bool hasCoin(const OutPoint& point) const override { return coins.contains(point); }
    uint32_t getHeight() const override { return height_; }
private:
    uint32_t height_;
};
class SelectedArchive {
public:
    SelectedArchive(const ChainDB& db, const BlockStorage* blocks, uint32_t activation,
        uint32_t tip) : db_(db), blocks_(blocks), activation_(activation), tip_(tip) {}
    uint32_t Height(const uint256& hash) const {
        const auto metadata = Read(db_.getHeaderMetadata(hash));
        if (metadata.height < 0 || uint32_t(metadata.height) > tip_ ||
            Read(db_.getBlockHashByHeight(metadata.height)) != hash) Corrupt();
        const auto header = Read(db_.getHeader(hash));
        if (header.GetHash() != hash || header.prev_block_hash != metadata.parent_hash) Corrupt();
        if (metadata.height > 0 &&
            Read(db_.getBlockHashByHeight(metadata.height - 1)) != header.prev_block_hash) Corrupt();
        return uint32_t(metadata.height);
    }
    OrchardBlockCandidate OrchardBody(const uint256& hash, uint32_t height) const {
        if (height < activation_ || Height(hash) != height) Corrupt();
        auto body = ReadStoredOrchardBlock(db_, hash, true);
        if (body.Header().SerializeForHash() != Read(db_.getHeader(hash)).SerializeForHash()) Corrupt();
        return body;
    }
    UTXOEntry Previous(const OutPoint& point, uint32_t origin_height, uint32_t origin_index) const {
        const auto location = Read(db_.getTxLocation(point.txid.AsUint256()));
        const auto height = Height(location.first);
        if (height > origin_height || (height == origin_height && location.second >= origin_index)) Corrupt();
        if (height >= activation_) {
            const auto body = OrchardBody(location.first, height);
            if (location.second >= body.Transactions().size()) Corrupt();
            const auto& tx = body.Transactions()[location.second];
            if (tx.GetTxid() != point.txid) Corrupt();
            if (tx.IsOrchard()) {
                if (point.vout >= tx.Orchard().Outputs().size()) Corrupt();
                const auto& output = tx.Orchard().Outputs()[point.vout];
                return {AmountUna::Una(output.amount_una), output.script_pub_key, height, false};
            }
            return Output(tx.Historical(), point.vout, height, location.second);
        }
        const auto body = Read(storage::ReadArchivalBlock(db_, blocks_, location.first));
        bool mutated = false;
        const auto root = ComputeMerkleRoot(body.vtx, &mutated);
        if (body.header.GetHash() != location.first || mutated || body.vtx.empty() ||
            root != body.header.merkle_root ||
            body.header.SerializeForHash() != Read(db_.getHeader(location.first)).SerializeForHash() ||
            location.second >= body.vtx.size() || body.vtx[location.second].GetTxid() != point.txid) Corrupt();
        // Only non-witness transparent output data is consumed from historical
        // bodies. Do not impose the new envelope's accepted language on replay.
        return Output(body.vtx[location.second], point.vout, height, location.second);
    }
    std::optional<uint64_t> Mtp(uint32_t height) const {
        if (height > tip_) Corrupt();
        std::vector<uint64_t> times;
        for (uint32_t h = height;; --h) {
            const auto hash = Read(db_.getBlockHashByHeight(static_cast<int>(h)));
            if (Height(hash) != h) Corrupt();
            times.push_back(Read(db_.getHeader(hash)).timestamp);
            if (h == 0 || times.size() == 11) break;
        }
        std::sort(times.begin(), times.end()); return times[times.size()/2];
    }
private:
    static UTXOEntry Output(const Transaction& tx, uint32_t vout, uint32_t height, uint32_t index) {
        if (vout >= tx.vout.size() || tx.IsCoinbase() != (index == 0)) Corrupt();
        const auto& output = tx.vout[vout];
        if (output.is_confidential || !output.commitment.empty() || output.value.GetUna() > kMaxMoneyUna) Corrupt();
        return {output.value, output.scriptPubKey, height, index == 0};
    }
    const ChainDB& db_;
    const BlockStorage* blocks_;
    uint32_t activation_, tip_;
};
}
OrchardAccountState RestoreOrchardAccountFromChainUnderLock(
    const ChainDB& db, const BlockStorage* blocks, const WalletStateBytes& payload,
    SigningDomain domain, const FullViewingKeyBytes& fvk, uint32_t activation) {
    const auto checkpoint = Read(db.getOrchardState());
    const auto tip = Read(db.getValidatedTip()), current = Read(db.getTip());
    if (activation == 0 || activation == UINT32_MAX || checkpoint.height < activation ||
        checkpoint.height > uint32_t(INT32_MAX) || tip.height != int(checkpoint.height) ||
        tip.hash != checkpoint.block_hash || current.hash != tip.hash || current.height != tip.height) Corrupt();
    const SelectedArchive archive(db, blocks, activation, checkpoint.height);
    if (archive.Height(tip.hash) != checkpoint.height) Corrupt();
    // At most one authorization is cached, for adjacent notes from one tx.
    // Cache lifetime is this single locked restore call, never across a reorg.
    std::shared_ptr<const VerifiedOrchardAuthorizations> cached;
    uint256 cached_block;
    OrchardWalletRestoreLookups lookups;
    lookups.origin = [&](uint32_t height, const uint256& block, const Hash& txid) {
        if (height < activation || height > checkpoint.height) Corrupt();
        const auto body = archive.OrchardBody(block, height);
        const auto location = Read(db.getTxLocation(Hash256(txid)));
        if (location.first != block || location.second >= body.Transactions().size()) Corrupt();
        const auto& parsed = body.Transactions()[location.second];
        if (!parsed.IsOrchard() || parsed.Orchard().Txid() != txid) Corrupt();
        if (cached && cached_block == block && cached->Orchard().Txid() == txid) return cached;
        HistoricalInputs inputs(height - 1);
        for (const auto& input : parsed.Orchard().Inputs()) {
            const OutPoint point(TxId(Hash256(input.txid_wire)), input.output_index);
            if (!inputs.coins.emplace(point, archive.Previous(point, height, location.second)).second) Corrupt();
        }
        try {
            const auto snapshot = OrchardCoinSnapshot::ResolveUnderChainstateLock(parsed.Orchard(), inputs);
            cached = std::make_shared<const VerifiedOrchardAuthorizations>(VerifyOrchardAuthorizations(
                snapshot, domain, height, [&](uint32_t h) { return archive.Mtp(h); }));
        } catch (const OrchardCoinLookupError& error) {
            throw OrchardStateLookupError(error.SourceStatus());
        } catch (const OrchardTransparentError&) {
            Corrupt(); // Revalidating local storage, not classifying a peer block.
        } catch (const BackendError& error) {
            throw OrchardStateLookupError(error.Status() == DINERO_ORCHARD_PANIC ?
                Status::Internal : Status::Corruption);
        }
        cached_block = block; return cached;
    };
    lookups.spent_nullifier = [&](const uint256& nullifier) -> StatusOr<bool> {
        const auto owner = db.getOrchardNullifierOwner(nullifier);
        if (owner.status() == Status::NotFound) return false;
        if (!owner.ok()) return owner.status();
        const auto height = archive.Height(*owner);
        const auto body = archive.OrchardBody(*owner, height);
        for (const auto& tx : body.Transactions()) if (tx.IsOrchard()) {
            const auto& facts = tx.Orchard().UnverifiedFacts();
            for (uint32_t i = 0; i < facts.action_count; ++i)
                if (std::equal(nullifier.begin(), nullifier.end(), std::begin(facts.nullifiers[i]))) return true;
        }
        Corrupt(); // A present owner row must name a selected body with that NF.
    };
    auto restored = OrchardAccountState::Restore(payload, domain, fvk, activation, checkpoint, lookups);
    if (Read(db.getOrchardState()) != checkpoint || Read(db.getValidatedTip()).hash != tip.hash) Corrupt();
    return restored;
}
} // namespace dinero::wallet
