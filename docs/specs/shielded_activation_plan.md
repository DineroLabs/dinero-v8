# Shielded Pool — Mainnet Activation Plan (historical)

> **Historical record.** Activation has occurred and the deployed height/epoch
> rules are normative in
> [`shielded_protocol_v1.md`](shielded_protocol_v1.md). Forward-looking
> instructions in this plan are no longer operator guidance.

**Status:** drafted 2026-04-27 (initial), revised 2026-04-27 to drop
the testnet phase. Owner: project (solo operator).

The original draft of this plan called for a multi-week
testnet → soak → mainnet pipeline. With one operator, one fleet, and
the entire 25/25 shielded ctest suite green on regtest, the testnet
phase added calendar time without adding coverage. This revision
collapses the rollout to a direct mainnet activation with a small
deploy buffer and a fix-forward policy on the live chain.

**Retired coin type:** coin_type 1447 is permanently retired; any reference outside historical archives is a bug.

---

## Decision summary

- **Mainnet `shielded_activation_height = 8650`** (chainparams_impl.cpp:78).
  Tip at decision time was 8602 → +48 blocks. At typical ~10-min
  mainnet cadence that's ~8 hours: enough to deploy the new binary
  to all 4 servers, watch a few blocks land cleanly, then the
  shielded gate flips automatically.
- **Testnet stays parked at `UINT32_MAX`.** No live testnet today;
  bringing one up just to mirror what mainnet will do under the same
  operator was redundant. If testnet is ever brought back online
  (cross-impl parity, third-party testers), pick a real height there.
- **Fix-forward on the live chain.** Bugs found post-activation get
  fixed on `p2p-fix`, redeployed across the fleet, and the chain
  rolls forward. Solo operator + solo fleet means a chain split
  between two of our own nodes is recoverable manually; there is no
  external party to drag along a soft fork.

---

## Step 1 — Pick mainnet activation height (DONE)

Done in this commit. `chainparams_impl.cpp:78` now reads `8650`.
Testnet height reverted to `UINT32_MAX` in `:241`.

---

## Step 2 — Cut a release (PENDING)

The activation height ships in a daemon release that gets pushed
to all 4 servers and one Mac dev box.

Required:

- Next user-facing tag: **v2.1.32** (carried on the dinero-qt repo;
  current latest tag is v2.1.31). Dinero-Coin daemon doesn't tag the
  v2.x.y series itself — it lives on the bundled app side.
- Build artefacts:
  - Notarised macOS arm64 bundle (`dinero-qt.app`) — already bundles
    the daemon at `Contents/Resources/dinerod` per memory; rebuilds
    must replace it.
  - Linux x86_64 server `dinerod`. Cross-build untested at present;
    do a smoke build on Mac with the Linux toolchain before deploy.
- Tag commit on `Dinero-Coin/p2p-fix`.
- Upload to `github.com/DineroLabs/dinero-releases`.
- Release notes: Path C consensus + Phase 5 surface + mainnet
  shielded activation @ block 8650.

dinero-qt also needs a release for the new 🛡 Shielded tab. Latest
on `qt-main`: `c62e02a` (post-QSettings persistence).

---

## Step 3 — Deploy + activation (PENDING)

Deployment workflow per memory's standard pattern (Server Deployment
section of MEMORY.md):

```
cd /root/Dinero-Coin
git fetch origin p2p-fix && git checkout p2p-fix && git pull origin p2p-fix
cmake --build build --target dinerod -j$(nproc)
cp build/dinerod build/dinerod.backup.pre-<commit>
systemctl stop dinerod && sleep 3 && systemctl start dinerod
```

Run this on LA, VA, MO, CN sequentially (not in parallel — keep at
least 3 nodes serving while one is restarting).

Then watch the chain advance to 8650. Once the gate flips:

- Verify `wallet.shieldedbalance` returns the active state (no
  `shielded_not_active` error) on the dev Mac.
- Try a shield → mine → unshield round-trip.
- Try an addressed transfer to a new shielded address.

If anything misbehaves, fix on `p2p-fix`, redeploy, observe.

---

## Step 4 — Live monitoring (PENDING, ongoing)

Things to watch on the live chain:

- **Consensus reorgs** caused by shielded txs — must be 0. Any reorg
  is a chain-split risk.
- **dinerod crashes** — must be 0. Any crash on a malformed shielded
  bundle is a DoS vector to fix immediately.
- **ZK prover wall-clock on Linux** — first-time benchmark on the
  live fleet. Spend + output proofs are ~250ms each on Mac M-series;
  Linux x86 / arm64 numbers are unmeasured. Watch for slow blocks.
- **Receive-scan latency** — `ProcessConfirmedBlock` runs
  `kScanAccounts = 4` ivk trial-decrypts per shielded output. A heavy
  block could stall the wallet thread. If it bites, gate the scan
  behind a height checkpoint or move to a Bloom pre-screen.

Block explorer / RPC commands for monitoring:

```
dinero-cli getblockcount                          # tip height
dinero-cli wallet.shieldedbalance                 # local wallet view
dinero-cli getrawmempool                          # any stuck shielded txs
dinero-cli getblock $(dinero-cli getbestblockhash) 1  # latest block tx list
```

---

## Step 5 — DineroDPI iOS shielded UX (PENDING, post-activation)

After activation is stable. Mirror the dinero-qt 🛡 Shielded surface
in SwiftUI. No daemon-side changes; same RPCs.

---

## Risk inventory (revised for direct-to-mainnet)

- **Consensus bug** would split the 4 nodes. Recovery: invalidate
  the divergent block on the minority fork(s), redeploy the fix,
  manually pick the winner. Solo operator can resolve this within
  hours; no external coordination needed.
- **Wallet bug** could leave a shielded note unspendable. Forensic
  recovery via `DeriveNoteSpendKey(rcm)` from the on-chain
  encrypted_note. Probably recoverable in all cases short of a
  non-deterministic prover bug.
- **DoS bug** from a malformed bundle — daemon crash. Fix is small,
  redeploy fast.
- **No external testers** — the four-server fleet is the entire
  test population. Cross-impl parity (DineroDPI iOS) lands
  post-activation per Step 5.

The "fix on live chain" stance accepts these risks because the
blast radius is the operator's own coins on the operator's own
fleet. If external users join later, the pre-activation calendar
will need to grow back in.

---

## Decision log

- **2026-04-27 (initial):** Picked testnet `shielded_activation_height = 10000`.
  Plan called for ≥7-day testnet soak before mainnet pick.
- **2026-04-27 (revised):** Reverted testnet to `UINT32_MAX`.
  Picked mainnet `shielded_activation_height = 8650` directly,
  fix-forward policy. Driver: solo operator with no external
  testers; testnet phase added calendar time without coverage.
