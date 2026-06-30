#include "common/status.h"
#include "consensus/chainparams.h"
#include "consensus/genesis_canonical.h"
#include "dinero/compat/int128.hpp"
#include "consensus/merkle_root.h"
#include "consensus/reindexer.h"
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_block_validation.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/utreexo_accumulator.h"
#include "primitives/block.h"
#include "storage/archival_block_reader.h"
#include "storage/block_storage.h"
#include "storage/chain_db.h"

#include <sqlite3.h>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace sh = dinero::consensus::shielded;

namespace {

struct TestFailure : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

std::string StatusString(dinero::Status status) {
    return dinero::StatusToString(status);
}

std::string HashToHexUpper(const sh::Hash& hash) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (uint8_t byte : hash) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

sh::Hash MakeHash(uint8_t fill) {
    sh::Hash hash{};
    std::memset(hash.data(), fill, sh::HASH_BYTES);
    return hash;
}

sh::Hash ValueAsHash(uint64_t value) {
    sh::Hash hash{};
    for (int i = 0; i < 8; ++i) {
        hash[31 - i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return hash;
}

struct TestNote {
    sh::Hash secret_key;
    sh::Hash public_key;
    sh::Hash value_hash;
    sh::Hash randomness;
    sh::Hash commitment;
};

TestNote MakeSpendableNote(uint8_t seed_base) {
    TestNote note{};
    note.secret_key = MakeHash(seed_base);
    sh::Hash zero{};
    note.public_key = sh::PoseidonHash2(note.secret_key, zero);
    note.value_hash = ValueAsHash(100'000'000 + seed_base);
    note.randomness = MakeHash(static_cast<uint8_t>(seed_base + 2));
    // d=zero: this fixture predates the (d, pk, value, randomness) signature
    // bump in 1a2f8f05c; the reference path doesn't exercise the address-bind
    // domain so a zero diversifier is fine here.
    note.commitment = sh::NoteCommitment(sh::Hash{}, note.public_key, note.value_hash, note.randomness);
    return note;
}

sh::ShieldedOutput MakeProvenOutput(const TestNote& note) {
    sh::OutputWitness witness;
    witness.value = note.value_hash;
    witness.public_key = note.public_key;
    witness.randomness = note.randomness;
    witness.d = sh::Hash{};

    sh::OutputPublicInputs pub;
    pub.commitment = note.commitment;

    sh::ShieldedOutput output;
    output.commitment = note.commitment;
    output.zk_proof = sh::ProveOutput(witness, pub, nullptr);
    Require(!output.zk_proof.empty(), "failed to prove shielded output fixture");
    return output;
}

sh::ShieldedSpend MakeProvenSpend(const TestNote& note,
                                  uint64_t leaf_index,
                                  const sh::CommitmentTree& tree) {
    auto auth_path = tree.GetAuthPath(leaf_index);
    Require(auth_path.has_value(), "missing auth path for shielded spend");

    sh::SpendWitness witness;
    witness.secret_key = note.secret_key;
    witness.leaf_index = leaf_index;
    witness.value = note.value_hash;
    witness.randomness = note.randomness;
    witness.d = sh::Hash{};
    witness.merkle_path = auth_path->siblings;

    sh::SpendPublicInputs pub;
    pub.nullifier = sh::ComputeNullifier(note.secret_key, leaf_index);
    pub.anchor = tree.Root();

    sh::ShieldedSpend spend;
    spend.nullifier = pub.nullifier;
    spend.anchor = pub.anchor;
    spend.zk_proof = sh::ProveSpend(witness, pub, nullptr);
    Require(!spend.zk_proof.empty(), "failed to prove shielded spend fixture");
    return spend;
}

// Inflation-fix update: the live reindexer now requires a non-empty aggregated
// range proof + valid Schnorr binding sig on every active-height bundle. The
// PlannedOutput/PlannedSpend variants below carry the same real Spartan proofs
// as their MakeProven* counterparts plus the cv blind (rcv), rangeproof nonce,
// and a `value_una` chosen so the builder-derived value_balance matches the
// transparent delta of the carrying tx. (The cv value need not equal the note's
// committed value — the circuit does not yet bind cv to the note value; that is
// the out-of-scope second inflation vector. This fixture exercises the
// range-proof + binding-sig path, not cv↔note binding.)
sh::PlannedOutput MakePlannedOutput(const TestNote& note,
                                    uint64_t value_una,
                                    uint8_t blind_seed) {
    sh::OutputWitness witness;
    witness.value = note.value_hash;
    witness.public_key = note.public_key;
    witness.randomness = note.randomness;
    witness.d = sh::Hash{};

    sh::OutputPublicInputs pub;
    pub.commitment = note.commitment;

    sh::PlannedOutput po;
    po.commitment = note.commitment;
    po.value_una = value_una;
    po.rcv = MakeHash(blind_seed);
    po.encrypted_note = std::vector<uint8_t>(32, 0xAA);
    po.output_proof = sh::ProveOutput(witness, pub, nullptr);
    Require(!po.output_proof.empty(), "failed to prove planned shielded output");
    po.nonce = MakeHash(static_cast<uint8_t>(blind_seed + 1));
    return po;
}

sh::PlannedSpend MakePlannedSpend(const TestNote& note,
                                  uint64_t leaf_index,
                                  const sh::CommitmentTree& tree,
                                  uint64_t value_una,
                                  uint8_t blind_seed) {
    auto auth_path = tree.GetAuthPath(leaf_index);
    Require(auth_path.has_value(), "missing auth path for planned shielded spend");

    sh::SpendWitness witness;
    witness.secret_key = note.secret_key;
    witness.leaf_index = leaf_index;
    witness.value = note.value_hash;
    witness.randomness = note.randomness;
    witness.d = sh::Hash{};
    witness.merkle_path = auth_path->siblings;

    sh::SpendPublicInputs pub;
    pub.nullifier = sh::ComputeNullifier(note.secret_key, leaf_index);
    pub.anchor = tree.Root();

    sh::PlannedSpend ps;
    ps.nullifier = pub.nullifier;
    ps.anchor = pub.anchor;
    ps.value_una = value_una;
    ps.rcv = MakeHash(blind_seed);
    ps.spend_proof = sh::ProveSpend(witness, pub, nullptr);
    Require(!ps.spend_proof.empty(), "failed to prove planned shielded spend");
    ps.nonce = MakeHash(static_cast<uint8_t>(blind_seed + 1));
    return ps;
}

struct RefOutPoint {
    dinero::uint256 txid;
    uint32_t vout{0};

    bool operator<(const RefOutPoint& other) const {
        if (txid != other.txid) {
            return txid < other.txid;
        }
        return vout < other.vout;
    }
};

struct RefCoin {
    uint64_t amount{0};
    std::vector<uint8_t> script_pubkey;
    bool coinbase{false};
    bool is_confidential{false};
};

struct PrefixExpectation {
    uint32_t height{0};
    dinero::uint256 block_hash;
    std::vector<uint8_t> frontier_before;
    std::vector<uint8_t> frontier_after;
    std::vector<std::pair<std::string, uint32_t>> nullifier_rows;
};

struct ReferenceState {
    dinero::consensus::UtreexoForest forest;
    sh::CommitmentTree tree;
    sh::AnchorHistory anchor_history;
    sh::NullifierSet nullifiers;
    std::map<RefOutPoint, RefCoin> utxos;
    std::vector<std::pair<std::string, uint32_t>> nullifier_rows;
};

bool UsesShieldedValueSemantics(const dinero::Transaction& tx) {
    return dinero::Transaction::IsShieldedVersion(tx.version) ||
           !tx.shielded_bundle_bytes.empty();
}

uint64_t SumOutputs(const dinero::Transaction& tx) {
    uint64_t sum = 0;
    for (const auto& output : tx.vout) {
        const uint64_t value = output.value.GetUna();
        sum += value;
        if (sum < value) {
            return UINT64_MAX;
        }
    }
    return sum;
}

bool ComputeValidatedTransactionFee(const dinero::Transaction& tx,
                                    uint64_t total_input_value,
                                    uint64_t total_output_value,
                                    uint64_t& fee,
                                    std::string& error) {
    if (UsesShieldedValueSemantics(tx) && tx.HasExplicitFee()) {
        fee = tx.GetExplicitFee();
        return true;
    }
    if (total_output_value > total_input_value) {
        error = "negative-fee";
        return false;
    }
    fee = total_input_value - total_output_value;
    return true;
}

bool ComputeTransparentValueDelta(uint64_t total_input_value,
                                  uint64_t total_output_value,
                                  uint64_t fee,
                                  int64_t& delta,
                                  std::string& error) {
    using dinero::compat::i128;
    using dinero::compat::i128_zext_u64;
    // i128_zext_u64 preserves the original `(__int128)uint64_t` zero-extend
    // semantics under any backend (native or struct).
    const i128 signed_delta =
        i128_zext_u64(total_input_value) -
        i128_zext_u64(total_output_value) -
        i128_zext_u64(fee);
    if (signed_delta < i128(std::numeric_limits<int64_t>::min()) ||
        signed_delta > i128(std::numeric_limits<int64_t>::max())) {
        error = "transparent-delta-out-of-range";
        return false;
    }
    delta = static_cast<int64_t>(signed_delta);
    return true;
}

dinero::uint256 ForestRootToUint256(const dinero::consensus::UtreexoHash& root) {
    dinero::uint256 out;
    out.SetNull();
    if (root.size() == 32) {
        std::memcpy(out.data, root.data(), 32);
    }
    return out;
}

std::vector<uint8_t> ScriptBytes(uint8_t a, uint8_t b) {
    return {a, b};
}

dinero::Transaction MakeCoinbaseTx(uint32_t height, uint64_t value, uint8_t tag) {
    dinero::Transaction tx;
    tx.version = dinero::Transaction::TX_VERSION_SEGWIT;
    tx.witness_version = 0xFF;
    tx.lockTime = 0;

    dinero::TxInput input;
    input.prevout = dinero::TxOutPoint(dinero::TxId(dinero::uint256()), 0xffffffff);
    input.scriptSig = {static_cast<uint8_t>(height & 0xFF), tag};
    tx.vin.push_back(input);

    tx.vout.emplace_back(dinero::AmountUna::Una(value), ScriptBytes(0x51, tag));
    return tx;
}

dinero::Transaction MakeShieldedTx(const dinero::uint256& prev_txid,
                                   uint32_t prev_vout,
                                   uint64_t output_value,
                                   uint64_t fee,
                                   const sh::ShieldedBundle& bundle,
                                   uint8_t tag) {
    dinero::Transaction tx;
    tx.version = dinero::Transaction::TX_VERSION_SHIELDED;
    tx.witness_version = 0xFF;
    tx.lockTime = 0;

    dinero::TxInput input;
    input.prevout = dinero::TxOutPoint(dinero::TxId(prev_txid), prev_vout);
    input.scriptSig = {tag};
    tx.vin.push_back(input);

    if (output_value > 0) {
        tx.vout.emplace_back(dinero::AmountUna::Una(output_value), ScriptBytes(0x52, tag));
    }
    tx.SetExplicitFee(fee);
    tx.shielded_bundle_bytes = sh::SerializeShieldedBundle(bundle);
    return tx;
}

// Build a transparent shielded tx, then attach a builder-produced bundle whose
// binding sig is signed over the tx's canonical sighash (which does not depend
// on the bundle bytes). The supplied value_una values must yield
// value_balance == the tx's transparent delta.
dinero::Transaction MakeShieldedTxWithBuiltBundle(
        const dinero::uint256& prev_txid,
        uint32_t prev_vout,
        uint64_t output_value,
        uint64_t fee,
        const std::vector<sh::PlannedSpend>& spends,
        const std::vector<sh::PlannedOutput>& outputs,
        uint8_t tag) {
    dinero::Transaction tx =
        MakeShieldedTx(prev_txid, prev_vout, output_value, fee, sh::ShieldedBundle{}, tag);
    const sh::Hash sighash = sh::ComputeShieldedTxSighash(tx);
    sh::ShieldedBundle bundle;
    Require(sh::BuildShieldedBundle(spends, outputs, sighash, bundle) ==
                sh::BundleBuildResult::Ok,
            "BuildShieldedBundle failed for reindex fixture");
    tx.shielded_bundle_bytes = sh::SerializeShieldedBundle(bundle);
    return tx;
}

PrefixExpectation FinalizeAndApplyReferenceBlock(dinero::Block& block,
                                                 uint32_t height,
                                                 const dinero::uint256& prev_hash,
                                                 ReferenceState& state) {
    PrefixExpectation expectation;
    expectation.height = height;
    expectation.frontier_before = state.tree.SerializeFrontier();

    std::vector<sh::ShieldedBundle> bundles;
    std::vector<int64_t> transparent_deltas;
    std::vector<std::pair<std::string, uint32_t>> new_nullifiers;

    auto next_utxos = state.utxos;

    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto& tx = block.vtx[tx_idx];
        uint64_t total_input_value = 0;

        if (tx_idx > 0) {
            for (const auto& input : tx.vin) {
                RefOutPoint key{input.prevout.txid.AsUint256(), input.prevout.vout};
                auto it = next_utxos.find(key);
                Require(it != next_utxos.end(), "reference UTXO missing for spend");

                total_input_value += it->second.amount;
                next_utxos.erase(it);
            }
        }

        if (UsesShieldedValueSemantics(tx)) {
            sh::ShieldedBundle bundle;
            const auto decode = sh::DeserializeShieldedBundle(tx.shielded_bundle_bytes, &bundle);
            Require(decode == sh::BundleDecodeError::Ok, "reference decode failed");

            const uint64_t total_output_value = SumOutputs(tx);
            Require(total_output_value != UINT64_MAX, "reference SumOutputs overflow");

            uint64_t fee = 0;
            std::string fee_error;
            Require(ComputeValidatedTransactionFee(tx, total_input_value, total_output_value, fee, fee_error),
                    "reference fee failure: " + fee_error);

            int64_t transparent_delta = 0;
            std::string delta_error;
            Require(ComputeTransparentValueDelta(total_input_value, total_output_value, fee,
                                                 transparent_delta, delta_error),
                    "reference transparent delta failure: " + delta_error);

            // Reindex-equivalence fixture: keep the reference builder on
            // the same ValidationContext construction path as live
            // ConnectTip/reindex. These synthetic bundles still are not
            // the test's authority — byte-for-byte replay equivalence
            // below is — but this tripwire prevents the fixture from
            // drifting back to positional/default context construction.
            sh::ValidationContext ctx = sh::BuildShieldedValidationContext(
                tx,
                &state.nullifiers,
                &state.tree,
                height,
                transparent_delta,
                dinero::Params().shielded_activation_height,
                &state.anchor_history);
            (void)sh::ValidateShieldedBundle(bundle, ctx);

            for (const auto& spend : bundle.spends) {
                new_nullifiers.emplace_back(HashToHexUpper(spend.nullifier), height);
            }

            bundles.push_back(std::move(bundle));
            transparent_deltas.push_back(transparent_delta);
        }

        const auto txid = tx.GetTxid().AsUint256();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            const auto& output = tx.vout[vout];
            RefCoin coin;
            coin.amount = output.value.GetUna();
            coin.script_pubkey = output.scriptPubKey;
            coin.coinbase = (tx_idx == 0);
            coin.is_confidential = output.is_confidential;
            next_utxos[{txid, vout}] = std::move(coin);
        }
    }

    // Mirror live/reindex Utreexo mutation order exactly:
    //   1. remove all non-ephemeral spends in the whole block
    //   2. add all non-ephemeral outputs in block tx order
    auto next_forest = state.forest.clone();
    std::map<RefOutPoint, size_t> intra_block_outputs;
    for (size_t tx_idx = 0; tx_idx < block.vtx.size(); ++tx_idx) {
        const auto txid = block.vtx[tx_idx].GetTxid().AsUint256();
        for (uint32_t n = 0; n < block.vtx[tx_idx].vout.size(); ++n) {
            intra_block_outputs[{txid, n}] = tx_idx;
        }
    }

    std::set<RefOutPoint> intra_block_spends;
    for (const auto& tx : block.vtx) {
        if (tx.IsCoinbase()) {
            continue;
        }
        for (const auto& input : tx.vin) {
            RefOutPoint outpoint{input.prevout.txid.AsUint256(), input.prevout.vout};
            if (intra_block_outputs.count(outpoint) != 0) {
                intra_block_spends.insert(outpoint);
            }
        }
    }

    for (const auto& tx : block.vtx) {
        if (tx.IsCoinbase()) {
            continue;
        }
        for (const auto& input : tx.vin) {
            RefOutPoint key{input.prevout.txid.AsUint256(), input.prevout.vout};
            if (intra_block_spends.count(key) != 0) {
                continue;
            }
            auto it = state.utxos.find(key);
            Require(it != state.utxos.end(), "reference pre-block UTXO missing for forest removal");

            const auto leaf_hash = dinero::consensus::HashUTXO(
                input.prevout.txid.AsUint256(),
                input.prevout.vout,
                it->second.is_confidential ? 0 : it->second.amount,
                it->second.script_pubkey);
            auto pos = next_forest.findLeafPosition(leaf_hash);
            Require(pos.has_value(), "reference forest missing spent leaf");
            Require(next_forest.removeAtKnownPosition(pos.value(), leaf_hash),
                    "reference forest failed to remove spent leaf");
        }
    }

    for (const auto& tx : block.vtx) {
        const auto txid = tx.GetTxid().AsUint256();
        for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
            RefOutPoint outpoint{txid, vout};
            if (intra_block_spends.count(outpoint) != 0) {
                continue;
            }
            const auto& output = tx.vout[vout];
            const auto leaf_hash = dinero::consensus::HashUTXO(
                txid,
                vout,
                output.is_confidential ? 0 : output.value.GetUna(),
                std::vector<uint8_t>(output.scriptPubKey.begin(), output.scriptPubKey.end()));
            const uint64_t position = next_forest.add(leaf_hash);
            Require(position != UINT64_MAX, "reference forest failed to add leaf");
        }
    }

    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.merkle_root = dinero::consensus::ComputeMerkleRoot(block.vtx);
    block.header.utreexo_root = ForestRootToUint256(next_forest.getCommitment());
    block.header.timestamp = 1776384000ULL + height;
    block.header.difficulty = 0x207fffff;
    block.header.nonce = 1000 + height;
    block.header.ZeroReserved();

    if (!bundles.empty()) {
        sh::BlockShieldedContext block_ctx{
            &state.nullifiers,
            &state.tree,
            height,
        };
        const auto block_validation =
            sh::ValidateBlockShielded(bundles, transparent_deltas, block_ctx);
        Require(block_validation == sh::BlockValidationError::Ok,
                "reference block shielded validation failed");
    }

    state.utxos = std::move(next_utxos);
    state.forest = std::move(next_forest);

    if (!bundles.empty()) {
        sh::ApplyBlockShielded(bundles, &state.tree, &state.nullifiers, height);
        state.nullifier_rows.insert(state.nullifier_rows.end(),
                                    new_nullifiers.begin(), new_nullifiers.end());
        std::sort(state.nullifier_rows.begin(), state.nullifier_rows.end());
    }
    state.anchor_history.RecordRoot(height, state.tree.Root());

    expectation.block_hash = block.GetHash();
    expectation.frontier_after = state.tree.SerializeFrontier();
    expectation.nullifier_rows = state.nullifier_rows;
    return expectation;
}

std::vector<uint8_t> ReadFileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    Require(in.is_open(), "failed to open file: " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

std::vector<std::pair<std::string, uint32_t>> DumpNullifiers(const fs::path& db_path) {
    sqlite3* db = nullptr;
    Require(sqlite3_open(db_path.string().c_str(), &db) == SQLITE_OK,
            "failed to open nullifier DB");

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT hex(nullifier), block_height FROM nullifiers ORDER BY nullifier ASC";
    Require(sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK,
            "failed to prepare nullifier dump query");

    std::vector<std::pair<std::string, uint32_t>> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(stmt, 0);
        const int height = sqlite3_column_int(stmt, 1);
        rows.emplace_back(text ? reinterpret_cast<const char*>(text) : "", static_cast<uint32_t>(height));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rows;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::optional<fs::path> emit_datadir;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--emit-datadir") {
                Require(i + 1 < argc, "--emit-datadir requires a path");
                emit_datadir = fs::path(argv[++i]);
                continue;
            }
            throw TestFailure("unknown argument: " + arg);
        }

        dinero::SelectParams(dinero::Chain::REGTEST);

        const auto canonical = dinero::BuildCanonicalGenesis(dinero::Params());
        dinero::Transaction genesis_coinbase;
        const auto genesis_coinbase_bytes =
            dinero::TransactionSerializer::FromHex(canonical.coinbase_hex);
        size_t genesis_coinbase_consumed = 0;
        Require(!genesis_coinbase_bytes.empty() &&
                    dinero::TransactionSerializer::Deserialize(
                        genesis_coinbase, genesis_coinbase_bytes, genesis_coinbase_consumed) &&
                    genesis_coinbase_consumed == genesis_coinbase_bytes.size(),
                "failed to decode canonical genesis coinbase");

        dinero::Block genesis_block;
        genesis_block.header = canonical.header;
        genesis_block.vtx.push_back(genesis_coinbase);
        const auto genesis_hash = genesis_block.GetHash();

        ReferenceState state;
        const fs::path reference_db =
            fs::temp_directory_path() /
            ("dinero_shielded_reference_nullifiers_" + std::to_string(getpid()) + ".db");
        std::error_code ec;
        fs::remove(reference_db, ec);
        Require(state.nullifiers.Open(reference_db.string()) == sh::NullifierSet::OpenResult::Ok,
                "failed to open reference nullifier DB");

        std::vector<dinero::Block> blocks;
        std::vector<PrefixExpectation> expectations;

        // Height 1: transparent coinbase only.
        {
            dinero::Block block;
            block.vtx.push_back(MakeCoinbaseTx(1, 50, 0x11));
            expectations.push_back(FinalizeAndApplyReferenceBlock(block, 1, genesis_hash, state));
            blocks.push_back(std::move(block));
        }

        const auto note1 = MakeSpendableNote(0x10);
        // Height 2: shield from transparent into shielded pool.
        // Transparent delta = 50 (coinbase in) - 40 (out) - 1 (fee) = +9, so the
        // builder must derive value_balance = +9 (one output of value_una 9).
        {
            dinero::Block block;
            block.vtx.push_back(MakeCoinbaseTx(2, 50, 0x12));
            block.vtx.push_back(MakeShieldedTxWithBuiltBundle(
                blocks[0].vtx[0].GetTxid().AsUint256(), 0, 40, 1,
                /*spends=*/{}, {MakePlannedOutput(note1, /*value_una=*/9, 0x30)},
                0x21));
            expectations.push_back(FinalizeAndApplyReferenceBlock(
                block, 2, blocks.back().GetHash(), state));
            blocks.push_back(std::move(block));
        }

        const auto note2 = MakeSpendableNote(0x40);
        // Height 3: shielded transfer (spend note1, create note2).
        // Transparent delta = 40 - 39 - 1 = 0, so spend/output value_una are equal.
        {
            dinero::Block block;
            block.vtx.push_back(MakeCoinbaseTx(3, 50, 0x13));
            block.vtx.push_back(MakeShieldedTxWithBuiltBundle(
                blocks[1].vtx[1].GetTxid().AsUint256(), 0, 39, 1,
                {MakePlannedSpend(note1, 0, state.tree, /*value_una=*/1000, 0x32)},
                {MakePlannedOutput(note2, /*value_una=*/1000, 0x34)},
                0x22));
            expectations.push_back(FinalizeAndApplyReferenceBlock(
                block, 3, blocks.back().GetHash(), state));
            blocks.push_back(std::move(block));
        }

        // Height 4: unshield note2 back to transparent.
        // Transparent delta = 39 - 43 - 1 = -5, so the spend's value_una = 5
        // (no outputs) gives value_balance = 0 - 5 = -5.
        {
            dinero::Block block;
            block.vtx.push_back(MakeCoinbaseTx(4, 50, 0x14));
            block.vtx.push_back(MakeShieldedTxWithBuiltBundle(
                blocks[2].vtx[1].GetTxid().AsUint256(), 0, 43, 1,
                {MakePlannedSpend(note2, 1, state.tree, /*value_una=*/5, 0x36)},
                /*outputs=*/{}, 0x23));
            expectations.push_back(FinalizeAndApplyReferenceBlock(
                block, 4, blocks.back().GetHash(), state));
            blocks.push_back(std::move(block));
        }

        // Height 5: transparent tail block to prove stability after shielded state stops changing.
        {
            dinero::Block block;
            block.vtx.push_back(MakeCoinbaseTx(5, 50, 0x15));
            expectations.push_back(FinalizeAndApplyReferenceBlock(
                block, 5, blocks.back().GetHash(), state));
            blocks.push_back(std::move(block));
        }

        state.nullifiers.Close();
        fs::remove(reference_db, ec);

        const auto run_prefix = [&](const fs::path& prefix_dir,
                                    const fs::path& chain_db_path,
                                    const fs::path& frontier_path,
                                    const fs::path& nullifier_db_path,
                                    size_t prefix) {
            dinero::BlockStorage block_storage;
            auto storage_status = block_storage.init(prefix_dir);
            Require(storage_status == dinero::Status::Ok,
                    "failed to init BlockStorage: " + StatusString(storage_status));

            auto genesis_write = block_storage.writeBlock(genesis_hash, genesis_block);
            Require(genesis_write.ok(), "failed to write genesis block");
            for (size_t i = 0; i < prefix; ++i) {
                auto write_result = block_storage.writeBlock(blocks[i].GetHash(), blocks[i]);
                Require(write_result.ok(), "failed to write prefix block");
            }
            auto flush_status = block_storage.flush();
            Require(flush_status == dinero::Status::Ok,
                    "failed to flush block storage");

            dinero::ChainDB chain_db;
            auto db_status = chain_db.init(chain_db_path);
            Require(db_status == dinero::Status::Ok,
                    "failed to init ChainDB: " + StatusString(db_status));

            dinero::consensus::BlockReindexer::Config config;
            config.mode = dinero::consensus::BlockReindexer::Mode::FULL;
            config.use_assumevalid = true;
            config.progress_interval = 1000;
            config.shielded_frontier_output_path = frontier_path;
            config.shielded_nullifier_db_path = nullifier_db_path;

            dinero::consensus::BlockReindexer reindexer(prefix_dir, &chain_db, &block_storage, config);
            auto reindex_result = reindexer.execute();
            Require(reindex_result.ok(), "reindex execution failed");
            Require(reindex_result.value().success, "reindex stats reported failure");

            const auto& expected = expectations[prefix - 1];

            auto tip_result = chain_db.getTip();
            Require(tip_result.ok(), "missing tip after reindex");
            Require(tip_result.value().height == static_cast<int>(expected.height),
                    "tip height mismatch at prefix " + std::to_string(prefix));
            Require(tip_result.value().hash == expected.block_hash,
                    "tip hash mismatch at prefix " + std::to_string(prefix));

            const auto frontier_bytes = ReadFileBytes(config.shielded_frontier_output_path);
            Require(frontier_bytes == expected.frontier_after,
                    "shielded frontier mismatch at prefix " + std::to_string(prefix));

            const auto nullifier_rows = DumpNullifiers(config.shielded_nullifier_db_path);
            Require(nullifier_rows == expected.nullifier_rows,
                    "shielded nullifier rows mismatch at prefix " + std::to_string(prefix));

            auto undo_result = dinero::storage::ReadArchivalUndo(
                chain_db, &block_storage, expected.block_hash,
                dinero::storage::ArchivalReadMode::RequireFlatfiles);
            Require(undo_result.ok(), "failed to read archival undo at prefix " +
                                      std::to_string(prefix));
            Require(undo_result.value().pre_block_shielded_frontier.has_value(),
                    "undo missing pre-block shielded frontier at prefix " +
                    std::to_string(prefix));
            Require(*undo_result.value().pre_block_shielded_frontier == expected.frontier_before,
                    "undo frontier mismatch at prefix " + std::to_string(prefix));

            block_storage.close();
            chain_db.close();
        };

        if (emit_datadir.has_value()) {
            fs::remove_all(*emit_datadir, ec);
            fs::create_directories(*emit_datadir);

            const auto& expected = expectations.back();
            run_prefix(*emit_datadir,
                       *emit_datadir / "blockchain" / "chaindb",
                       *emit_datadir / "blockchain" / "shielded_frontier.bin",
                       *emit_datadir / "blockchain" / "shielded_nullifiers.db",
                       blocks.size());

            const auto frontier_bytes =
                ReadFileBytes(*emit_datadir / "blockchain" / "shielded_frontier.bin");
            Require(frontier_bytes == expected.frontier_after,
                    "fixture shielded frontier mismatch");
            const auto nullifier_rows =
                DumpNullifiers(*emit_datadir / "blockchain" / "shielded_nullifiers.db");
            Require(nullifier_rows == expected.nullifier_rows,
                    "fixture nullifier rows mismatch");

            std::printf("ShieldedReindexFixture: PASS (%s)\n",
                        emit_datadir->string().c_str());
            return 0;
        }

        const fs::path base_dir =
            fs::temp_directory_path() /
            ("dinero_shielded_reindex_equivalence_" + std::to_string(getpid()));
        fs::remove_all(base_dir, ec);
        fs::create_directories(base_dir);

        for (size_t prefix = 1; prefix <= blocks.size(); ++prefix) {
            const fs::path prefix_dir = base_dir / ("prefix_" + std::to_string(prefix));
            fs::create_directories(prefix_dir);
            run_prefix(prefix_dir,
                       prefix_dir / "blockchain" / "chaindb.reindex",
                       prefix_dir / "blockchain" / "shielded_frontier.reindex.bin",
                       prefix_dir / "blockchain" / "shielded_nullifiers.reindex.db",
                       prefix);
        }

        fs::remove_all(base_dir, ec);

        std::printf("ShieldedReindexEquivalence: PASS\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ShieldedReindexEquivalence: FAIL: %s\n", e.what());
        return 1;
    }
}
