#include "wallet/coin_selection.h"
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <functional>  // For std::function

namespace dinero {

namespace {

// Phase 10: count P2MR (witness v3, 0x53 0x20 || 32-byte merkle_root) inputs
// in a selection so the fee estimator sizes their witness properly.
size_t CountP2MRInputs(const std::vector<CanonicalWalletUTXO>& coins) {
    size_t n = 0;
    for (const auto& c : coins) {
        if (c.spk.size() == 34 && c.spk[0] == 0x53 && c.spk[1] == 0x20) {
            ++n;
        }
    }
    return n;
}

// Worst-case initial estimate: assume the single probe input is P2MR so
// the estimator reserves enough fee room for coin selection to pick a
// P2MR coin on the first pass. False positives cost a few hundred una of
// over-estimation on pure-taproot wallets; false negatives cost a
// rejected submission (min-feerate), which is much worse.
size_t InitialP2MRProbe(const std::vector<CanonicalWalletUTXO>& utxos) {
    for (const auto& u : utxos) {
        if (u.spk.size() == 34 && u.spk[0] == 0x53 && u.spk[1] == 0x20) {
            return 1;
        }
    }
    return 0;
}

} // namespace

CoinSelectionResult CoinSelector::SelectCoins(
    const std::vector<CanonicalWalletUTXO>& available_utxos,
    uint64_t target_amount,
    uint64_t fee_rate,
    size_t num_outputs
) {
    CoinSelectionResult result;

    if (available_utxos.empty()) {
        result.error = "No UTXOs available";
        return result;
    }

    // Milestone 12.3: Apply privacy heuristics (group by path, mix ages)
    std::vector<CanonicalWalletUTXO> privacy_sorted = ApplyPrivacyHeuristics(available_utxos);

    // Estimate transaction size (will adjust as we select coins).
    // If any candidate UTXO is P2MR, probe with a 1-P2MR-input estimate so
    // the first-pass fee buffer isn't five orders of magnitude low.
    const size_t probe_p2mr_inputs = InitialP2MRProbe(privacy_sorted);
    size_t estimated_size = EstimateTransactionSize(1, num_outputs, probe_p2mr_inputs);
    uint64_t estimated_fee = CalculateFee(estimated_size, fee_rate);

    // Milestone 12.3: Try Branch-and-Bound first (exact match, no change output)
    // This is better for privacy and reduces fees.
    result = SelectCoinsBnB(privacy_sorted, target_amount, estimated_fee);

    if (result.success) {
        // BnB found exact match! No change output needed.
        std::cout << "INFO: BnB found exact match - no change output (better privacy)" << std::endl;
        // Phase 10: recompute fee against the actual selection so the caller
        // gets a fee that matches the real (P2MR-aware) serialized size.
        const size_t p2mr_inputs = CountP2MRInputs(result.selected_coins);
        const size_t actual_size = EstimateTransactionSize(
            result.selected_coins.size(), num_outputs, p2mr_inputs);
        result.fee = CalculateFee(actual_size, fee_rate);
    } else {
        // BnB failed, fall back to least-waste selection. This prefers the
        // smallest sufficient value instead of consuming a much larger coin
        // when smaller confirmed coins can cover the payment.
        result = SelectCoinsLeastWaste(privacy_sorted, target_amount, estimated_fee);

        if (!result.success) {
            return result;
        }

        // Recalculate fee with actual number of inputs
        size_t p2mr_inputs = CountP2MRInputs(result.selected_coins);
        size_t actual_size = EstimateTransactionSize(
            result.selected_coins.size(), num_outputs, p2mr_inputs);
        uint64_t actual_fee = CalculateFee(actual_size, fee_rate);

        // Check if we need to select more coins to cover the higher fee
        uint64_t total_needed = target_amount + actual_fee;
        if (result.total_value < total_needed) {
            // Try again with updated fee
            result = SelectCoinsLeastWaste(privacy_sorted, target_amount, actual_fee);
            if (!result.success) {
                return result;
            }

            // Recalculate one more time with final input count
            p2mr_inputs = CountP2MRInputs(result.selected_coins);
            actual_size = EstimateTransactionSize(
                result.selected_coins.size(), num_outputs, p2mr_inputs);
            actual_fee = CalculateFee(actual_size, fee_rate);
        }

        result.fee = actual_fee;
        result.change_amount = result.total_value - target_amount - actual_fee;

        // Check if change is dust
        if (result.change_amount > 0 && result.change_amount < DUST_THRESHOLD) {
            // Add dust to fee instead of creating dust output
            result.fee += result.change_amount;
            result.change_amount = 0;
        }
    }

    std::cout << "INFO: Coin selection - selected " << result.selected_coins.size()
              << " inputs, total: " << result.total_value
              << ", fee: " << result.fee
              << ", change: " << result.change_amount << std::endl;

    return result;
}

CoinSelectionResult CoinSelector::SelectCoinsLeastWaste(
    const std::vector<CanonicalWalletUTXO>& available_utxos,
    uint64_t target_amount,
    uint64_t estimated_fee
) {
    CoinSelectionResult result;

    uint64_t total_needed = target_amount + estimated_fee;
    std::vector<CanonicalWalletUTXO> sorted_utxos = available_utxos;

    // Sort ascending by value. For equal values, prefer Taproot (cheaper
    // to spend) over P2MR (preserve as PQ cold reserve). This is the
    // spend-preference tiebreaker: rational coin selection spends the
    // cheapest-to-sign input first.
    auto is_p2mr = [](const CanonicalWalletUTXO& u) {
        return u.spk.size() == 34 && u.spk[0] == 0x53 && u.spk[1] == 0x20;
    };
    std::sort(sorted_utxos.begin(), sorted_utxos.end(),
        [&](const CanonicalWalletUTXO& a, const CanonicalWalletUTXO& b) {
            const auto a_value = a.value.GetUna();
            const auto b_value = b.value.GetUna();
            if (a_value != b_value) {
                return a_value < b_value;
            }
            // Equal value: Taproot before P2MR (cheaper to spend).
            const bool a_pq = is_p2mr(a);
            const bool b_pq = is_p2mr(b);
            if (a_pq != b_pq) {
                return !a_pq;  // non-P2MR (Taproot) sorts first
            }
            if (a.path != b.path) {
                return a.path < b.path;
            }
            if (a.txid != b.txid) {
                return a.txid < b.txid;
            }
            return a.vout < b.vout;
        });

    uint64_t available_total = 0;
    for (const auto& utxo : sorted_utxos) {
        available_total += utxo.value.GetUna();
    }
    if (available_total < total_needed) {
        result.error = "Insufficient funds: have " + std::to_string(available_total) +
                       ", need " + std::to_string(total_needed);
        return result;
    }

    // For large sets, fall back to a deterministic ascending accumulation
    // instead of exploring an exponential search space.
    if (sorted_utxos.size() > 256) {
        uint64_t accumulated = 0;
        for (const auto& utxo : sorted_utxos) {
            result.selected_coins.push_back(utxo);
            accumulated += utxo.value.GetUna();
            if (accumulated >= total_needed) {
                result.success = true;
                result.total_value = accumulated;
                return result;
            }
        }
    }

    std::vector<uint64_t> suffix_sum(sorted_utxos.size() + 1, 0);
    for (size_t i = sorted_utxos.size(); i > 0; --i) {
        suffix_sum[i - 1] = suffix_sum[i] + sorted_utxos[i - 1].value.GetUna();
    }

    std::vector<size_t> best_selection;
    uint64_t best_total = 0;
    size_t best_inputs = 0;
    size_t tries = 0;

    std::function<void(uint64_t, size_t, std::vector<size_t>&)> search;
    search = [&](uint64_t current_total, size_t index, std::vector<size_t>& selection) {
        if (++tries > LEAST_WASTE_MAX_TRIES) {
            return;
        }

        if (current_total >= total_needed) {
            if (best_selection.empty() ||
                current_total < best_total ||
                (current_total == best_total && selection.size() < best_inputs)) {
                best_selection = selection;
                best_total = current_total;
                best_inputs = selection.size();
            }
            return;
        }

        if (index >= sorted_utxos.size()) {
            return;
        }

        if (current_total + suffix_sum[index] < total_needed) {
            return;
        }

        if (!best_selection.empty() && current_total > best_total) {
            return;
        }

        for (size_t i = index; i < sorted_utxos.size(); ++i) {
            const auto candidate_total = current_total + sorted_utxos[i].value.GetUna();
            if (!best_selection.empty() && candidate_total > best_total) {
                break;
            }

            selection.push_back(i);
            search(candidate_total, i + 1, selection);
            selection.pop_back();
        }
    };

    std::vector<size_t> current_selection;
    search(0, 0, current_selection);

    if (best_selection.empty()) {
        result.error = "Insufficient funds: have " + std::to_string(available_total) +
                       ", need " + std::to_string(total_needed);
        return result;
    }

    result.success = true;
    result.total_value = best_total;
    for (size_t idx : best_selection) {
        result.selected_coins.push_back(sorted_utxos[idx]);
    }

    std::cout << "INFO: Least-waste selection after " << tries << " tries, "
              << result.selected_coins.size() << " inputs, total: " << best_total
              << std::endl;
    return result;
}

size_t CoinSelector::EstimateTransactionSize(size_t num_inputs,
                                              size_t num_outputs,
                                              size_t num_p2mr_inputs) {
    // Taproot (P2TR) / P2MR transaction size estimation.
    // Matches the mempool's BIP141 vsize metric (Transaction::GetVirtualSize()):
    // vsize = (base_size * 3 + total_size + 3) / 4
    // i.e. witness bytes get a 4× discount, non-witness bytes count fully.
    //
    // Serialized component sizes:
    //   Base: version(4) + marker(1) + flag(1) + input_count(1) + output_count(1) + locktime(4) = 12
    //   Per input non-witness: txid(32) + vout(4) + scriptSig_len(1) + sequence(4) = 41
    //   Per input witness (P2TR): stack_items(1) + sig_len(1) + schnorr_sig(64) = 66
    //   Per input witness (P2MR, ML-DSA-65): scheme(1) + pubkey_len(3) + pubkey(1952)
    //                                        + sig_len(3) + sig(3309) + depth(1) + leaf(1)
    //                                        + stack_items(1) + blob_len(3) ≈ 5274
    //   Per output: value(8) + spk_len(1) + P2TR_spk(34) = 43

    if (num_p2mr_inputs > num_inputs) {
        num_p2mr_inputs = num_inputs;
    }
    const size_t num_p2tr_inputs = num_inputs - num_p2mr_inputs;

    // Non-witness portion (counted at 4× weight per BIP141).
    // Note: marker+flag bytes only appear in witness-inclusive form, so we
    // count base_size as version(4) + input_count(1) + output_count(1) +
    // locktime(4) = 10 for weight purposes.
    size_t base_size = 10;
    base_size += num_inputs * 41;
    base_size += num_outputs * 43;

    // Per-input witness bytes (counted at 1× weight per BIP141).
    // P2MR (post-quantum) carries a ~5.3KB witness; the 4× discount keeps
    // a P2MR spend tractable instead of pricing it like a 5KB legacy tx.
    constexpr size_t P2TR_WITNESS_BYTES = 66;
    constexpr size_t P2MR_WITNESS_BYTES = 5274;

    // Marker(1) + flag(1) are part of the witness-inclusive serialization
    // and only appear when the tx has witness data, which is always true for
    // the v7 tx shapes we estimate.
    size_t witness_size = 2 + num_p2tr_inputs * P2TR_WITNESS_BYTES
                        + num_p2mr_inputs * P2MR_WITNESS_BYTES;

    // BIP141 weight = base * 3 + total = base*3 + (base + witness)
    //               = base * 4 + witness
    // vsize = (weight + 3) / 4  (ceiling division)
    size_t weight = base_size * 4 + witness_size;
    return (weight + 3) / 4;
}

uint64_t CoinSelector::CalculateFee(size_t tx_size, uint64_t fee_rate) {
    return tx_size * fee_rate;
}

// ═══════════════════════════════════════════════════════════════════════════
// Milestone 12.3: Branch-and-Bound Coin Selection
// ═══════════════════════════════════════════════════════════════════════════

CoinSelectionResult CoinSelector::SelectCoinsBnB(
    const std::vector<CanonicalWalletUTXO>& available_utxos,
    uint64_t target_amount,
    uint64_t estimated_fee
) {
    CoinSelectionResult result;
    uint64_t target = target_amount + estimated_fee;

    // Skip BnB for large UTXO sets — recursion depth equals UTXO count
    // and will overflow the thread stack (default 512 KB).
    // Bitcoin Core uses a similar cap. Fall through to greedy/knapsack.
    if (available_utxos.size() > 500) {
        result.success = false;
        return result;
    }

    // Sort UTXOs by descending value for better BnB performance
    std::vector<CanonicalWalletUTXO> sorted_utxos = available_utxos;
    std::sort(sorted_utxos.begin(), sorted_utxos.end(),
        [](const CanonicalWalletUTXO& a, const CanonicalWalletUTXO& b) {
            return a.value > b.value;
        });

    // BnB recursive backtracking
    // State: (current_value, index, selected_indices)
    std::vector<size_t> best_selection;
    size_t tries = 0;

    // Lambda for recursive backtracking
    std::function<bool(uint64_t, size_t, std::vector<size_t>&)> search;
    search = [&](uint64_t current_value, size_t index, std::vector<size_t>& selection) -> bool {
        // Prevent exponential blowup
        if (++tries > BNB_MAX_TRIES) {
            return false;
        }

        // Exact match found!
        if (current_value == target) {
            best_selection = selection;
            return true;
        }

        // Exceeded target (backtrack)
        if (current_value > target) {
            return false;
        }

        // No more UTXOs to try
        if (index >= sorted_utxos.size()) {
            return false;
        }

        // Branch 1: Include current UTXO
        selection.push_back(index);
        // Phase M.6.2: Extract raw value for arithmetic (will use checked arithmetic in M.6.3)
        if (search(current_value + sorted_utxos[index].value.GetUna(), index + 1, selection)) {
            return true;
        }
        selection.pop_back();

        // Branch 2: Exclude current UTXO
        if (search(current_value, index + 1, selection)) {
            return true;
        }

        return false;
    };

    std::vector<size_t> current_selection;
    if (search(0, 0, current_selection)) {
        // BnB found exact match!
        result.success = true;
        result.total_value = 0;

        for (size_t idx : best_selection) {
            result.selected_coins.push_back(sorted_utxos[idx]);
            // Phase M.6.2: Extract raw value for accumulation (will use checked arithmetic in M.6.3)
            result.total_value += sorted_utxos[idx].value.GetUna();
        }

        result.fee = estimated_fee;
        result.change_amount = 0;  // Exact match, no change

        std::cout << "INFO: BnB success after " << tries << " tries, "
                  << result.selected_coins.size() << " inputs" << std::endl;
    } else {
        // BnB failed (no exact match or too many tries)
        result.success = false;
        std::cout << "INFO: BnB failed after " << tries << " tries, falling back to least-waste search" << std::endl;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Milestone 12.3: Privacy-Aware Coin Selection
// ═══════════════════════════════════════════════════════════════════════════

std::vector<CanonicalWalletUTXO> CoinSelector::ApplyPrivacyHeuristics(
    const std::vector<CanonicalWalletUTXO>& utxos
) {
    if (utxos.empty()) {
        return utxos;
    }

    // Group UTXOs by derivation path (Phase M.3: path instead of address)
    std::unordered_map<std::string, std::vector<CanonicalWalletUTXO>> by_path;
    for (const auto& utxo : utxos) {
        by_path[utxo.path].push_back(utxo);
    }

    // Privacy heuristic 1: Prefer same-path UTXOs
    // UTXOs from the same derivation path belong to the same address
    // Grouping them prevents linking unrelated addresses

    // Privacy heuristic 2: Mix UTXO ages
    // Avoid only selecting old or only new UTXOs (timing analysis)

    std::vector<CanonicalWalletUTXO> result;

    // First, add UTXOs grouped by path (most frequent path first)
    std::vector<std::pair<std::string, std::vector<CanonicalWalletUTXO>>> sorted_by_count;
    for (const auto& pair : by_path) {
        sorted_by_count.push_back(pair);
    }

    std::sort(sorted_by_count.begin(), sorted_by_count.end(),
        [](const auto& a, const auto& b) {
            return a.second.size() > b.second.size();
        });

    // Add UTXOs in round-robin from each path group (age mixing)
    size_t max_utxos_per_path = 0;
    for (const auto& pair : sorted_by_count) {
        max_utxos_per_path = std::max(max_utxos_per_path, pair.second.size());
    }

    for (size_t i = 0; i < max_utxos_per_path; ++i) {
        for (const auto& pair : sorted_by_count) {
            if (i < pair.second.size()) {
                result.push_back(pair.second[i]);
            }
        }
    }

    return result;
}

} // namespace dinero
