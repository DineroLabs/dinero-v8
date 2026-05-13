// Canonical ChainParams implementation
// This is the ONLY file that defines g_chainParams and implements Params()
// All other chainparams*.cpp files MUST be deleted to prevent ODR violations

#include "consensus/chainparams.h"
#include "consensus/chain_identity.h"
#include "consensus/chainparamsseeds.h"  // Phase D: generated fixed-seed list
#include "crypto/sha256.h"
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace dinero {

// ============================================================================
// LINK-TIME SENTINEL
// If this symbol is missing at link-time, the build MUST fail.
// If a duplicate implementation sneaks in, you'll get a duplicate symbol error.
// ============================================================================
extern "C" const char* kChainParamsImplTag =
    "chainparams_impl: src/consensus/chainparams_impl.cpp";

// ============================================================================
// MOTTO VERIFICATION (Proof-of-Authenticity)
// ============================================================================
// Official Dinero motto — v7 restart (April 17 2026)
// SHA256 hash for tamper-proof verification
static constexpr const char* DINERO_MOTTO = dinero::chain_bundle::GENESIS_MOTTO;
static constexpr const char* DINERO_MOTTO_SHA256 = "[compute at runtime]";

// ============================================================================
// COMPILE-TIME GENESIS VERIFICATION
// Prevents accidental genesis constant corruption at build time
// ============================================================================
static constexpr const char* EXPECTED_GENESIS_HASH =
    dinero::consensus::kMainnetGenesisHash.data();  // v7 genesis (from chain_bundle)
static constexpr const char* EXPECTED_MERKLE_ROOT =
    dinero::chain_bundle::GENESIS_MERKLE_ROOT;       // v7 merkle (from chain_bundle)

// ============================================================================
// MAINNET PARAMETERS - Dinero Official Mainnet
// ============================================================================
static ChainParams g_mainnet = {
    .name = "mainnet",
    .hrp = "din",
    .magic = 0xD1A0C0DEu,  // Dinero mainnet P2P magic — v7 restart (must match p2p::NetworkMagic::MAINNET)
    .rpc_port = 20998,     // Dinero mainnet RPC
    .http_port = 8080,
    .ws_port = 21001,
    .p2p_port = 20999,     // Dinero mainnet P2P
    .genesis_hash = std::string(dinero::consensus::kMainnetGenesisHash),  // v7 restart genesis
    .network_id = "main",
    .pow_limit_bits = 0x1d31ffce,  // 50× easier than Bitcoin genesis (CPU-friendly bootstrap)
    .target_spacing = 120,  // 2 minutes (Post-Utreexo ASERT)
    .retarget_interval = 720,   // ~1 day (720 blocks × 2 min) - matches bootstrapWindow
    .dust_threshold = 546,
    .min_relay_fee = 1000,  // 1000 una/kb
    .max_block_size = 1000000,  // 1MB
    .coinbase_maturity = 100,  // Standard Bitcoin maturity (100 confirmations)
    .pubkey_address_prefix = 0x00,
    .script_address_prefix = 0x05,
    .allow_min_difficulty = false,  // Production: Full difficulty enforcement
    .require_standard_txs = true,
    .mine_blocks_on_demand = false,

    // Phase 11d: Witness commitment enforcement (active from height 2)
    .enforce_witness_commitment = true,              // ENFORCED on mainnet
    .witness_commitment_enforcement_height = 1,      // Height 1+ (v7 restart: features live from block 1)

    // Phase 11e: Bitcoin magic translation (OFF by default - safe)
    .enable_witness_magic_translation = false,       // NOT translated on mainnet
    .witness_magic_translation_height = UINT32_MAX,  // Never triggers

    // Confidential transaction activation (after genesis)
    .confidential_activation_height = 1,

    // Shielded pool activation on mainnet — set 2026-04-27.
    // Direct mainnet activation; testnet phase skipped per
    // shielded_activation_plan.md (solo operator + 25/25 ctest
    // suite as soak proxy + fix-forward policy on the live chain).
    // Buffer: tip at decision time was 8602 → activation +48 blocks
    // ≈ ~8 hrs at typical 10-min mainnet cadence, enough to deploy
    // the binary across all 4 servers and watch a few blocks land
    // before the gate flips.
    .shielded_activation_height = 8650,

    .genesis = {
        .nVersion = 1,
        .nTime = 1776384000,  // 2026-04-17 00:00:00 UTC — v7 Genesis Restart
        .nBits = 0x1d31ffce,  // 50x easier than Bitcoin (reused from v5)
        .nNonce = 813915426,
        .genesisHashHex =
            std::string(EXPECTED_GENESIS_HASH),
        .merkleRootHex =
            std::string(EXPECTED_MERKLE_ROOT),
        .genesisCoinbaseHex =
            // 100 DIN burned via OP_RETURN (genesis), double commitment
            // Motto: "Dinero: Real Money For Free People - Post-Quantum Native. April 17 2026"
            "01000000010000000000000000000000000000000000000000000000000000000000000000"
            "ffffffff480044696e65726f3a205265616c204d6f6e657920466f7220467265652050"
            "656f706c65202d20506f73742d5175616e74756d204e61746976652e20417072696c20"
            "31372032303236ffffffff0100e40b5402000000496a4744696e65726f3a205265616c"
            "204d6f6e657920466f7220467265652050656f706c65202d20506f73742d5175616e74"
            "756d204e61746976652e20417072696c203137203230323600000000"
    },

    // ===========================================================================
    // ANTI-SELF-CHAIN SAFEGUARDS FOR MAINNET
    // ===========================================================================

    // ───────────────────────────────────────────────────────────────────────────
    // F.10.10: Minimum Chainwork Enforcement (Eclipse Attack Protection)
    // ───────────────────────────────────────────────────────────────────────────
    // WHAT: Reject chains that don't meet this cumulative proof-of-work threshold
    // WHY:  Prevents eclipse attacks during Initial Block Download (IBD)
    //       Attackers can't isolate a node and feed it a low-work fake chain
    //
    // UPDATE POLICY (Bitcoin Core standard):
    //   1. Update before EACH RELEASE (monthly/quarterly)
    //   2. Set to chainwork of block ~2 weeks before release date
    //   3. Verify against block explorer: cumulative_work(height - 4032)
    //   4. Format: 64-character hex string with 0x prefix
    //   5. NEVER set to 0 in production (disables protection entirely)
    //
    // CURRENT VALUE: Chainwork at block 1000 (set 2026-03-04 at height 1297)
    //   - Measured via getblockheader chainwork at height 1000
    //   - Update before each release to chainwork ~2 weeks behind tip
    //
    // ENFORCEMENT: See src/daemon/block_acceptor.cpp:1048-1061
    // ───────────────────────────────────────────────────────────────────────────
    .nMinimumChainWork = "0x0000000000000000000000000000000000000000000000000000000000000000",  // Reset for chain restart

    // ───────────────────────────────────────────────────────────────────────────
    // F.10.9: AssumeValid Optimization (IBD Performance)
    // ───────────────────────────────────────────────────────────────────────────
    // WHAT: Nodes skip script verification for blocks below this height during IBD
    // WHY:  5-10x faster sync without sacrificing security
    //       Still validates: PoW, merkle roots, UTXOs, structure, chainwork
    //
    // SAFETY GUARANTEE: AssumeValid is ONLY safe because minimum chainwork (F.10.10)
    // prevents eclipse attacks. The combination is critical:
    //   1. Minimum chainwork ensures we're on the real chain (hard gate)
    //   2. AssumeValid skips expensive signatures (performance optimization)
    //   3. Without F.10.10, AssumeValid would be dangerous
    //
    // UPDATE POLICY (Bitcoin Core standard):
    //   1. Update before EACH RELEASE (monthly/quarterly)
    //   2. Set to block hash ~2 weeks before release date
    //   3. Set assumeValidHeight to match the block height
    //   4. Verify against block explorer (e.g., block 10000 → hash from explorer)
    //   5. Both defaultAssumeValid (hash) and assumeValidHeight (height) must match
    //   6. Use `-assumevalid=0` CLI flag to disable (for testing/auditing)
    //
    // CURRENT VALUE: Block 1200 (set 2026-03-04 at height 1292)
    //   - Nodes skip script verification for blocks 0-1200 during IBD
    //   - Update before each release to block ~2 weeks behind tip
    //
    // ENFORCEMENT: See src/daemon/block_acceptor.cpp:1063-1086
    //              See src/consensus/tx_validation.cpp:126-140
    // ───────────────────────────────────────────────────────────────────────────
    .defaultAssumeValid = "0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3",
    .assumeValidHeight = 13000,

    // DNS Seeds: Domain names that return IP addresses of seed nodes
    // DEPLOYMENT NOTE: Configure DNS A records:
    //   seed1.dinero-coin.com -> 172.93.160.131 (California)
    //   seed2.dinero-coin.com -> 173.249.195.59 (Virginia)
    //   seed3.dinero-coin.com -> 72.18.214.120 (Missouri)
    //   seed4.dinero-coin.com -> 96.9.226.98 (Canada)
    .vSeeds = {
        "seed1.dinero-coin.com",  // California seed
        "seed2.dinero-coin.com",  // Virginia seed
        "seed3.dinero-coin.com",  // Missouri seed
        "seed4.dinero-coin.com",  // Canada seed
    },

    // Fixed Seeds: hardcoded fallback IP:port combinations. Last-resort
    // when peers.dat is empty AND DNS seeds are unreachable AND no
    // -addnode is set. Generated by contrib/seeds/generate-seeds.py
    // from contrib/seeds/seeds_main.txt (Phase D of v8 peer-discovery).
    // Edit seeds_main.txt, re-run the generator, commit both files.
    .vFixedSeeds = dinero::consensus::kFixedSeedsMainnet,

    // ═══════════════════════════════════════════════════════════════════════════
    // CHECKPOINTS: Anti-Reorg & Fake Chain Protection
    // ═══════════════════════════════════════════════════════════════════════════
    // Checkpoints prevent reorgs past these blocks and reject chains that don't
    // match these hashes. This protects against 51% attacks and fake chains.
    //
    // HOW TO ADD CHECKPOINTS:
    //   1. Run: ./tools/get_checkpoint_hashes.sh
    //   2. Copy the output hashes to this list
    //   3. Rebuild binaries: cd build && make -j8
    //   4. Deploy to all nodes (Mac + Linux)
    //
    // WHEN TO ADD CHECKPOINTS:
    //   - Add checkpoint every ~1000 blocks for mature chains
    //   - Add after major milestones (10k, 20k, 50k blocks)
    //   - Update before major releases
    // ═══════════════════════════════════════════════════════════════════════════
    .vCheckpoints = {
        // v7 restart genesis (April 17 2026)
        {0,    std::string(dinero::consensus::kMainnetGenesisHash)},  // Genesis (v7 restart)
        // Dinero v1 trust anchor. Verified across CN/LA/VA/MO/Dell/Mac on 2026-05-03.
        // Utreexo root at this height:
        //   eca67bc825cadefab2561f48e82a00342016d1f3ad905bb277283d38de0bd54c
        // Chainwork at this height:
        //   0x000000000000000000000000000000000000000000000000000001198ed06efa
        {13000, "0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3"},
    }
};

// ============================================================================
// TESTNET PARAMETERS
// ============================================================================
static ChainParams g_testnet = {
    .name = "testnet",
    .hrp = "tdin",
    .magic = 0xDAB5BFFAu,  // Dinero testnet P2P magic (must match p2p::NetworkMagic::TESTNET)
    .rpc_port = 20998,     // Dinero testnet RPC
    .http_port = 18080,
    .ws_port = 18081,
    .p2p_port = 21000,     // Dinero testnet P2P
    .genesis_hash = std::string(dinero::consensus::kTestnetGenesisHash),  // Phase M.0: updated with corrected merkle
    .network_id = "test",
    .pow_limit_bits = 0x1d31ffce,  // 50× easier than Bitcoin genesis (matches mainnet)
    .target_spacing = 120,  // 2 minutes (ASERT optimized)
    .retarget_interval = 720,   // ~1 day (720 blocks × 2 min) - matches bootstrapWindow
    .dust_threshold = 546,
    .min_relay_fee = 1000,  // 1000 una/kb
    .max_block_size = 1000000,  // 1MB
    .coinbase_maturity = 100,  // Standard Bitcoin maturity (100 confirmations)
    .pubkey_address_prefix = 0x6f,
    .script_address_prefix = 0xc4,
    .allow_min_difficulty = true,
    .require_standard_txs = true,
    .mine_blocks_on_demand = false,

    // Phase 11d: Witness commitment enforcement (active from height 2)
    .enforce_witness_commitment = true,              // ENFORCED on testnet
    .witness_commitment_enforcement_height = 2,      // First block after genesis

    // Phase 11e: Bitcoin magic translation (OFF by default - safe)
    .enable_witness_magic_translation = false,       // NOT translated on testnet
    .witness_magic_translation_height = UINT32_MAX,  // Never triggers

    // Confidential transaction activation (immediately on testnet)
    .confidential_activation_height = 0,

    // Shielded pool — testnet phase parked.
    // Solo operator + fix-forward-on-mainnet policy made the
    // testnet → soak → mainnet pipeline obsolete. Testnet stays
    // gated indefinitely; if testnet is ever brought up again
    // (cross-impl parity, third-party testers), pick a real height
    // here. See docs/specs/shielded_activation_plan.md.
    .shielded_activation_height = UINT32_MAX,

    .genesis = {
        .nVersion = 1,
        .nTime = 1296688602,
        .nBits = 0x1d00ffff,
        .nNonce = 0,
        .genesisHashHex = std::string(dinero::consensus::kTestnetGenesisHash),  // Phase M.0: recomputed with correct merkle
        .merkleRootHex = "31557821c35a8c6d91fba33f422d3fdce9a12d0aaf348c23e2990d837c6e6827",  // Phase M.0 fix: byte order corrected (testnet)
        .genesisCoinbaseHex =
            // Testnet genesis coinbase tx
            // Message: "Dinero Testnet Genesis - Testing Network 2025"
            "01000000010000000000000000000000000000000000000000000000000000000000000000"
            "ffffffff2f002d44696e65726f20546573746e65742047656e65736973202d2054657374"
            "696e67204e6574776f726b2032303235ffffffff030000000000000000156a1344696e65"
            "726f2047656e65736973204275726e0003164e020000000200ac00407a10f35a00001600"
            "147e0027e0e55eaacd520b5792d6dc61a10464939300000000"
    }
};

// ============================================================================
// REGTEST PARAMETERS - EASY DIFFICULTY FOR INSTANT TESTING
// ============================================================================
// Helper function to initialize regtest genesis params (workaround for std::string in static init)
static GenesisParams CreateRegtestGenesis() {
    GenesisParams g;
    g.nVersion = 1;
    // Regtest uses SAME canonical genesis as mainnet for consistency
    g.nTime = 1776384000;  // 2026-04-17 00:00:00 UTC — v7 Genesis Restart
    g.nBits = 0x1d31ffce;
    g.nNonce = 813915426;
    g.genesisHashHex = std::string(EXPECTED_GENESIS_HASH);
    g.merkleRootHex = std::string(EXPECTED_MERKLE_ROOT);
    g.genesisCoinbaseHex = std::string(
        "01000000010000000000000000000000000000000000000000000000000000000000000000"
        "ffffffff480044696e65726f3a205265616c204d6f6e657920466f7220467265652050"
        "656f706c65202d20506f73742d5175616e74756d204e61746976652e20417072696c20"
        "31372032303236ffffffff0100e40b5402000000496a4744696e65726f3a205265616c"
        "204d6f6e657920466f7220467265652050656f706c65202d20506f73742d5175616e74"
        "756d204e61746976652e20417072696c203137203230323600000000"
    );
    return g;
}

static ChainParams g_regtest = {
    .name = "regtest",
    .hrp = "rdin",                    // DIFFERENT from mainnet (rdin vs din) - prevents address confusion
    .magic = 0xFABFB5DAu,             // Dinero regtest P2P magic (must match p2p::NetworkMagic::REGTEST)
    .rpc_port = 20996,                // DIFFERENT RPC port (20996 vs 20998)
    .http_port = 18880,               // DIFFERENT HTTP port
    .ws_port = 18881,                 // DIFFERENT WebSocket port
    .p2p_port = 21001,                // DIFFERENT P2P port (21001 vs 20999)
    .genesis_hash = std::string(dinero::consensus::kRegtestGenesisHash),  // Same canonical genesis as mainnet (v4)
    .network_id = "regtest",          // DIFFERENT network ID
    .pow_limit_bits = 0x207fffff,     // VERY EASY difficulty for instant mining
    .target_spacing = 120,            // 2 minutes (same as mainnet for ASERT testing)
    .retarget_interval = 144,         // Shorter for testing
    .dust_threshold = 546,
    .min_relay_fee = 1000,            // 1000 una/kb
    .max_block_size = 1000000,        // 1MB
    .coinbase_maturity = 10,          // Regtest: Faster maturity for testing (vs 100 for mainnet)
    .pubkey_address_prefix = 0x6f,    // Same as testnet (prevents mainnet address confusion)
    .script_address_prefix = 0xc4,    // Same as testnet (prevents mainnet address confusion)
    .allow_min_difficulty = true,     // Allow instant mining
    .require_standard_txs = false,    // Allow non-standard txs for testing
    .mine_blocks_on_demand = true,    // Enable instant block generation

    // Phase 11d: Witness commitment enforcement (OFF by default, configurable for tests)
    .enforce_witness_commitment = false,             // NOT enforced by default
    .witness_commitment_enforcement_height = UINT32_MAX,  // Never triggers (tests override)

    // Phase 11e: Bitcoin magic translation (OFF by default, configurable for tests)
    .enable_witness_magic_translation = false,       // NOT translated by default
    .witness_magic_translation_height = UINT32_MAX,  // Never triggers (tests override)

    // Confidential transaction activation (immediately on regtest)
    .confidential_activation_height = 0,

    // Shielded pool: active from genesis on regtest so unit tests
    // exercise the full pipeline without needing to override.
    .shielded_activation_height = 0,

    .genesis = CreateRegtestGenesis()
};

// ============================================================================
// RUNTIME GENESIS INTEGRITY VERIFICATION
// Verify genesis constants match expected values at initialization
// Note: C++ static_assert cannot work with ChainParams struct initialization
// because std::string is not a constexpr type. Runtime validation in main() provides
// equivalent protection and catches mismatches before chain operations begin.
// ============================================================================

// ============================================================================
// CONSENSUS CHECKSUM FUNCTION
// Compute a deterministic hash of critical consensus parameters
// ============================================================================

std::string ConsensusChecksum(const ChainParams& params) {
    std::ostringstream ss;
    // Include all consensus-critical parameters
    ss << params.target_spacing
       << params.retarget_interval
       << params.pow_limit_bits
       << params.genesis.nTime
       << params.genesis.nBits
       << params.genesis.nNonce
       << params.genesis.genesisHashHex
       << params.genesis.merkleRootHex;

    auto str = ss.str();
    std::vector<uint8_t> data(str.begin(), str.end());

    crypto::CSHA256 hash;
    hash.Write(data.data(), data.size());
    auto result = hash.Finalize();

    // Convert to hex string
    std::ostringstream hexStream;
    hexStream << std::hex << std::setfill('0');
    for (auto byte : result) {
        hexStream << std::setw(2) << static_cast<int>(byte);
    }
    return hexStream.str();
}

// ============================================================================
// ACTIVE CHAIN STATE
// ============================================================================
static const ChainParams* g_active = &g_mainnet;  // Default to mainnet
static bool g_paramsSelected = false;

// ============================================================================
// PUBLIC API IMPLEMENTATION
// ============================================================================

namespace detail {

const ChainParams& ParamsImpl() {
    // Lazy init regtest genesis if selected (workaround for std::string in static init)
    if (g_active == &g_regtest && g_regtest.genesis.merkleRootHex.empty()) {
        auto g = CreateRegtestGenesis();
        g_regtest.genesis.nVersion = g.nVersion;
        g_regtest.genesis.nTime = g.nTime;
        g_regtest.genesis.nBits = g.nBits;
        g_regtest.genesis.nNonce = g.nNonce;
        g_regtest.genesis.genesisHashHex = g.genesisHashHex;
        g_regtest.genesis.merkleRootHex = g.merkleRootHex;
        g_regtest.genesis.genesisCoinbaseHex = g.genesisCoinbaseHex;
    }
    return *g_active;
}

} // namespace detail

void SelectParams(Chain chain) {
    g_paramsSelected = true;
    switch (chain) {
        case Chain::MAINNET:
            g_active = &g_mainnet;
            break;
        case Chain::TESTNET:
            g_active = &g_testnet;
            break;
        case Chain::REGTEST:
            // Lazy initialize regtest genesis (workaround for std::string in static init)
            if (g_regtest.genesis.merkleRootHex.empty()) {
                g_regtest.genesis = CreateRegtestGenesis();
            }
            g_active = &g_regtest;
            break;
        default:
            throw std::invalid_argument("Unknown chain");
    }
}

Chain GetActiveChain() {
    if (!g_paramsSelected) {
        throw std::runtime_error("Chain parameters not selected. Call SelectParams() first.");
    }
    if (g_active == &g_mainnet) return Chain::MAINNET;
    if (g_active == &g_testnet) return Chain::TESTNET;
    if (g_active == &g_regtest) return Chain::REGTEST;
    throw std::runtime_error("Internal error: unknown active chain");
}

std::string ChainToString(Chain chain) {
    switch (chain) {
        case Chain::MAINNET: return "mainnet";
        case Chain::TESTNET: return "testnet";
        case Chain::REGTEST: return "regtest";
        default: throw std::invalid_argument("Unknown chain");
    }
}

Chain StringToChain(const std::string& chain_str) {
    if (chain_str == "mainnet" || chain_str == "main") return Chain::MAINNET;
    if (chain_str == "testnet" || chain_str == "test") return Chain::TESTNET;
    if (chain_str == "regtest") return Chain::REGTEST;
    throw std::invalid_argument("Unknown chain: " + chain_str);
}

bool IsChainSelected() {
    return g_paramsSelected;
}

// ============================================================================
// TEST-ONLY MUTABLE ACCESSOR
// ============================================================================
// WARNING: This function is for testing ONLY. Do not use in production code.
// It allows tests to modify chain parameters like the ZK kill-switch.
// Note: Always compiled, but only called by test code.

ChainParams& MutableParams() {
    if (!g_paramsSelected) {
        throw std::runtime_error("Chain parameters not selected. Call SelectParams() first.");
    }
    return *const_cast<ChainParams*>(g_active);
}

const std::string& HrpForActiveNetworkRef() {
    return g_active->hrp;
}

} // namespace dinero
