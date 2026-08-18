/**
 * #585 / #588 regression: the ActivateBestChain self-heal must realign the
 * shielded tip marker + state DOWN to the active tip after a disconnect.
 *
 * The regtest invalidateblock leg rolls back ChainDB UTXO only and touches
 * nothing in-memory; the self-heal moves active_tip_ but IsCanonicalStateAligned
 * ignores the shielded marker. So without an explicit arm the shielded tip marker
 * and state stay at the pre-invalidation tip, and VerifyOrBootstrapShieldedTipMarker
 * then trips SAFE MODE -> mining paused -> livelock. #588 added
 * RealignShieldedStateToActiveTipAfterHeal() as the missing seam: once the
 * self-heal has lowered active_tip_, rewind the shielded state + marker to match
 * (idempotent — a no-op when the marker is not ahead).
 *
 * These tests stage the desync synthetically (the owner's spec: "stage marker
 * ahead of active tip, invoke the realign, assert three-view alignment + no
 * SAFE MODE"), because reproducing it through the live self-heal needs a
 * misaligned consensus_utxo_set_ — impractical for a focused unit test, and now
 * unreachable via the invalidateblock route after #590 serialized that path.
 *
 *   test01: marker AHEAD of the active tip -> realign rewinds the marker down to
 *           the active tip, and no SAFE MODE. (Neuter proof: early-return
 *           RealignShieldedStateToActiveTipAfterHeal, or drop its
 *           RewindShieldedStateToActiveTipForStartup call -> the marker stays
 *           ahead -> the require() below aborts. Confirmed RED without the arm.)
 *   test02: marker ALREADY at the active tip -> realign is an idempotent no-op
 *           (marker unchanged, no SAFE MODE) — pins the marker_height <= active
 *           early-out so the arm never spuriously rewinds an aligned node.
 *
 * Checks are exit-nonzero (require/abort), NOT assert() — this test must still
 * gate under NDEBUG (Release/CI).
 */

#include "consensus/block_index.h"
#include "daemon/services/chainstate_service.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

using namespace dinero;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "[FAIL] " << msg << std::endl;
        std::abort();
    }
}

std::filesystem::path MakeTempRoot() {
    auto root = std::filesystem::temp_directory_path() /
                ("din_realign_shielded_after_heal_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

// Register a CBlockIndex at `height` with a distinct hash (varied via nonce so
// two tests in one process don't violate the one-BlockIndex-per-hash invariant),
// and return its hash.
uint256 RegisterTip(uint32_t height, uint32_t nonce) {
    BlockHeader hdr{};
    hdr.version = 1;
    hdr.nonce = nonce;
    CBlockIndex* idx = AddBlockIndex(hdr, height);
    require(idx != nullptr, "RegisterTip: AddBlockIndex returned null");
    require(idx->height == height, "RegisterTip: height mismatch");
    return idx->hash;
}

// test01 — the load-bearing case: a shielded tip marker staged AHEAD of the
// active tip is rewound down to the active tip by the realign, with no SAFE MODE.
void test01_realign_rewinds_marker_ahead_of_tip(const std::filesystem::path& root) {
    const auto db_dir = root / "t01";
    ChainDB db;
    require(db.init(db_dir) == Status::Ok, "t01: ChainDB::init failed");

    ChainWriteToken token = ChainWriteToken::CreateForTesting();

    const uint32_t active_height = 0;
    const uint256 tip_hash = RegisterTip(active_height, /*nonce=*/1);

    // The rewind walks getBlockHashByHeight(active_height + 1); give it a row so
    // it reaches the "no undo above tip -> empty frontier" fall-through rather
    // than erroring out early. The hash value is irrelevant (ReadStoredUndo has
    // no block_storage here, so it returns non-Ok -> empty frontier).
    require(db.putHeightIndex(token, static_cast<int>(active_height + 1), tip_hash) == Status::Ok,
            "t01: putHeightIndex failed");

    // Stage the desync: shielded tip marker AHEAD of the active tip.
    ChainDB::ShieldedTipMarker marker;
    marker.height = 5;             // ahead of active tip (0)
    marker.block_hash = tip_hash;  // irrelevant to the realign (it reads height)
    require(db.putShieldedTipMarker(token, marker) == Status::Ok,
            "t01: putShieldedTipMarker failed");

    auto staged = db.getShieldedTipMarker();
    require(staged.status() == Status::Ok && staged.value().height == 5,
            "t01: precondition — staged marker must be at height 5 (ahead of tip 0)");

    ChainstateService svc;
    svc.setChainDB(&db);
    // Give the rewind a writable frontier path so PersistShieldedState() succeeds
    // and the rewind reaches its marker persist (the assertion under test).
    svc.SetShieldedFrontierPathForTesting(db_dir / "shielded.frontier");

    std::string err;
    require(svc.ForceSetActiveTip(tip_hash, err), "t01: ForceSetActiveTip failed: " + err);
    require(svc.GetActiveTip() != nullptr &&
                static_cast<uint32_t>(svc.GetActiveTip()->height) == active_height,
            "t01: active tip must be at height 0");
    require(!svc.IsInSafeMode(), "t01: precondition — must not be in safe mode before realign");

    // Act: the #588 self-heal shielded arm.
    svc.RealignShieldedStateToActiveTipAfterHealForTesting();

    // THE TEETH: the marker must be rewound down to the active tip. Without the
    // arm it stays at height 5, and this aborts.
    auto after = db.getShieldedTipMarker();
    require(after.status() == Status::Ok, "t01: marker must be readable after realign");
    require(after.value().height == static_cast<int32_t>(active_height),
            "t01: marker must be rewound to the active tip height (0), got " +
                std::to_string(after.value().height));

    // Three-view alignment held without tripping SAFE MODE.
    require(!svc.IsInSafeMode(),
            "t01: realign must NOT enter safe mode: " + svc.GetSafeModeReason());

    db.close();
    std::cout << "[PASS] test01 — marker ahead of tip rewound to active tip, no safe mode\n";
}

// test02 — the idempotent no-op: a marker already at the active tip is left
// untouched (pins the marker_height <= active_height early-out).
void test02_realign_noop_when_marker_aligned(const std::filesystem::path& root) {
    const auto db_dir = root / "t02";
    ChainDB db;
    require(db.init(db_dir) == Status::Ok, "t02: ChainDB::init failed");

    ChainWriteToken token = ChainWriteToken::CreateForTesting();

    const uint32_t active_height = 0;
    const uint256 tip_hash = RegisterTip(active_height, /*nonce=*/2);

    // Marker already aligned with the active tip.
    ChainDB::ShieldedTipMarker marker;
    marker.height = static_cast<int32_t>(active_height);
    marker.block_hash = tip_hash;
    require(db.putShieldedTipMarker(token, marker) == Status::Ok,
            "t02: putShieldedTipMarker failed");

    ChainstateService svc;
    svc.setChainDB(&db);
    svc.SetShieldedFrontierPathForTesting(db_dir / "shielded.frontier");

    std::string err;
    require(svc.ForceSetActiveTip(tip_hash, err), "t02: ForceSetActiveTip failed: " + err);

    // Act: realign on an already-aligned node.
    svc.RealignShieldedStateToActiveTipAfterHealForTesting();

    // Marker unchanged, no safe mode — the arm did not spuriously rewind.
    auto after = db.getShieldedTipMarker();
    require(after.status() == Status::Ok, "t02: marker must be readable after realign");
    require(after.value().height == static_cast<int32_t>(active_height),
            "t02: aligned marker must be unchanged, got " + std::to_string(after.value().height));
    require(!svc.IsInSafeMode(),
            "t02: no-op realign must NOT enter safe mode: " + svc.GetSafeModeReason());

    db.close();
    std::cout << "[PASS] test02 — aligned marker unchanged (idempotent no-op), no safe mode\n";
}

}  // namespace

int main() {
    const auto root = MakeTempRoot();
    test01_realign_rewinds_marker_ahead_of_tip(root);
    test02_realign_noop_when_marker_aligned(root);
    std::filesystem::remove_all(root);
    std::cout << "All RealignShieldedStateToActiveTipAfterHeal tests passed.\n";
    return 0;
}
