// K1 consensus forced-link probe (separation gate; spec section 8).
//
// Linked as: dinero_consensus whole-archive + only its declared link deps.
// Returns nonzero on any mismatch (no assert: must gate under NDEBUG too).
//
// Vectors (reused, not invented):
//  * script: first usable case of tests/data/script_tests.json
//      ["", "DEPTH 0 EQUAL", "P2SH,STRICTENC", "OK"]
//    evaluated as the ScriptJSONTests runner does (VerifyScript with a
//    ScriptExecutionContext over a one-input spending transaction).
//    A negative control ("DEPTH 1 EQUAL" must fail) guards against a
//    verifier that accepts everything.
//  * sighash: BIP341 official keyPathSpending vector, input 0, hash_type 3,
//    copied from tests/consensus/test_bip341_sighash_vectors.cpp.

#include "consensus/chainparams.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "consensus/tx_parser.h"
#include "primitives/transaction.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

using dinero::AmountUna;
using dinero::Transaction;
using dinero::TransactionSerializer;
using namespace dinero::consensus;

std::string ToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const uint8_t byte : bytes) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

// First keyPathSpending transaction from the official BIP341 wallet vectors:
// https://github.com/bitcoin/bips/blob/master/bip-0341/wallet-test-vectors.json
constexpr char kUnsignedTx[] =
    "02000000097de20cbff686da83a54981d2b9bab3586f4ca7e48f57f5b55963115f3b334e9c010000000000000000d7b7cab57b1393ace2d064f4d4a2cb8af6def61273e127517d44759b6dafdd990000000000fffffffff8e1f583384333689228c5d28eac13366be082dc57441760d957275419a418420000000000fffffffff0689180aa63b30cb162a73c6d2a38b7eeda2a83ece74310fda0843ad604853b0100000000feffffffaa5202bdf6d8ccd2ee0f0202afbbb7461d9264a25e5bfd3c5a52ee1239e0ba6c0000000000feffffff956149bdc66faa968eb2be2d2faa29718acbfe3941215893a2a3446d32acd050000000000000000000e664b9773b88c09c32cb70a2a3e4da0ced63b7ba3b22f848531bbb1d5d5f4c94010000000000000000e9aa6b8e6c9de67619e6a3924ae25696bb7b694bb677a632a74ef7eadfd4eabf0000000000ffffffffa778eb6a263dc090464cd125c466b5a99667720b1c110468831d058aa1b82af10100000000ffffffff0200ca9a3b000000001976a91406afd46bcdfd22ef94ac122aa11f241244a37ecc88ac807840cb0000000020ac9a87f5594be208f8532db38cff670c450ed2fea8fcdefcc9a663f78bab962b0065cd1d";

constexpr uint64_t kAmounts[] = {
    420000000, 462000000, 294000000, 504000000, 630000000,
    378000000, 672000000, 546000000, 588000000,
};

constexpr const char* kScriptPubKeys[] = {
    "512053a1f6e454df1aa2776a2814a721372d6258050de330b3c6d10ee8f4e0dda343",
    "5120147c9c57132f6e7ecddba9800bb0c4449251c92a1e60371ee77557b6620f3ea3",
    "76a914751e76e8199196d454941c45d1b3a323f1433bd688ac",
    "5120e4d810fd50586274face62b8a807eb9719cef49c04177cc6b76a9a4251d5450e",
    "512091b64d5324723a985170e4dc5a0f84c041804f2cd12660fa5dec09fc21783605",
    "00147dd65592d0ab2fe0d0257d571abf032cd9db93dc",
    "512075169f4001aa68f15bbed28b218df1d0a62cbbcf1188c6665110c293c907b831",
    "5120712447206d7a5238acc7ff53fbe94a3b64539ad291c7cdbc490b7577e4b17df5",
    "512077e30a5522dd9f894c3f8b8bd4c4b2cf82ca7da8a3ea6a239655c39c050ab220",
};

constexpr uint32_t kSighashInput = 0;
constexpr uint8_t kSighashType = 3;
constexpr char kSighashExpected[] =
    "2514a6272f85cfa0f45eb907fcb0d121b808ed37c6ea160a5a9046ed5526d555";

int fail(const std::string& what) {
    std::fprintf(stderr, "K1 FAIL: %s\n", what.c_str());
    return 1;
}

// Runs scriptSig/scriptPubKey with the given flags; true on success.
bool RunScript(const std::vector<uint8_t>& sig, const std::vector<uint8_t>& spk,
               uint32_t flags, ScriptError& err) {
    Transaction tx;
    tx.version = 1;
    tx.lockTime = 0;
    tx.witness_version = 0xFF;
    dinero::TxInput in;
    in.prevout.vout = 0;
    in.scriptSig = sig;
    in.sequence = 0xFFFFFFFF;
    tx.vin.push_back(in);
    dinero::TxOutput out;
    out.value = AmountUna::Zero();
    tx.vout.push_back(out);
    ScriptExecutionContext ctx(&tx, 0, 0, flags);
    err = ScriptError::OK;
    return VerifyScript(Script(sig), Script(spk), {}, ctx, err);
}

int CheckScriptVector() {
    const uint32_t flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC;
    ScriptError err;
    // "DEPTH 0 EQUAL" -> OP_DEPTH OP_0 OP_EQUAL; expected "OK".
    if (!RunScript({}, {0x74, 0x00, 0x87}, flags, err))
        return fail(std::string("script vector DEPTH 0 EQUAL: ") + ScriptErrorString(err));
    // Negative control: "DEPTH 1 EQUAL" must not verify.
    if (RunScript({}, {0x74, 0x51, 0x87}, flags, err))
        return fail("negative control DEPTH 1 EQUAL verified");
    return 0;
}

int CheckSighashVector() {
    Transaction tx;
    std::string error;
    if (!TransactionParser::ParseTransaction(kUnsignedTx, tx, error))
        return fail("parse BIP341 tx: " + error);
    if (tx.vin.size() != std::size(kAmounts)) return fail("BIP341 tx input count");
    std::vector<uint64_t> amounts(std::begin(kAmounts), std::end(kAmounts));
    std::vector<std::vector<uint8_t>> scripts;
    for (const char* hex : kScriptPubKeys) scripts.push_back(TransactionSerializer::FromHex(hex));
    const std::vector<uint8_t> confidential_flags(tx.vin.size(), 0);
    const std::vector<std::vector<uint8_t>> commitments(tx.vin.size());
    ScriptExecutionContext context(&tx, kSighashInput, amounts[kSighashInput],
                                   SCRIPT_VERIFY_TAPROOT, amounts, scripts,
                                   confidential_flags, commitments);
    const std::string got = ToHex(SignatureHashTaproot(context, kSighashType, {}, {}));
    if (got != kSighashExpected)
        return fail("BIP341 sighash: got " + got + " want " + kSighashExpected);
    return 0;
}

}  // namespace

int main() {
    try {
        const dinero::ChainParams& params = dinero::Params();
        if (params.name.empty() || params.genesis.genesisHashHex.empty())
            return fail("Params() returned empty chain parameters");
        if (int rc = CheckScriptVector()) return rc;
        if (int rc = CheckSighashVector()) return rc;
    } catch (const std::exception& e) {
        return fail(std::string("exception: ") + e.what());
    }
    std::printf("K1 OK: Params, script vector, BIP341 sighash vector\n");
    return 0;
}
