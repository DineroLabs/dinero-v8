#include "consensus/covenants.h"
#include "consensus/script.h"
#include "consensus/script_interpreter.h"
#include "primitives/transaction.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using dinero::AmountUna;
using dinero::Transaction;
using dinero::TxId;
using dinero::TxInput;
using dinero::TxOutput;
using dinero::consensus::ContractSpendContext;
using dinero::consensus::ContractState;
using dinero::consensus::PrecomputedTransactionData;
using dinero::consensus::ScriptExecutionContext;
using dinero::consensus::UTXOEntry;

volatile uint8_t benchmark_sink = 0;

Transaction BuildTransparentTransaction(size_t count) {
    Transaction tx;
    tx.vin.reserve(count);
    tx.vout.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        TxInput input;
        dinero::uint256 txid;
        txid.data[0] = static_cast<uint8_t>(index);
        txid.data[1] = static_cast<uint8_t>(index >> 8);
        input.prevout.txid = TxId(txid);
        input.prevout.vout = static_cast<uint32_t>(index);
        input.sequence = 0xfffffffeU - static_cast<uint32_t>(index);
        tx.vin.push_back(std::move(input));

        tx.vout.emplace_back(
            AmountUna::Una(1'000 + index),
            std::vector<uint8_t>{
                dinero::consensus::OP_TRUE,
                static_cast<uint8_t>(index)});
    }
    return tx;
}

uint64_t BenchmarkCtvDirect(const Transaction& tx, size_t rounds) {
    const auto start = Clock::now();
    for (size_t round = 0; round < rounds; ++round) {
        for (uint32_t input = 0; input < tx.vin.size(); ++input) {
            std::array<uint8_t, 32> hash{};
            if (!dinero::consensus::TryComputeCTVHash(tx, input, hash)) {
                std::abort();
            }
            benchmark_sink ^= hash[0];
        }
    }
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start)
            .count());
}

uint64_t BenchmarkCtvPrecomputed(const Transaction& tx, size_t rounds) {
    const auto start = Clock::now();
    for (size_t round = 0; round < rounds; ++round) {
        const PrecomputedTransactionData precomputed(tx);
        for (uint32_t input = 0; input < tx.vin.size(); ++input) {
            std::array<uint8_t, 32> hash{};
            if (!precomputed.TryComputeCTVHash(input, hash)) {
                std::abort();
            }
            benchmark_sink ^= hash[0];
        }
    }
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start)
            .count());
}

std::vector<UTXOEntry> BuildInputUtxos(const Transaction& tx) {
    std::vector<UTXOEntry> input_utxos;
    input_utxos.reserve(tx.vin.size());
    for (size_t index = 0; index < tx.vin.size(); ++index) {
        input_utxos.emplace_back(
            AmountUna::Una(2'000 + index),
            std::vector<uint8_t>{dinero::consensus::OP_TRUE},
            1, false);
    }
    return input_utxos;
}

uint64_t BenchmarkTaprootSighash(
    const Transaction& tx,
    const std::vector<UTXOEntry>& input_utxos,
    size_t rounds,
    bool use_precomputation) {
    std::vector<uint64_t> amounts;
    std::vector<std::vector<uint8_t>> scripts;
    std::vector<uint8_t> confidential_flags;
    std::vector<std::vector<uint8_t>> commitments;
    for (const auto& spent : input_utxos) {
        amounts.push_back(spent.value.GetUna());
        scripts.push_back(spent.scriptPubKey);
        confidential_flags.push_back(spent.is_confidential ? 1 : 0);
        commitments.push_back(spent.commitment);
    }

    const auto start = Clock::now();
    for (size_t round = 0; round < rounds; ++round) {
        std::unique_ptr<PrecomputedTransactionData> precomputed;
        if (use_precomputation) {
            precomputed =
                std::make_unique<PrecomputedTransactionData>(
                    tx, input_utxos);
        }
        for (uint32_t input = 0; input < tx.vin.size(); ++input) {
            ScriptExecutionContext context(
                &tx, input, amounts[input],
                dinero::consensus::SCRIPT_VERIFY_TAPROOT);
            if (use_precomputation) {
                context.covenant_precomputed = precomputed.get();
            } else {
                context.all_amounts = amounts;
                context.all_scriptpubkeys = scripts;
                context.all_confidential_flags = confidential_flags;
                context.all_input_commitments = commitments;
            }
            const std::vector<uint8_t> hash =
                dinero::consensus::SignatureHashTaproot(
                    context, 0, {}, {});
            if (hash.size() != 32) {
                std::abort();
            }
            benchmark_sink ^= hash[0];
        }
    }
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start)
            .count());
}

struct CcvCase {
    Transaction tx;
    ContractState previous;
    ContractState next;
    std::vector<uint8_t> script;
    std::array<uint8_t, 32> internal_key{};
    std::array<uint8_t, 32> merkle_root{};
    uint8_t parity{0};
    std::vector<UTXOEntry> inputs;
};

CcvCase BuildCcvCase(size_t output_count) {
    CcvCase test_case;
    test_case.script = {
        static_cast<uint8_t>(
            dinero::consensus::OP_CHECKCONTRACTVERIFY),
        static_cast<uint8_t>(dinero::consensus::OP_TRUE)};
    test_case.previous.codeHash =
        dinero::consensus::ComputeContractCodeHash(test_case.script);
    test_case.previous.counter = 1;
    test_case.previous.data = {0x01};
    test_case.previous.stateHash =
        dinero::consensus::ComputeContractStateHash(test_case.previous);
    test_case.next.codeHash = test_case.previous.codeHash;
    test_case.next.counter = 2;
    test_case.next.data = {0x02};
    test_case.next.stateHash =
        dinero::consensus::ComputeContractStateHash(test_case.next);
    test_case.merkle_root.fill(0x42);

    if (!dinero::consensus::DeriveContractInternalKey(
            test_case.previous, test_case.internal_key)) {
        std::abort();
    }
    std::vector<uint8_t> current_script;
    if (!dinero::consensus::ComputeContractOutputScript(
            test_case.previous, test_case.merkle_root,
            current_script, &test_case.parity)) {
        std::abort();
    }
    std::vector<uint8_t> successor_script;
    if (!dinero::consensus::ComputeContractOutputScript(
            test_case.next, test_case.merkle_root, successor_script)) {
        std::abort();
    }

    test_case.tx.vin.emplace_back();
    test_case.tx.vout.emplace_back(
        AmountUna::Una(50'000), successor_script);
    for (size_t index = 1; index < output_count; ++index) {
        test_case.tx.vout.emplace_back(
            AmountUna::Una(1),
            std::vector<uint8_t>{
                dinero::consensus::OP_TRUE,
                static_cast<uint8_t>(index)});
    }
    test_case.inputs.emplace_back(
        AmountUna::Una(50'000), current_script, 1, false);
    return test_case;
}

uint64_t BenchmarkCcv(const CcvCase& test_case, size_t rounds) {
    const ContractSpendContext context{
        test_case.inputs,
        test_case.script,
        test_case.internal_key,
        test_case.merkle_root,
        test_case.parity};
    const auto start = Clock::now();
    for (size_t round = 0; round < rounds; ++round) {
        const bool valid = dinero::consensus::VerifyContractTransition(
            test_case.tx, 0, test_case.previous, test_case.next, context);
        if (!valid) {
            std::abort();
        }
        benchmark_sink ^= static_cast<uint8_t>(valid);
    }
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start)
            .count());
}

} // namespace

int main() {
    std::cout << "{\n  \"ctv\": [\n";
    // 1,800 inputs produces a ~95 KiB transaction, close to the 100,000-byte
    // consensus ceiling while leaving room for encoding-size variation.
    const std::array<size_t, 6> ctv_counts{
        1, 64, 256, 512, 1'024, 1'800};
    const std::array<size_t, 6> ctv_rounds{2'000, 300, 60, 20, 5, 2};
    for (size_t index = 0; index < ctv_counts.size(); ++index) {
        const Transaction tx =
            BuildTransparentTransaction(ctv_counts[index]);
        const uint64_t direct =
            BenchmarkCtvDirect(tx, ctv_rounds[index]);
        const uint64_t precomputed =
            BenchmarkCtvPrecomputed(tx, ctv_rounds[index]);
        std::cout
            << "    {\"inputs\":" << ctv_counts[index]
            << ",\"tx_bytes\":" << tx.GetSize()
            << ",\"rounds\":" << ctv_rounds[index]
            << ",\"direct_us_per_tx\":"
            << static_cast<double>(direct) / ctv_rounds[index]
            << ",\"precomputed_us_per_tx\":"
            << static_cast<double>(precomputed) / ctv_rounds[index]
            << "}"
            << (index + 1 == ctv_counts.size() ? "\n" : ",\n");
    }

    std::cout << "  ],\n  \"taproot_sighash\": [\n";
    const std::array<size_t, 6> sighash_counts{
        1, 64, 256, 512, 1'024, 1'800};
    const std::array<size_t, 6> sighash_rounds{2'000, 200, 30, 10, 3, 1};
    for (size_t index = 0; index < sighash_counts.size(); ++index) {
        const Transaction tx =
            BuildTransparentTransaction(sighash_counts[index]);
        const std::vector<UTXOEntry> input_utxos = BuildInputUtxos(tx);
        const uint64_t direct = BenchmarkTaprootSighash(
            tx, input_utxos, sighash_rounds[index], false);
        const uint64_t precomputed = BenchmarkTaprootSighash(
            tx, input_utxos, sighash_rounds[index], true);
        std::cout
            << "    {\"inputs\":" << sighash_counts[index]
            << ",\"tx_bytes\":" << tx.GetSize()
            << ",\"rounds\":" << sighash_rounds[index]
            << ",\"direct_us_per_tx\":"
            << static_cast<double>(direct) / sighash_rounds[index]
            << ",\"precomputed_us_per_tx\":"
            << static_cast<double>(precomputed) / sighash_rounds[index]
            << "}"
            << (index + 1 == sighash_counts.size() ? "\n" : ",\n");
    }

    std::cout << "  ],\n  \"ccv\": [\n";
    const std::array<size_t, 5> ccv_outputs{1, 64, 512, 4'096, 8'192};
    const std::array<size_t, 5> ccv_rounds{1'000, 500, 100, 25, 10};
    for (size_t index = 0; index < ccv_outputs.size(); ++index) {
        const CcvCase test_case = BuildCcvCase(ccv_outputs[index]);
        const uint64_t elapsed =
            BenchmarkCcv(test_case, ccv_rounds[index]);
        std::cout
            << "    {\"outputs\":" << ccv_outputs[index]
            << ",\"rounds\":" << ccv_rounds[index]
            << ",\"us_per_transition\":"
            << static_cast<double>(elapsed) / ccv_rounds[index]
            << "}"
            << (index + 1 == ccv_outputs.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
    (void)benchmark_sink;
    return 0;
}
