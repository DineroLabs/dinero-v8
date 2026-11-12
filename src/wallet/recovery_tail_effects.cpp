#include "wallet/recovery_tail_effects.h"
#include "primitives/orchard_block_reader.h"
#include <stdexcept>
#include <string>

namespace dinero {
namespace {
// The stage-B recovery window admits at most 20000 blocks above the base; the
// event walk may additionally traverse reorg churn that nets away.
constexpr size_t kMaxTailBlocks = 20000;
constexpr uint64_t kMaxTailEvents = 65536;
constexpr size_t kMaxTailOutputs = size_t(1) << 21;
constexpr size_t kMaxTailSpends = size_t(1) << 21;
constexpr size_t kMaxTailScriptBytes = 10000;
constexpr size_t kMaxTailMaterial = size_t(256) * 1024 * 1024;

[[noreturn]] void FailTail(const char* what) {
    throw std::runtime_error(std::string("Recovery tail ") + what);
}
}  // namespace

RecoveryTailEffects ReadRecoveryTailEffects(const RuntimeAccountReplay& replay) {
    const auto facts = replay.Profile();
    const auto origin = replay.ActivationOrigin().checkpoint;
    if (!facts.activation_height || uint64_t(origin.height) + 1 != facts.activation_height)
        FailTail("requires the activation parent as base");
    RecoveryTailEffects result;
    result.base_height = origin.height;
    result.base_hash = origin.block_hash;
    size_t outputs = 0, spends = 0, material = 0;
    const auto top_hash = [&] { return result.blocks.empty() ? result.base_hash : result.blocks.back().hash; };
    const auto top_height = [&] { return result.blocks.empty() ? result.base_height : result.blocks.back().height; };
    const auto extract = [&](uint32_t height, const OrchardBlockCandidate& body) {
        if (result.blocks.size() >= kMaxTailBlocks) FailTail("window over capacity");
        if (body.WireBytes().size() > kMaxTailMaterial - material) FailTail("material over capacity");
        material += body.WireBytes().size();
        const auto& header = body.Header();
        RecoveryTailBlock block;
        block.hash = header.GetHash();
        block.parent = header.prev_block_hash;
        block.height = height;
        block.timestamp = header.timestamp;
        const auto& transactions = body.Transactions();
        if (transactions.empty()) FailTail("block body empty");
        for (size_t index = 0; index < transactions.size(); ++index) {
            const auto& tx = transactions[index];
            RecoveryTailTransaction effect;
            effect.txid = tx.GetTxid();
            if (tx.IsOrchard()) {
                if (index == 0) FailTail("coinbase must be historical");
                for (const auto& input : tx.Orchard().Inputs()) {
                    if (++spends > kMaxTailSpends) FailTail("spend inventory over capacity");
                    uint256 hash;
                    std::copy(input.txid_wire.begin(), input.txid_wire.end(), hash.begin());
                    effect.spends.emplace_back(TxId(hash), input.output_index);
                }
                for (const auto& coin : tx.Orchard().Outputs()) {
                    if (++outputs > kMaxTailOutputs || coin.script_pub_key.size() > kMaxTailScriptBytes)
                        FailTail("output inventory over capacity");
                    effect.outputs.push_back({coin.amount_una, coin.script_pub_key, false});
                }
            } else {
                const auto& historical = tx.Historical();
                if ((index == 0) != historical.IsCoinbase()) FailTail("coinbase position invalid");
                if (!historical.IsCoinbase())
                    for (const auto& input : historical.vin) {
                        if (++spends > kMaxTailSpends) FailTail("spend inventory over capacity");
                        effect.spends.emplace_back(input.prevout.txid, input.prevout.vout);
                    }
                for (const auto& coin : historical.vout) {
                    if (++outputs > kMaxTailOutputs || coin.scriptPubKey.size() > kMaxTailScriptBytes)
                        FailTail("output inventory over capacity");
                    effect.outputs.push_back({coin.value.GetUna(), coin.scriptPubKey, coin.is_confidential});
                }
            }
            block.transactions.push_back(std::move(effect));
        }
        result.blocks.push_back(std::move(block));
    };
    // Verified activation prefix (late-v6 composition); absent, the walk is
    // empty because ActivationPrefixHeight() reports the activation parent.
    const auto prefix_end = replay.ActivationPrefixHeight();
    for (uint32_t height = facts.activation_height; height <= prefix_end; ++height) {
        const auto step = replay.PrefixStep(height);  // refuses absent prefix material
        if (!step.block || step.context.height != height || step.context.parent_hash != top_hash() ||
            step.block->Header().GetHash() != step.context.block_hash ||
            step.block->Header().prev_block_hash != step.context.parent_hash)
            FailTail("prefix linkage invalid");
        extract(height, *step.block);
    }
    const auto head = replay.Head();
    if (head.sequence > kMaxTailEvents) FailTail("event walk over capacity");
    for (uint64_t sequence = 1; sequence <= head.sequence; ++sequence) {
        const auto event = replay.Event(sequence);
        if (!event || event->cursor.sequence != sequence) FailTail("event sequence invalid");
        const auto& context = event->context;
        if (event->direction == RuntimeBlockDirection::Connect) {
            const auto block = replay.Block(sequence);
            if (!block || uint64_t(context.height) != uint64_t(top_height()) + 1 ||
                context.parent_hash != top_hash() || block->Header().GetHash() != context.block_hash ||
                block->Header().prev_block_hash != context.parent_hash)
                FailTail("connect linkage invalid");
            extract(context.height, *block);
        } else if (event->direction == RuntimeBlockDirection::Disconnect) {
            if (result.blocks.empty()) FailTail("disconnect below the recovery base");
            if (context.block_hash != result.blocks.back().hash || context.height != result.blocks.back().height)
                FailTail("disconnect order invalid");
            result.blocks.pop_back();
        } else {
            FailTail("event direction unsupported");
        }
    }
    const auto tip = replay.Point(head).checkpoint;
    if (tip.height != top_height() || tip.block_hash != top_hash()) FailTail("selected head mismatch");
    return result;
}
}  // namespace dinero
