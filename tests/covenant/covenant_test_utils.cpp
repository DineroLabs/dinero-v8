/**
 * Phase C.3: Covenant Test Transaction Generators - Implementation
 */

#include "covenant_test_utils.h"
#include "wallet/covenant_builders.h"
#include "primitives/uint256.h"
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <random>
#include <cstring>

namespace dinero {
namespace test {

using namespace dinero::wallet;

// ============================================================================
// Helper Functions
// ============================================================================

std::vector<uint8_t> generateTestPrivateKey() {
    std::vector<uint8_t> privkey(32);

    // Simple deterministic key for testing (NOT secure, test-only!)
    static uint8_t counter = 1;
    for (size_t i = 0; i < 32; i++) {
        privkey[i] = static_cast<uint8_t>((i * 7 + counter * 13) % 256);
    }
    counter++;

    return privkey;
}

std::vector<uint8_t> derivePubkey(const std::vector<uint8_t>& privkey) {
    if (privkey.size() != 32) {
        throw std::runtime_error("Private key must be 32 bytes");
    }

    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY
    );

    if (!ctx) {
        throw std::runtime_error("Failed to create secp256k1 context");
    }

    try {
        secp256k1_keypair keypair;
        if (!secp256k1_keypair_create(ctx, &keypair, privkey.data())) {
            throw std::runtime_error("Invalid private key");
        }

        secp256k1_xonly_pubkey xonly_pubkey;
        if (!secp256k1_keypair_xonly_pub(ctx, &xonly_pubkey, nullptr, &keypair)) {
            throw std::runtime_error("Failed to derive x-only pubkey");
        }

        std::vector<uint8_t> pubkey(32);
        secp256k1_xonly_pubkey_serialize(ctx, pubkey.data(), &xonly_pubkey);

        secp256k1_context_destroy(ctx);
        return pubkey;

    } catch (...) {
        secp256k1_context_destroy(ctx);
        throw;
    }
}

CTVOutput createStandardOutput(uint64_t value) {
    CTVOutput output;
    output.value = value;

    // P2WPKH: OP_0 + 20-byte pubkey hash
    output.scriptPubKey = {
        0x00, 0x14,  // OP_0 + PUSHBYTES_20
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
        0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14
    };
    output.address = "tb1q...test";

    return output;
}

CanonicalWalletUTXO createDummyUTXO(uint64_t value, uint32_t vout, bool confirmed) {
    CanonicalWalletUTXO utxo;

    // Generate deterministic TXID for testing
    static uint32_t txid_counter = 1;
    std::string txid_hex(64, '0');
    snprintf(&txid_hex[56], 9, "%08x", txid_counter++);

    utxo.txid = uint256::FromHex(txid_hex);
    utxo.vout = vout;
    utxo.value = value;
    utxo.height = confirmed ? 100 : 0;  // 0 = unconfirmed
    utxo.is_coinbase = false;

    return utxo;
}

// ============================================================================
// Valid CTV Scenario
// ============================================================================

CovenantTestScenario createValidCTVScenario() {
    CovenantTestScenario scenario;
    scenario.description = "Valid CTV: funding + spending match template";

    // Step 1: Create CTV template
    std::vector<CTVOutput> template_outputs;

    CTVOutput out1 = createStandardOutput(50000);
    CTVOutput out2 = createStandardOutput(40000);
    template_outputs.push_back(out1);
    template_outputs.push_back(out2);

    auto ctv_template = buildCTVTemplate(template_outputs, 0, 2);

    // Step 2: Create funding transaction
    // This tx creates an output locked by CTV
    scenario.funding_tx.version = 2;
    scenario.funding_tx.lockTime = 0;

    // Funding input (from somewhere)
    TxInput funding_input;
    funding_input.prevout.txid = uint256::FromHex(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    );
    funding_input.prevout.vout = 0;
    funding_input.sequence = 0xfffffffe;
    scenario.funding_tx.vin.push_back(funding_input);

    // CTV-locked output
    TxOutput ctv_output;
    ctv_output.value = 100000;  // Enough for both template outputs + fee
    ctv_output.scriptPubKey = createCTVScript(ctv_template.template_hash, false);
    scenario.funding_tx.vout.push_back(ctv_output);

    // Step 3: Create spending transaction that matches template
    CanonicalWalletUTXO funding_utxo;
    funding_utxo.txid = scenario.funding_tx.getTxId();
    funding_utxo.vout = 0;
    funding_utxo.value = 100000;
    funding_utxo.height = 100;  // Confirmed
    funding_utxo.is_coinbase = false;

    scenario.spending_tx = buildCTVSpendingTx(ctv_template, funding_utxo, 0);

    return scenario;
}

// ============================================================================
// Invalid CTV Scenario (Template Mismatch)
// ============================================================================

CovenantTestScenario createInvalidCTVScenario() {
    CovenantTestScenario scenario;
    scenario.description = "Invalid CTV: spending tx doesn't match template";

    // Start with valid scenario
    scenario = createValidCTVScenario();

    // Break the template match by modifying output value
    if (!scenario.spending_tx.vout.empty()) {
        scenario.spending_tx.vout[0].value += 1000;  // Change value (breaks CTV)
    }

    scenario.description = "Invalid CTV: output value mismatch";
    return scenario;
}

// ============================================================================
// Valid CSFS Scenario
// ============================================================================

CovenantTestScenario createValidCSFSScenario() {
    CovenantTestScenario scenario;
    scenario.description = "Valid CSFS: properly signed delegation";

    // Step 1: Generate keypair
    scenario.privkey = generateTestPrivateKey();
    std::vector<uint8_t> pubkey = derivePubkey(scenario.privkey);

    // Step 2: Create delegation message
    std::vector<uint8_t> message = {
        'd', 'e', 'l', 'e', 'g', 'a', 't', 'i', 'o', 'n', ' ',
        't', 'e', 's', 't', ' ', 'm', 's', 'g'
    };

    // Step 3: Create unsigned delegation
    auto delegation = createCSFSDelegation(pubkey, message, "test");

    // Step 4: Sign the delegation
    auto signed_delegation = signCSFSDelegation(delegation, scenario.privkey);

    // Step 5: Create funding transaction
    scenario.funding_tx.version = 2;
    scenario.funding_tx.lockTime = 0;

    TxInput funding_input;
    funding_input.prevout.txid = uint256::FromHex(
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    );
    funding_input.prevout.vout = 0;
    funding_input.sequence = 0xfffffffe;
    scenario.funding_tx.vin.push_back(funding_input);

    // CSFS-locked output
    TxOutput csfs_output;
    csfs_output.value = 100000;
    csfs_output.scriptPubKey = createCSFSScript(pubkey, message);
    scenario.funding_tx.vout.push_back(csfs_output);

    // Step 6: Create spending transaction with signature
    scenario.spending_tx.version = 2;
    scenario.spending_tx.lockTime = 0;

    TxInput spending_input;
    spending_input.prevout.txid = scenario.funding_tx.getTxId();
    spending_input.prevout.vout = 0;
    spending_input.sequence = 0xfffffffe;

    // Add signature to witness
    spending_input.witness = {signed_delegation.signature};

    scenario.spending_tx.vin.push_back(spending_input);

    // Standard output
    TxOutput output;
    output.value = 95000;  // After fee
    output.scriptPubKey = createStandardOutput(95000).scriptPubKey;
    scenario.spending_tx.vout.push_back(output);

    return scenario;
}

// ============================================================================
// Invalid CSFS Scenario (Bad Signature)
// ============================================================================

CovenantTestScenario createInvalidCSFSScenario() {
    CovenantTestScenario scenario;
    scenario.description = "Invalid CSFS: bad signature";

    // Start with valid scenario
    scenario = createValidCSFSScenario();

    // Corrupt the signature
    if (!scenario.spending_tx.vin.empty() &&
        !scenario.spending_tx.vin[0].witness.empty() &&
        scenario.spending_tx.vin[0].witness[0].size() >= 32) {

        // Flip a byte in the signature
        scenario.spending_tx.vin[0].witness[0][16] ^= 0xFF;
    }

    scenario.description = "Invalid CSFS: corrupted signature";
    return scenario;
}

// ============================================================================
// Mixed Covenant Scenario
// ============================================================================

CovenantTestScenario createMixedCovenantScenario(
    size_t covenant_inputs,
    size_t standard_inputs
) {
    CovenantTestScenario scenario;
    scenario.description = "Mixed: covenant + standard inputs";

    scenario.spending_tx.version = 2;
    scenario.spending_tx.lockTime = 0;

    uint64_t total_value = 0;

    // Add covenant inputs (simple CTV for testing)
    for (size_t i = 0; i < covenant_inputs; i++) {
        // Create simple CTV template
        std::vector<CTVOutput> outputs;
        outputs.push_back(createStandardOutput(30000));

        auto ctv_template = buildCTVTemplate(outputs, 0, 2);

        TxInput input;
        input.prevout.txid = uint256::FromHex(
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc" +
            std::to_string(i).substr(0, 4)
        );
        input.prevout.vout = 0;
        input.sequence = 0xfffffffe;

        scenario.spending_tx.vin.push_back(input);
        total_value += 50000;
    }

    // Add standard inputs
    for (size_t i = 0; i < standard_inputs; i++) {
        TxInput input;
        input.prevout.txid = uint256::FromHex(
            "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd" +
            std::to_string(i).substr(0, 4)
        );
        input.prevout.vout = 0;
        input.sequence = 0xfffffffe;

        scenario.spending_tx.vin.push_back(input);
        total_value += 50000;
    }

    // Single output
    TxOutput output;
    output.value = total_value - 5000;  // Leave fee
    output.scriptPubKey = createStandardOutput(output.value).scriptPubKey;
    scenario.spending_tx.vout.push_back(output);

    return scenario;
}

// ============================================================================
// DoS Covenant Scenario (Too Many Inputs)
// ============================================================================

CovenantTestScenario createDoSCovenantScenario(size_t covenant_input_count) {
    CovenantTestScenario scenario;
    scenario.description = "DoS: too many covenant inputs";

    scenario.spending_tx.version = 2;
    scenario.spending_tx.lockTime = 0;

    uint64_t total_value = 0;

    // Add many covenant inputs
    for (size_t i = 0; i < covenant_input_count; i++) {
        TxInput input;

        std::string txid_hex(64, 'e');
        snprintf(&txid_hex[56], 9, "%08zx", i);

        input.prevout.txid = uint256::FromHex(txid_hex);
        input.prevout.vout = 0;
        input.sequence = 0xfffffffe;

        scenario.spending_tx.vin.push_back(input);
        total_value += 50000;
    }

    // Single output
    TxOutput output;
    output.value = total_value - 5000;  // Leave fee
    output.scriptPubKey = createStandardOutput(output.value).scriptPubKey;
    scenario.spending_tx.vout.push_back(output);

    return scenario;
}

} // namespace test
} // namespace dinero
