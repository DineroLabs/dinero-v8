/**
 * Phase C.3: Covenant Test Transaction Generators
 *
 * Provides utilities for creating VALID and INVALID covenant transactions
 * for integration testing across wallet → mempool → consensus boundaries.
 */

#pragma once

#include "primitives/transaction.h"
#include "wallet/covenant_builders.h"
#include "wallet/canonical_wallet_utxo.h"
#include <utility>
#include <vector>

namespace dinero {
namespace test {

/**
 * Covenant test scenario result
 * Contains both funding and spending transactions
 */
struct CovenantTestScenario {
    Transaction funding_tx;      // Transaction that creates covenant-locked UTXO
    Transaction spending_tx;     // Transaction that spends the covenant UTXO
    std::vector<uint8_t> privkey;  // Private key (for CSFS scenarios)
    std::string description;     // Human-readable description
};

/**
 * Create a valid CTV test scenario
 *
 * Phase C.3.D: CONSTRUCTION - generates valid covenant transactions
 * Creates a funding tx with CTV-locked output and matching spending tx
 *
 * Flow:
 * 1. Create CTV template (commits to specific outputs)
 * 2. Create funding tx with CTV scriptPubKey
 * 3. Create spending tx that matches the template
 *
 * Both transactions are VALID and ready for:
 * - Mempool acceptance testing
 * - Consensus validation testing
 * - Integration testing
 *
 * @return CovenantTestScenario with valid CTV transaction pair
 */
CovenantTestScenario createValidCTVScenario();

/**
 * Create an invalid CTV test scenario (template mismatch)
 *
 * Phase C.3.D: CONSTRUCTION - generates invalid covenant transactions
 * Creates a spending tx that does NOT match the CTV template
 *
 * Used to test consensus rejection of invalid covenant spends
 *
 * @return CovenantTestScenario with invalid CTV spending tx
 */
CovenantTestScenario createInvalidCTVScenario();

/**
 * Create a valid CSFS test scenario
 *
 * Phase C.3.D: CONSTRUCTION - generates valid CSFS delegation
 * Creates a funding tx with CSFS-locked output and properly signed spending tx
 *
 * Flow:
 * 1. Generate keypair
 * 2. Create CSFS delegation (message + pubkey)
 * 3. Create funding tx with CSFS scriptPubKey
 * 4. Sign the delegation message
 * 5. Create spending tx with valid signature
 *
 * @return CovenantTestScenario with valid CSFS transaction pair
 */
CovenantTestScenario createValidCSFSScenario();

/**
 * Create an invalid CSFS test scenario (bad signature)
 *
 * Phase C.3.D: CONSTRUCTION - generates invalid CSFS signature
 * Creates a spending tx with incorrect or missing signature
 *
 * Used to test consensus rejection of invalid signatures
 *
 * @return CovenantTestScenario with invalid CSFS spending tx
 */
CovenantTestScenario createInvalidCSFSScenario();

/**
 * Create a mixed covenant scenario (covenant + standard inputs)
 *
 * Phase C.3.D: Tests mixed transactions
 * Creates a tx spending both covenant-locked and standard UTXOs
 *
 * Used to test:
 * - Mempool policy handling of mixed transactions
 * - Covenant input counting
 * - Fee estimation
 *
 * @param covenant_inputs  Number of covenant inputs (default: 2)
 * @param standard_inputs  Number of standard inputs (default: 3)
 * @return CovenantTestScenario with mixed transaction
 */
CovenantTestScenario createMixedCovenantScenario(
    size_t covenant_inputs = 2,
    size_t standard_inputs = 3
);

/**
 * Create a DoS test scenario (too many covenant inputs)
 *
 * Phase C.3.D: Tests mempool DoS protection
 * Creates a tx with more covenant inputs than policy allows
 *
 * Used to test mempool rejection based on covenant input limits
 *
 * @param covenant_input_count  Number of covenant inputs (default: 15)
 * @return CovenantTestScenario with policy-violating transaction
 */
CovenantTestScenario createDoSCovenantScenario(size_t covenant_input_count = 15);

/**
 * Helper: Create a simple P2WPKH output for testing
 *
 * @param value  Output value in una
 * @return       Standard P2WPKH output
 */
wallet::CTVOutput createStandardOutput(uint64_t value);

/**
 * Helper: Create a dummy funding UTXO for testing
 *
 * @param value       UTXO value
 * @param vout        Output index (default: 0)
 * @param confirmed   Is UTXO confirmed? (default: true)
 * @return            CanonicalWalletUTXO for test usage
 */
wallet::CanonicalWalletUTXO createDummyUTXO(
    uint64_t value,
    uint32_t vout = 0,
    bool confirmed = true
);

/**
 * Helper: Generate a random 32-byte private key for testing
 *
 * @return 32-byte private key vector
 */
std::vector<uint8_t> generateTestPrivateKey();

/**
 * Helper: Derive x-only Schnorr pubkey from private key
 *
 * @param privkey  32-byte private key
 * @return         32-byte x-only pubkey
 */
std::vector<uint8_t> derivePubkey(const std::vector<uint8_t>& privkey);

} // namespace test
} // namespace dinero
