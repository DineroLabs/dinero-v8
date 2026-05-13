/**
 * Sighash fuzz harness (current API)
 *
 * Targets:
 * - SignatureHashLegacy()
 * - SignatureHashWitness()
 * - SignatureHashTaproot()
 * - SighashBIP143::ComputeSighash()
 */

#include "consensus/crypto/sighash_bip143.h"
#include "consensus/script_interpreter.h"
#include "primitives/transaction.h"

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

namespace {

class Cursor {
public:
    Cursor(const uint8_t* data, size_t size) : data_(data), size_(size), pos_(0) {}

    uint8_t TakeByte() {
        if (pos_ < size_) {
            return data_[pos_++];
        }
        return 0;
    }

    uint32_t TakeU32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(TakeByte()) << (8 * i);
        }
        return v;
    }

    uint64_t TakeU64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(TakeByte()) << (8 * i);
        }
        return v;
    }

    std::vector<uint8_t> TakeBytes(size_t n) {
        std::vector<uint8_t> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            out.push_back(TakeByte());
        }
        return out;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_;
};

static uint8_t PickSigHashType(uint8_t selector) {
    // Mix valid and invalid values intentionally.
    static constexpr uint8_t kTypes[] = {
        0x00,  // Taproot default / legacy invalid edge
        SIGHASH_ALL,
        SIGHASH_NONE,
        SIGHASH_SINGLE,
        static_cast<uint8_t>(SIGHASH_ALL | SIGHASH_ANYONECANPAY),
        static_cast<uint8_t>(SIGHASH_NONE | SIGHASH_ANYONECANPAY),
        static_cast<uint8_t>(SIGHASH_SINGLE | SIGHASH_ANYONECANPAY),
        0x84,  // reserved bits
        0xFF   // clearly invalid
    };
    return kTypes[selector % (sizeof(kTypes) / sizeof(kTypes[0]))];
}

static Script BuildScript(Cursor& c, size_t max_len) {
    const size_t len = static_cast<size_t>(c.TakeByte()) % (max_len + 1);
    return Script(c.TakeBytes(len));
}

static Transaction BuildTransaction(Cursor& c, uint8_t mode) {
    Transaction tx;
    tx.version = static_cast<int32_t>(c.TakeU32());
    tx.lockTime = c.TakeU32();

    const size_t input_count = 1 + (static_cast<size_t>(c.TakeByte()) % 4);
    const size_t output_count = 1 + (static_cast<size_t>(c.TakeByte()) % 4);

    for (size_t i = 0; i < input_count; ++i) {
        TxInput in;

        for (int j = 0; j < 32; ++j) {
            in.prevout.txid.v.data[j] = c.TakeByte();
        }
        in.prevout.vout = c.TakeU32();

        const size_t script_sig_len = static_cast<size_t>(c.TakeByte()) % 96;
        in.scriptSig = c.TakeBytes(script_sig_len);
        in.sequence = c.TakeU32();

        if ((mode & 0x08) != 0) {
            const size_t witness_items = static_cast<size_t>(c.TakeByte()) % 4;
            in.witness.reserve(witness_items);
            for (size_t w = 0; w < witness_items; ++w) {
                const size_t witness_len = static_cast<size_t>(c.TakeByte()) % 96;
                in.witness.push_back(c.TakeBytes(witness_len));
            }
        }

        tx.vin.push_back(std::move(in));
    }

    for (size_t i = 0; i < output_count; ++i) {
        TxOutput out;
        const uint64_t raw = c.TakeU64() % (MAX_SUPPLY_UNA_CONST + 1ULL);
        out.value = AmountUna::UnsafeFromRaw(raw);
        const size_t spk_len = static_cast<size_t>(c.TakeByte()) % 96;
        out.scriptPubKey = c.TakeBytes(spk_len);
        tx.vout.push_back(std::move(out));
    }

    tx.witness_version = ((mode & 0x08) != 0) ? 0 : 0xFF;
    return tx;
}

static std::vector<uint64_t> BuildAllAmounts(size_t count, Cursor& c) {
    std::vector<uint64_t> amounts;
    amounts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        amounts.push_back(c.TakeU64() % (MAX_SUPPLY_UNA_CONST + 1ULL));
    }
    return amounts;
}

static std::vector<std::vector<uint8_t>> BuildAllScripts(size_t count, Cursor& c) {
    std::vector<std::vector<uint8_t>> scripts;
    scripts.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t len = static_cast<size_t>(c.TakeByte()) % 96;
        scripts.push_back(c.TakeBytes(len));
    }
    return scripts;
}

static std::vector<uint8_t> BuildLeafHash(Cursor& c, uint8_t mode) {
    if ((mode & 0x20) == 0) {
        return {};
    }
    if ((c.TakeByte() & 1U) != 0) {
        return {};
    }
    return c.TakeBytes(32);
}

static std::vector<uint8_t> BuildAnnex(Cursor& c, uint8_t mode) {
    if ((mode & 0x40) == 0) {
        return {};
    }
    const size_t len = 1 + (static_cast<size_t>(c.TakeByte()) % 32);
    std::vector<uint8_t> annex = c.TakeBytes(len);
    // BIP341 annex starts with 0x50 when present.
    annex[0] = 0x50;
    return annex;
}

static Script DefaultScriptCode() {
    const std::vector<uint8_t> p2pkh = {
        static_cast<uint8_t>(OP_DUP),
        static_cast<uint8_t>(OP_HASH160),
        0x14,  // push 20
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        static_cast<uint8_t>(OP_EQUALVERIFY),
        static_cast<uint8_t>(OP_CHECKSIG)
    };
    return Script(p2pkh);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) {
        return 0;
    }

    Cursor c(data, size);
    const uint8_t mode = c.TakeByte();
    const uint8_t hash_type = PickSigHashType(c.TakeByte());

    Transaction tx = BuildTransaction(c, mode);
    if (tx.vin.empty()) {
        return 0;
    }

    const uint32_t input_index =
        static_cast<uint32_t>(c.TakeByte() % static_cast<uint8_t>(tx.vin.size()));
    const uint64_t input_amount = c.TakeU64() % (MAX_SUPPLY_UNA_CONST + 1ULL);

    Script script_code = BuildScript(c, 96);
    if (script_code.empty()) {
        script_code = DefaultScriptCode();
    }

    const std::vector<uint64_t> all_amounts = BuildAllAmounts(tx.vin.size(), c);
    const std::vector<std::vector<uint8_t>> all_scripts = BuildAllScripts(tx.vin.size(), c);

    ScriptExecutionContext base_ctx(&tx, input_index, input_amount, SCRIPT_VERIFY_STANDARD);
    ScriptExecutionContext tap_ctx(
        &tx,
        input_index,
        input_amount,
        SCRIPT_VERIFY_STANDARD | SCRIPT_VERIFY_TAPROOT,
        all_amounts,
        all_scripts
    );

    const std::vector<uint8_t> h_legacy = SignatureHashLegacy(script_code, base_ctx, hash_type);
    const std::vector<uint8_t> h_witness = SignatureHashWitness(script_code, base_ctx, hash_type);
    const std::vector<uint8_t> h_bip143 = SighashBIP143::ComputeSighash(
        tx, input_index, script_code.data(), input_amount, hash_type
    );
    const std::vector<uint8_t> leaf_hash = BuildLeafHash(c, mode);
    const std::vector<uint8_t> annex = BuildAnnex(c, mode);
    const std::vector<uint8_t> h_taproot =
        SignatureHashTaproot(tap_ctx, hash_type, leaf_hash, annex);

    (void)h_legacy;
    (void)h_witness;
    (void)h_bip143;
    (void)h_taproot;

    // Signature parser/verification paths.
    if ((mode & 0x01) != 0) {
        std::vector<uint8_t> sig = c.TakeBytes(static_cast<size_t>(c.TakeByte()) % 80);
        if (sig.empty()) {
            sig.push_back(hash_type);
        } else {
            sig.back() = hash_type;
        }
        std::vector<uint8_t> pubkey = c.TakeBytes(33);
        if (pubkey.size() != 33) {
            pubkey.assign(33, 0);
            pubkey[0] = 0x02;
        }
        std::vector<uint8_t> msg32 = c.TakeBytes(32);

        (void)IsValidSignatureEncoding(sig);
        (void)CheckECDSASignature(
            sig,
            pubkey,
            msg32,
            SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_LOW_S | SCRIPT_VERIFY_STRICTENC
        );
    }

    if ((mode & 0x02) != 0) {
        const size_t sig_len = 64 + (static_cast<size_t>(c.TakeByte()) % 2);
        const std::vector<uint8_t> schnorr_sig = c.TakeBytes(sig_len);
        const std::vector<uint8_t> xonly_pubkey = c.TakeBytes(32);
        const std::vector<uint8_t> sighash = (h_taproot.size() == 32) ? h_taproot : c.TakeBytes(32);
        (void)CheckSchnorrSignature(schnorr_sig, xonly_pubkey, sighash, SCRIPT_VERIFY_TAPROOT);
    }

    return 0;
}

#ifdef AFL_MAIN
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::vector<uint8_t> input;
        uint8_t buf[4096];
        ssize_t n = 0;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
            input.insert(input.end(), buf, buf + n);
        }
        return LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    for (int i = 1; i < argc; ++i) {
        const int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            continue;
        }

        struct stat st {};
        if (fstat(fd, &st) != 0 || st.st_size <= 0) {
            close(fd);
            continue;
        }

        std::vector<uint8_t> input(static_cast<size_t>(st.st_size));
        (void)read(fd, input.data(), input.size());
        close(fd);
        LLVMFuzzerTestOneInput(input.data(), input.size());
    }

    return 0;
}
#endif

