// Phase 3 wave 3b: integration-test bundle builder.
//
// Constructs v0.3.0 shielded bundles (cv + range proof + bvk_commitment +
// binding_sig) for the daemon-restart equivalence tests. Deterministic per
// `--note-seed` so test fixtures are reproducible across runs and across
// restarts of the daemon under test.

#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_tx.h"
#include "primitives/transaction.h"

#include <json/json.h>

#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace sh = dinero::consensus::shielded;

struct TestNote {
    sh::Hash secret_key{};
    sh::Hash public_key{};
    sh::Hash value_hash{};
    sh::Hash randomness{};
    sh::Hash diversifier{};   ///< zero until shielded address surfacing (Phase 5)
    sh::Hash commitment{};
    uint64_t value_una{0};
};

void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  shielded_tx_builder attach-shield-output"
        << " --raw-tx <hex>"
        << " --shield-value-una <n>"
        << " --explicit-fee-una <n>"
        << " --note-seed <0-255>\n"
        << "  shielded_tx_builder build-transfer"
        << " --input-note-seed <0-255>"
        << " --input-value-una <n>"
        << " --input-leaf-index <n>"
        << " --output-note-seed <0-255>"
        << " --output-value-una <n>"
        << " [--explicit-fee-una <n>]\n";
}

std::string HashToHex(const sh::Hash& hash) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint8_t byte : hash) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

sh::Hash MakeHash(uint8_t fill) {
    sh::Hash hash{};
    std::memset(hash.data(), fill, hash.size());
    return hash;
}

// Phase 2 wave 4: encode value into Hash matching the circuit's
// big-endian Scalar layout. value bytes go into hash[24..31] so the
// in-circuit `range_check_limb` (val < 2^64) is satisfied.
sh::Hash ValueToHash(uint64_t value_una) {
    sh::Hash hash{};
    for (int i = 0; i < 8; ++i) {
        hash[31 - i] = static_cast<uint8_t>((value_una >> (8 * i)) & 0xFF);
    }
    return hash;
}

TestNote MakeDeterministicNote(uint8_t seed_base, uint64_t value_una) {
    TestNote note;
    note.secret_key = MakeHash(seed_base);
    sh::Hash zero{};
    note.public_key = sh::PoseidonHash2(note.secret_key, zero);
    note.value_hash = ValueToHash(value_una);
    note.randomness = MakeHash(static_cast<uint8_t>(seed_base + 1));
    // Phase 2 wave 5: zero diversifier until shielded address surfacing.
    note.diversifier = sh::Hash{};
    note.commitment =
        sh::NoteCommitment(note.diversifier, note.public_key,
                           note.value_hash, note.randomness);
    note.value_una = value_una;
    return note;
}

// Deterministic Pedersen-side material. rcv MUST stay below curve order; we
// derive it (and the rangeproof nonce) from the note seed by setting a small
// scalar in the low byte. Same convention as the unit tests.
sh::Hash DeterministicScalar(uint8_t seed) {
    sh::Hash s{};
    s[31] = seed;
    return s;
}

std::vector<uint8_t> EmptyEncryptedNote() {
    return std::vector<uint8_t>(96, 0);  // TODO Phase 5: real ECDH ciphertext
}

sh::PlannedOutput MakePlannedOutput(const TestNote& note, uint8_t pedersen_seed) {
    sh::OutputWitness witness{};
    witness.value      = note.value_hash;
    witness.public_key = note.public_key;
    witness.randomness = note.randomness;
    witness.d          = note.diversifier;

    sh::OutputPublicInputs pub{};
    pub.commitment = note.commitment;

    sh::PlannedOutput planned{};
    planned.commitment     = note.commitment;
    planned.value_una      = note.value_una;
    planned.rcv            = DeterministicScalar(pedersen_seed);
    planned.encrypted_note = EmptyEncryptedNote();
    planned.output_proof   = sh::ProveOutput(witness, pub, nullptr);
    planned.nonce          = DeterministicScalar(static_cast<uint8_t>(pedersen_seed ^ 0x55));
    return planned;
}

std::optional<sh::PlannedSpend> MakePlannedSpend(const TestNote& note,
                                                 uint64_t leaf_index,
                                                 const sh::CommitmentTree& tree,
                                                 uint8_t pedersen_seed) {
    const auto auth_path = tree.GetAuthPath(leaf_index);
    if (!auth_path.has_value()) {
        return std::nullopt;
    }

    sh::SpendWitness witness{};
    witness.secret_key  = note.secret_key;
    witness.leaf_index  = leaf_index;
    witness.value       = note.value_hash;
    witness.randomness  = note.randomness;
    witness.d           = note.diversifier;
    witness.merkle_path = auth_path->siblings;

    sh::SpendPublicInputs pub{};
    pub.nullifier = sh::ComputeNullifier(note.secret_key, leaf_index);
    pub.anchor    = tree.Root();

    sh::PlannedSpend planned{};
    planned.nullifier   = pub.nullifier;
    planned.anchor      = pub.anchor;
    planned.value_una   = note.value_una;
    planned.rcv         = DeterministicScalar(pedersen_seed);
    planned.spend_proof = sh::ProveSpend(witness, pub, nullptr);
    planned.nonce       = DeterministicScalar(static_cast<uint8_t>(pedersen_seed ^ 0xAA));
    if (planned.spend_proof.empty()) {
        return std::nullopt;
    }
    return planned;
}

bool ReadStringArg(int argc,
                   char** argv,
                   int* index,
                   const std::string& expected,
                   std::string* out) {
    if (std::string(argv[*index]) != expected || *index + 1 >= argc || !out) {
        return false;
    }
    *out = argv[++(*index)];
    return true;
}

bool ReadUint64Arg(int argc,
                   char** argv,
                   int* index,
                   const std::string& expected,
                   uint64_t* out) {
    if (std::string(argv[*index]) != expected || *index + 1 >= argc || !out) {
        return false;
    }
    try {
        *out = std::stoull(argv[++(*index)]);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadUint8Arg(int argc,
                  char** argv,
                  int* index,
                  const std::string& expected,
                  uint8_t* out) {
    uint64_t tmp = 0;
    if (!ReadUint64Arg(argc, argv, index, expected, &tmp) || tmp > 255) {
        return false;
    }
    *out = static_cast<uint8_t>(tmp);
    return true;
}

void EnsurePedersenReady() {
    if (!sh::PedersenGeneratorsReady()) {
        (void)sh::PedersenGeneratorV();
    }
    if (!sh::PedersenGeneratorsReady()) {
        throw std::runtime_error("pedersen generators failed to initialize");
    }
}

Json::Value BuildAttachShieldOutput(int argc, char** argv) {
    std::string raw_tx_hex;
    uint64_t shield_value_una = 0;
    uint64_t explicit_fee_una = 0;
    uint8_t note_seed = 0;

    for (int i = 2; i < argc; ++i) {
        if (ReadStringArg(argc, argv, &i, "--raw-tx", &raw_tx_hex)) {
            continue;
        }
        if (ReadUint64Arg(argc, argv, &i, "--shield-value-una", &shield_value_una)) {
            continue;
        }
        if (ReadUint64Arg(argc, argv, &i, "--explicit-fee-una", &explicit_fee_una)) {
            continue;
        }
        if (ReadUint8Arg(argc, argv, &i, "--note-seed", &note_seed)) {
            continue;
        }
        throw std::runtime_error("unknown or invalid argument: " +
                                 std::string(argv[i]));
    }

    if (raw_tx_hex.empty()) {
        throw std::runtime_error("--raw-tx is required");
    }

    EnsurePedersenReady();

    dinero::Transaction tx;
    const auto raw_tx_bytes = dinero::TransactionSerializer::FromHex(raw_tx_hex);
    size_t consumed = 0;
    if (raw_tx_bytes.empty() ||
        !dinero::TransactionSerializer::Deserialize(tx, raw_tx_bytes, consumed) ||
        consumed != raw_tx_bytes.size()) {
        throw std::runtime_error("failed to decode raw tx");
    }

    // Stamp shielded envelope BEFORE computing tx_sighash so the binding
    // sig commits to version=5 + explicit_fee. Witness fields stay unset;
    // BIP143 sighash is invariant to shielded_bundle_bytes, so the wallet
    // can sign transparent inputs after this binary returns.
    tx.version = dinero::Transaction::TX_VERSION_SHIELDED;
    tx.witness_version = 0xFF;
    tx.SetExplicitFee(explicit_fee_una);

    const TestNote note = MakeDeterministicNote(note_seed, shield_value_una);
    sh::PlannedOutput planned = MakePlannedOutput(note, note_seed);
    if (planned.output_proof.empty()) {
        throw std::runtime_error("output proof generation failed");
    }

    const sh::Hash tx_sighash = sh::ComputeShieldedTxSighash(tx);
    sh::ShieldedBundle bundle{};
    const auto rc = sh::BuildShieldedBundle({}, {planned}, tx_sighash, bundle);
    if (rc != sh::BundleBuildResult::Ok) {
        throw std::runtime_error("BuildShieldedBundle failed: code " +
                                 std::to_string(static_cast<int>(rc)));
    }

    tx.shielded_bundle_bytes = sh::SerializeShieldedBundle(bundle);
    if (tx.shielded_bundle_bytes.empty()) {
        throw std::runtime_error("bundle serialization produced empty bytes");
    }

    Json::Value out(Json::objectValue);
    out["hex"] = tx.SerializeHex(true);
    out["value_una"] = Json::UInt64(shield_value_una);
    out["explicit_fee_una"] = Json::UInt64(explicit_fee_una);
    out["note_seed"] = note_seed;
    out["note_secret_key"] = HashToHex(note.secret_key);
    out["note_public_key"] = HashToHex(note.public_key);
    out["note_value_hash"] = HashToHex(note.value_hash);
    out["note_randomness"] = HashToHex(note.randomness);
    out["note_commitment"] = HashToHex(note.commitment);
    return out;
}

Json::Value BuildTransfer(int argc, char** argv) {
    uint8_t input_note_seed = 0;
    uint64_t input_value_una = 0;
    uint64_t input_leaf_index = 0;
    uint8_t output_note_seed = 0;
    uint64_t output_value_una = 0;
    uint64_t explicit_fee_una = 0;

    for (int i = 2; i < argc; ++i) {
        if (ReadUint8Arg(argc, argv, &i, "--input-note-seed", &input_note_seed)) {
            continue;
        }
        if (ReadUint64Arg(argc, argv, &i, "--input-value-una", &input_value_una)) {
            continue;
        }
        if (ReadUint64Arg(argc, argv, &i, "--input-leaf-index", &input_leaf_index)) {
            continue;
        }
        if (ReadUint8Arg(argc, argv, &i, "--output-note-seed", &output_note_seed)) {
            continue;
        }
        if (ReadUint64Arg(argc, argv, &i, "--output-value-una", &output_value_una)) {
            continue;
        }
        if (ReadUint64Arg(argc, argv, &i, "--explicit-fee-una", &explicit_fee_una)) {
            continue;
        }
        throw std::runtime_error("unknown or invalid argument: " +
                                 std::string(argv[i]));
    }

    if (input_leaf_index != 0) {
        throw std::runtime_error(
            "first daemon-valid Phase 2 builder only supports leaf_index=0");
    }
    if (input_value_una < output_value_una + explicit_fee_una) {
        throw std::runtime_error(
            "input does not cover output + fee — value_balance constraint");
    }

    EnsurePedersenReady();

    const TestNote input_note =
        MakeDeterministicNote(input_note_seed, input_value_una);
    const TestNote output_note =
        MakeDeterministicNote(output_note_seed, output_value_una);

    sh::CommitmentTree tree;
    tree.Append(input_note.commitment);

    auto planned_spend = MakePlannedSpend(input_note, input_leaf_index, tree,
                                          input_note_seed);
    if (!planned_spend.has_value()) {
        throw std::runtime_error("failed to build spend proof");
    }
    sh::PlannedOutput planned_output =
        MakePlannedOutput(output_note,
                          static_cast<uint8_t>(output_note_seed ^ 0x33));
    if (planned_output.output_proof.empty()) {
        throw std::runtime_error("output proof generation failed");
    }

    sh::CommitmentTree expected_tree = tree;
    expected_tree.Append(output_note.commitment);

    dinero::Transaction tx;
    tx.version = dinero::Transaction::TX_VERSION_SHIELDED;
    tx.witness_version = 0xFF;
    tx.lockTime = 0;
    tx.SetExplicitFee(explicit_fee_una);

    // For a pure shielded-to-shielded transfer with no transparent envelope
    // (no vins, no vouts), the binding-sig sighash still domain-separates
    // the bundle. ComputeShieldedTxSighash hashes vin/vout sizes (zero
    // here) + version + locktime + explicit_fee.
    const sh::Hash tx_sighash = sh::ComputeShieldedTxSighash(tx);
    sh::ShieldedBundle bundle{};
    const auto rc = sh::BuildShieldedBundle({*planned_spend},
                                            {planned_output},
                                            tx_sighash, bundle);
    if (rc != sh::BundleBuildResult::Ok) {
        throw std::runtime_error("BuildShieldedBundle failed: code " +
                                 std::to_string(static_cast<int>(rc)));
    }

    tx.shielded_bundle_bytes = sh::SerializeShieldedBundle(bundle);
    if (tx.shielded_bundle_bytes.empty()) {
        throw std::runtime_error("bundle serialization produced empty bytes");
    }

    Json::Value out(Json::objectValue);
    out["hex"] = tx.SerializeHex(true);
    out["explicit_fee_una"] = Json::UInt64(explicit_fee_una);
    out["input_note_seed"] = input_note_seed;
    out["input_nullifier"] = HashToHex(planned_spend->nullifier);
    out["input_anchor"] = HashToHex(planned_spend->anchor);
    out["expected_tree_root"] = HashToHex(expected_tree.Root());
    out["expected_tree_size"] = Json::UInt64(expected_tree.Size());
    out["output_note_seed"] = output_note_seed;
    out["output_value_una"] = Json::UInt64(output_value_una);
    out["output_note_secret_key"] = HashToHex(output_note.secret_key);
    out["output_note_public_key"] = HashToHex(output_note.public_key);
    out["output_note_value_hash"] = HashToHex(output_note.value_hash);
    out["output_note_randomness"] = HashToHex(output_note.randomness);
    out["output_note_commitment"] = HashToHex(output_note.commitment);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    try {
        Json::Value result;
        const std::string command = argv[1];
        if (command == "attach-shield-output") {
            result = BuildAttachShieldOutput(argc, argv);
        } else if (command == "build-transfer") {
            result = BuildTransfer(argc, argv);
        } else {
            PrintUsage();
            return 2;
        }

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::cout << Json::writeString(builder, result) << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "shielded_tx_builder: " << e.what() << "\n";
        return 1;
    }
}
