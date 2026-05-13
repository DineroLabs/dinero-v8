#include <gtest/gtest.h>
#include "dpi/dpi_protocol.h"
#include "crypto/hash.h"
#include "wallet/schnorr_signer.h"
#include <cstring>
#include <ctime>

using namespace din::dpi;

// ============================================================================
// Helper: generate a test keypair
// ============================================================================
static std::pair<std::vector<uint8_t>, std::vector<uint8_t>> MakeTestKeypair() {
    std::vector<uint8_t> privkey(32);
    GenerateSecureRandom(privkey.data(), 32);
    auto pubkey = din::SchnorrSigner::getPublicKey(privkey);
    return {privkey, pubkey};
}

// ============================================================================
// Helper: create a valid test invoice
// ============================================================================
static DpiInvoice MakeTestInvoice(
    const std::vector<uint8_t>& merchant_privkey,
    const std::vector<uint8_t>& merchant_pubkey
) {
    DpiInvoice inv;
    inv.version = DPI_VERSION;
    inv.network = DPI_NETWORK_REG;  // regtest for testing
    inv.amount = 1000000000;  // 10 DIN
    inv.destination_address = "din1ptest1234567890abcdef";
    auto hash160 = ::din::crypto::HASH160(merchant_pubkey.data(), merchant_pubkey.size());
    std::memcpy(inv.merchant_id.data(), hash160.data(), MERCHANT_ID_SIZE);
    GenerateSecureRandom(inv.nonce.data(), NONCE_SIZE);
    inv.timestamp = 1772496000;  // fixed for determinism
    inv.expiry = 300;
    inv.memo = "Test payment";

    SignInvoice(inv, merchant_privkey);
    return inv;
}

// ============================================================================
// Tagged Hash Tests
// ============================================================================

TEST(DpiProtocol, TaggedHashDeterministic) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto h1 = DpiTaggedHash("DPI/test", data);
    auto h2 = DpiTaggedHash("DPI/test", data);
    EXPECT_EQ(h1, h2);
}

TEST(DpiProtocol, TaggedHashDifferentTags) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    auto h1 = DpiTaggedHash("DPI/invoice", data);
    auto h2 = DpiTaggedHash("DPI/attest-v1", data);
    EXPECT_NE(h1, h2);
}

TEST(DpiProtocol, TaggedHashDifferentData) {
    std::vector<uint8_t> d1 = {0x01};
    std::vector<uint8_t> d2 = {0x02};
    auto h1 = DpiTaggedHash("DPI/test", d1);
    auto h2 = DpiTaggedHash("DPI/test", d2);
    EXPECT_NE(h1, h2);
}

// ============================================================================
// Invoice ID Tests
// ============================================================================

TEST(DpiProtocol, InvoiceIdDeterministic) {
    DpiInvoice inv;
    inv.version = DPI_VERSION;
    inv.network = DPI_NETWORK_MAIN;
    inv.amount = 500000000;
    inv.destination_address = "din1pexampleaddress";
    std::memset(inv.merchant_id.data(), 0xAA, MERCHANT_ID_SIZE);
    std::memset(inv.nonce.data(), 0xBB, NONCE_SIZE);
    inv.timestamp = 1700000000;
    inv.expiry = 300;

    auto id1 = ComputeInvoiceId(inv);
    auto id2 = ComputeInvoiceId(inv);
    EXPECT_EQ(id1, id2);
}

TEST(DpiProtocol, InvoiceIdChangesWithAmount) {
    DpiInvoice inv;
    inv.version = DPI_VERSION;
    inv.network = DPI_NETWORK_MAIN;
    inv.amount = 100;
    inv.destination_address = "din1pexampleaddress";
    std::memset(inv.merchant_id.data(), 0xAA, MERCHANT_ID_SIZE);
    std::memset(inv.nonce.data(), 0xBB, NONCE_SIZE);
    inv.timestamp = 1700000000;
    inv.expiry = 300;

    auto id1 = ComputeInvoiceId(inv);

    inv.amount = 200;
    auto id2 = ComputeInvoiceId(inv);

    EXPECT_NE(id1, id2);
}

TEST(DpiProtocol, InvoiceIdChangesWithDestination) {
    DpiInvoice inv;
    inv.version = DPI_VERSION;
    inv.network = DPI_NETWORK_MAIN;
    inv.amount = 100;
    inv.destination_address = "din1paddress1";
    std::memset(inv.merchant_id.data(), 0xAA, MERCHANT_ID_SIZE);
    std::memset(inv.nonce.data(), 0xBB, NONCE_SIZE);
    inv.timestamp = 1700000000;
    inv.expiry = 300;

    auto id1 = ComputeInvoiceId(inv);

    inv.destination_address = "din1paddress2";
    auto id2 = ComputeInvoiceId(inv);

    EXPECT_NE(id1, id2);
}

TEST(DpiProtocol, InvoiceIdChangesWithNonce) {
    DpiInvoice inv;
    inv.version = DPI_VERSION;
    inv.network = DPI_NETWORK_MAIN;
    inv.amount = 100;
    inv.destination_address = "din1pexampleaddress";
    std::memset(inv.merchant_id.data(), 0xAA, MERCHANT_ID_SIZE);
    std::memset(inv.nonce.data(), 0x01, NONCE_SIZE);
    inv.timestamp = 1700000000;
    inv.expiry = 300;

    auto id1 = ComputeInvoiceId(inv);

    std::memset(inv.nonce.data(), 0x02, NONCE_SIZE);
    auto id2 = ComputeInvoiceId(inv);

    EXPECT_NE(id1, id2);
}

// ============================================================================
// Invoice Serialization Tests
// ============================================================================

TEST(DpiProtocol, InvoiceRoundTrip) {
    auto [privkey, pubkey] = MakeTestKeypair();
    auto inv = MakeTestInvoice(privkey, pubkey);

    auto serialized = SerializeInvoice(inv);
    EXPECT_GT(serialized.size(), 100u);

    DpiInvoice decoded;
    EXPECT_TRUE(DeserializeInvoice(serialized, decoded));

    EXPECT_EQ(decoded.version, inv.version);
    EXPECT_EQ(decoded.network, inv.network);
    EXPECT_EQ(decoded.amount, inv.amount);
    EXPECT_EQ(decoded.destination_address, inv.destination_address);
    EXPECT_EQ(decoded.merchant_id, inv.merchant_id);
    EXPECT_EQ(decoded.nonce, inv.nonce);
    EXPECT_EQ(decoded.timestamp, inv.timestamp);
    EXPECT_EQ(decoded.expiry, inv.expiry);
    EXPECT_EQ(decoded.memo, inv.memo);
    EXPECT_EQ(decoded.merchant_sig, inv.merchant_sig);
    EXPECT_EQ(decoded.invoice_id, inv.invoice_id);
}

TEST(DpiProtocol, InvoiceDeserializeTruncated) {
    auto [privkey, pubkey] = MakeTestKeypair();
    auto inv = MakeTestInvoice(privkey, pubkey);
    auto serialized = SerializeInvoice(inv);

    // Truncate at various points — all should fail gracefully
    DpiInvoice decoded;
    EXPECT_FALSE(DeserializeInvoice({}, decoded));
    EXPECT_FALSE(DeserializeInvoice({0x01}, decoded));

    auto truncated = std::vector<uint8_t>(serialized.begin(), serialized.begin() + 20);
    EXPECT_FALSE(DeserializeInvoice(truncated, decoded));
}

TEST(DpiProtocol, InvoiceEmptyMemo) {
    auto [privkey, pubkey] = MakeTestKeypair();
    auto inv = MakeTestInvoice(privkey, pubkey);
    inv.memo = "";
    SignInvoice(inv, privkey);

    auto serialized = SerializeInvoice(inv);
    DpiInvoice decoded;
    EXPECT_TRUE(DeserializeInvoice(serialized, decoded));
    EXPECT_EQ(decoded.memo, "");
}

// ============================================================================
// Invoice Signature Tests
// ============================================================================

TEST(DpiProtocol, InvoiceSignVerify) {
    auto [privkey, pubkey] = MakeTestKeypair();
    auto inv = MakeTestInvoice(privkey, pubkey);

    EXPECT_TRUE(VerifyInvoiceSignature(inv, pubkey));
}

TEST(DpiProtocol, InvoiceSignVerifyWrongKey) {
    auto [privkey, pubkey] = MakeTestKeypair();
    auto [privkey2, pubkey2] = MakeTestKeypair();
    auto inv = MakeTestInvoice(privkey, pubkey);

    // Verify with wrong key should fail
    EXPECT_FALSE(VerifyInvoiceSignature(inv, pubkey2));
}

TEST(DpiProtocol, InvoiceTamperingBreaksSignature) {
    auto [privkey, pubkey] = MakeTestKeypair();
    auto inv = MakeTestInvoice(privkey, pubkey);

    // Verify original is good
    EXPECT_TRUE(VerifyInvoiceSignature(inv, pubkey));

    // Tamper with amount
    inv.amount += 1;
    EXPECT_FALSE(VerifyInvoiceSignature(inv, pubkey));
}

TEST(DpiProtocol, InvoiceTamperDestinationBreaksSig) {
    auto [privkey, pubkey] = MakeTestKeypair();
    auto inv = MakeTestInvoice(privkey, pubkey);
    EXPECT_TRUE(VerifyInvoiceSignature(inv, pubkey));

    inv.destination_address += "x";
    EXPECT_FALSE(VerifyInvoiceSignature(inv, pubkey));
}

TEST(DpiProtocol, InvoiceTamperTimestampBreaksSig) {
    auto [privkey, pubkey] = MakeTestKeypair();
    auto inv = MakeTestInvoice(privkey, pubkey);
    EXPECT_TRUE(VerifyInvoiceSignature(inv, pubkey));

    inv.timestamp += 1;
    EXPECT_FALSE(VerifyInvoiceSignature(inv, pubkey));
}

// ============================================================================
// Attestation Tests
// ============================================================================

TEST(DpiProtocol, AttestationMessageDeterministic) {
    std::array<uint8_t, 32> inv_id{}, txid{}, pubkey{};
    std::memset(inv_id.data(), 0x01, 32);
    std::memset(txid.data(), 0x02, 32);
    std::memset(pubkey.data(), 0x03, 32);

    auto msg1 = ComputeAttestationMessage(inv_id, txid, pubkey);
    auto msg2 = ComputeAttestationMessage(inv_id, txid, pubkey);
    EXPECT_EQ(msg1, msg2);
}

TEST(DpiProtocol, AttestationMismatchInvoiceId) {
    std::array<uint8_t, 32> inv_id1{}, inv_id2{}, txid{}, pubkey{};
    std::memset(inv_id1.data(), 0x01, 32);
    std::memset(inv_id2.data(), 0xFF, 32);
    std::memset(txid.data(), 0x02, 32);
    std::memset(pubkey.data(), 0x03, 32);

    auto msg1 = ComputeAttestationMessage(inv_id1, txid, pubkey);
    auto msg2 = ComputeAttestationMessage(inv_id2, txid, pubkey);
    EXPECT_NE(msg1, msg2);
}

TEST(DpiProtocol, AttestationSignVerify) {
    auto [privkey, pubkey] = MakeTestKeypair();

    std::array<uint8_t, 32> inv_id{}, txid{};
    std::memset(inv_id.data(), 0xAA, 32);
    std::memset(txid.data(), 0xBB, 32);

    std::array<uint8_t, 32> sender_pub{};
    std::memcpy(sender_pub.data(), pubkey.data(), 32);

    auto sig = SignAttestation(inv_id, txid, sender_pub, privkey);
    EXPECT_EQ(sig.size(), SCHNORR_SIG_SIZE);

    EXPECT_TRUE(VerifyAttestationSignature(inv_id, txid, sender_pub, sig));
}

TEST(DpiProtocol, AttestationVerifyFailsWrongTxid) {
    auto [privkey, pubkey] = MakeTestKeypair();

    std::array<uint8_t, 32> inv_id{}, txid{}, wrong_txid{};
    std::memset(inv_id.data(), 0xAA, 32);
    std::memset(txid.data(), 0xBB, 32);
    std::memset(wrong_txid.data(), 0xCC, 32);

    std::array<uint8_t, 32> sender_pub{};
    std::memcpy(sender_pub.data(), pubkey.data(), 32);

    auto sig = SignAttestation(inv_id, txid, sender_pub, privkey);
    EXPECT_FALSE(VerifyAttestationSignature(inv_id, wrong_txid, sender_pub, sig));
}

TEST(DpiProtocol, AttestationVerifyFailsWrongSender) {
    auto [privkey, pubkey] = MakeTestKeypair();
    auto [privkey2, pubkey2] = MakeTestKeypair();

    std::array<uint8_t, 32> inv_id{}, txid{};
    std::memset(inv_id.data(), 0xAA, 32);
    std::memset(txid.data(), 0xBB, 32);

    std::array<uint8_t, 32> sender_pub{}, wrong_sender{};
    std::memcpy(sender_pub.data(), pubkey.data(), 32);
    std::memcpy(wrong_sender.data(), pubkey2.data(), 32);

    auto sig = SignAttestation(inv_id, txid, sender_pub, privkey);
    EXPECT_FALSE(VerifyAttestationSignature(inv_id, txid, wrong_sender, sig));
}

// ============================================================================
// Package Serialization Tests
// ============================================================================

TEST(DpiProtocol, PackageRoundTrip) {
    DpiPaymentPackage pkg;
    pkg.raw_tx = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    pkg.attestation_sig.resize(SCHNORR_SIG_SIZE, 0xAA);
    std::memset(pkg.sender_pubkey.data(), 0xBB, 32);
    std::memset(pkg.invoice_id.data(), 0xCC, 32);

    auto serialized = SerializePackage(pkg);
    EXPECT_GT(serialized.size(), 128u);

    DpiPaymentPackage decoded;
    EXPECT_TRUE(DeserializePackage(serialized, decoded));

    EXPECT_EQ(decoded.raw_tx, pkg.raw_tx);
    EXPECT_EQ(decoded.attestation_sig, pkg.attestation_sig);
    EXPECT_EQ(decoded.sender_pubkey, pkg.sender_pubkey);
    EXPECT_EQ(decoded.invoice_id, pkg.invoice_id);
}

TEST(DpiProtocol, PackageDeserializeTruncated) {
    DpiPaymentPackage decoded;
    EXPECT_FALSE(DeserializePackage({}, decoded));
    EXPECT_FALSE(DeserializePackage({0x01, 0x00, 0x00, 0x00}, decoded));  // tx_len=1 but no tx data
}

TEST(DpiProtocol, PackageDeserializeZeroTxLen) {
    // tx_len = 0 should fail
    std::vector<uint8_t> bad = {0x00, 0x00, 0x00, 0x00};
    DpiPaymentPackage decoded;
    EXPECT_FALSE(DeserializePackage(bad, decoded));
}

// ============================================================================
// Expiry Tests
// ============================================================================

TEST(DpiProtocol, ExpiryNotExpiredRecently) {
    DpiInvoice inv;
    inv.timestamp = static_cast<uint32_t>(std::time(nullptr));
    inv.expiry = 3600;  // 1 hour

    EXPECT_FALSE(IsInvoiceExpired(inv));
}

TEST(DpiProtocol, ExpiryExpiredInPast) {
    DpiInvoice inv;
    inv.timestamp = 1000000;  // Jan 1970
    inv.expiry = 300;

    EXPECT_TRUE(IsInvoiceExpired(inv));
}

// ============================================================================
// Risk Score Tests
// ============================================================================

TEST(DpiProtocol, RiskScoreAllGood) {
    DpiVerifyChecks checks;
    checks.invoice_bound = true;
    checks.output_match = true;
    checks.amount_match = true;
    checks.attestation_valid = true;
    checks.seen_in_mempool = true;
    checks.conflicts_found = false;
    checks.expired = false;

    double score = ComputeRiskScore(checks);
    EXPECT_LE(score, 0.01);  // Should be 0.0
    EXPECT_EQ(DetermineTier(checks, score), "T1");
}

TEST(DpiProtocol, RiskScoreNotInMempool) {
    DpiVerifyChecks checks;
    checks.invoice_bound = true;
    checks.output_match = true;
    checks.amount_match = true;
    checks.attestation_valid = true;
    checks.seen_in_mempool = false;  // not yet propagated
    checks.conflicts_found = false;
    checks.expired = false;

    double score = ComputeRiskScore(checks);
    EXPECT_GT(score, 0.15);  // Not T1
    EXPECT_EQ(DetermineTier(checks, score), "T0");
}

TEST(DpiProtocol, RiskScoreConflictDetected) {
    DpiVerifyChecks checks;
    checks.invoice_bound = true;
    checks.output_match = true;
    checks.amount_match = true;
    checks.attestation_valid = true;
    checks.seen_in_mempool = true;
    checks.conflicts_found = true;  // double-spend attempt
    checks.expired = false;

    double score = ComputeRiskScore(checks);
    EXPECT_NEAR(score, 0.15, 0.01);
    EXPECT_EQ(DetermineTier(checks, score), "T0");
}

TEST(DpiProtocol, RiskScoreAllBad) {
    DpiVerifyChecks checks{};  // all false/default

    double score = ComputeRiskScore(checks);
    EXPECT_GE(score, 0.85);
    EXPECT_EQ(DetermineTier(checks, score), "T0");
}

// ============================================================================
// Canonical Fields Tests
// ============================================================================

TEST(DpiProtocol, CanonicalInvoiceFieldsDeterministic) {
    DpiInvoice inv;
    inv.version = DPI_VERSION;
    inv.network = DPI_NETWORK_MAIN;
    inv.amount = 42;
    inv.destination_address = "din1ptest";
    std::memset(inv.merchant_id.data(), 0xAA, 20);
    std::memset(inv.nonce.data(), 0xBB, 16);
    inv.timestamp = 12345;
    inv.expiry = 300;

    auto f1 = CanonicalInvoiceFields(inv);
    auto f2 = CanonicalInvoiceFields(inv);
    EXPECT_EQ(f1, f2);
    EXPECT_GT(f1.size(), 50u);
}

TEST(DpiProtocol, CanonicalAttestationFieldsSize) {
    std::array<uint8_t, 32> a{}, b{}, c{};
    auto fields = CanonicalAttestationFields(a, b, c);
    EXPECT_EQ(fields.size(), 96u);  // 32 + 32 + 32
}

// ============================================================================
// End-to-end: Invoice → Attestation → Verify
// ============================================================================

TEST(DpiProtocol, EndToEndInvoiceAndAttestation) {
    // Merchant creates invoice
    auto [merchant_priv, merchant_pub] = MakeTestKeypair();
    auto inv = MakeTestInvoice(merchant_priv, merchant_pub);

    // Override timestamp to now so it's not expired
    inv.timestamp = static_cast<uint32_t>(std::time(nullptr));
    SignInvoice(inv, merchant_priv);

    // Verify invoice
    EXPECT_TRUE(VerifyInvoiceSignature(inv, merchant_pub));
    EXPECT_FALSE(IsInvoiceExpired(inv));

    // Sender creates attestation
    auto [sender_priv, sender_pub] = MakeTestKeypair();
    std::array<uint8_t, 32> fake_txid{};
    std::memset(fake_txid.data(), 0xDE, 32);

    std::array<uint8_t, 32> sender_pub_arr{};
    std::memcpy(sender_pub_arr.data(), sender_pub.data(), 32);

    auto attest_sig = SignAttestation(inv.invoice_id, fake_txid, sender_pub_arr, sender_priv);
    EXPECT_EQ(attest_sig.size(), SCHNORR_SIG_SIZE);

    // Verify attestation
    EXPECT_TRUE(VerifyAttestationSignature(inv.invoice_id, fake_txid, sender_pub_arr, attest_sig));

    // Build and round-trip package
    DpiPaymentPackage pkg;
    pkg.raw_tx = {0x01, 0x02, 0x03};  // fake tx
    pkg.attestation_sig = attest_sig;
    pkg.sender_pubkey = sender_pub_arr;
    pkg.invoice_id = inv.invoice_id;

    auto serialized_pkg = SerializePackage(pkg);
    DpiPaymentPackage decoded_pkg;
    EXPECT_TRUE(DeserializePackage(serialized_pkg, decoded_pkg));
    EXPECT_EQ(decoded_pkg.invoice_id, inv.invoice_id);
    EXPECT_TRUE(VerifyAttestationSignature(
        decoded_pkg.invoice_id, fake_txid, decoded_pkg.sender_pubkey, decoded_pkg.attestation_sig));
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
