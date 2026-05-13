# Dinero v7 Genesis Restart Plan

**Status:** Code purge complete; ready to mine + cut the new chain.
**Predecessor:** Fair Launch v5 (genesis `0000002d6b0abbf955fbf81faa4df1d0349a91c22d92ed9dd31cb4d79390b3d2`, mined 2026-04-11).
**Approach:** Maximum reuse of v5 pipeline. Only consensus-identity fields change. Ring/CT stack physically removed from the live tree; shielded pool parked for possible revival.

---

## 1. Why v7

The live chain at height ~3733 carries 3,474 drifted Utreexo entries from the ring/CT
era. Ring signatures and a stateless accumulator are architecturally incompatible —
ring spends demand ambiguity about which UTXO was consumed, Utreexo demands certainty.
The drift is the inevitable end state, not a bug.

The v4→v5 reset removed some ring/CT code but left the chain ambiguous. The freeze
fork at block 4000 stops new damage without repairing old damage. Mining is paused
by the safety gate.

v7 is a fresh genesis from a codebase that has ring/CT **physically removed** —
not just gated. The narrative and the code match: post-quantum native, no legacy.

---

## 2. Locked parameters

| Field | Value | Note |
|-------|-------|------|
| Genesis inscription | `Dinero: Real Money For Free People - Post-Quantum Native. April 17 2026` | 71 bytes UTF-8. Pin with `static_assert(size()==71)`. |
| Network magic | `0xd1a0c0de` (reused from v5) | Genesis hash + coin type are the real identity separators; magic churn buys nothing when the fleet is wiped. |
| Coin type (BIP44) | `1448'` | was `1447'` on v5. |
| Ports | `20998` (RPC) / `20999` (P2P) | reused from v5. |
| Bech32 HRP | `din` / `tdin` / `rdin` | unchanged. |
| BIP32 purposes | `86'` (Taproot), `88'` (P2MR) | no `77'` (shielded parked). |
| Difficulty | `0x1d31ffce` | reused from v5. |
| Subsidy | 100 DIN / 210,000-block halving / 21M cap | unchanged. |
| Coinbase burn | Single OP_RETURN with double-commitment (inscription in both `scriptSig` and OP_RETURN data) | reused v5 pattern — provably unspendable. |
| BlockHeader | v1, 128 bytes | unchanged layout. |

### External coordination

- **wDIN bridge:** default plan is *unwrap-and-park* — any self-held wDIN gets unwrapped to v5 DIN before Phase 3, contract stays inert on Base. If pre-Phase-3 checks show public float, reopen as a public wind-down announcement.
- **DineroDPI iOS:** deferred. Old app will show "no new blocks" until rebuilt post-v7 with the new genesis hash. Not a blocker.

---

## 3. Feature surface (v7)

**Active from block 0:**
- Taproot (P2TR, secp256k1 Schnorr) — `din1p…` addresses
- P2MR (ML-DSA-65, FIPS 204) — `din1r…` addresses
- Utreexo accumulator
- VWU fee market
- Coinbase maturity: 100 blocks

**Not present / parked / removed:**

| Feature | State | Where |
|---------|-------|-------|
| Ring signatures, ring covenants, CLSAG | **Removed** from live tree | Preserved at `v5.privacy-frozen` tag / `privacy/v5-archive` branch. Do not revive — architecturally incompatible with Utreexo. |
| Confidential transactions (CT) | **Removed** from live tree | Same archive. |
| `sendprivate*` RPCs, `dina1` payment codes, stealth addresses, keyimage/CT-output DBs, freeze-fork machinery | **Removed** | Same archive. |
| Shielded pool (Spartan, commitment tree, nullifier set) | **Code present, parked**. `wallet.shield` / `wallet.unshield` / `wallet.shieldedbalance` RPCs exist but are not wired into a v5 tx builder. Consensus bundle validation is compiled in. | Archive preserves identical copy; revival path is to wire the tx builder + optionally define a real `encrypted_note` format + `binding_sig`. |
| FALCON-512 | Reserved (`activation_height = 0xFFFFFFFF`) | `scheme_registry.cpp`. |
| SPHINCS+-128s | Reserved | Same. |

**No shielded activation at a specific block height.** The pool is compiled in but not advertised. Users who call `wallet.shield` will find it works only against a local note store — it does not build or broadcast a v5 transaction. This is honest: nothing to activate at genesis.

---

## 4. Genesis block specification

```
BlockHeader v1 (128 bytes):
  version         = 1
  prev_block_hash = 0x00…00 (32 zero bytes)
  merkle_root     = hash(coinbase)
  utreexo_root    = <empty forest root — same hardcoded value as v5>
  timestamp       = 1776643200       (2026-04-17 00:00:00 UTC; refresh on mining day)
  difficulty      = 0x1d31ffce
  nonce           = MINED during Phase 2
  reserved[12]    = 0x00…00

Coinbase tx (reuse v5 structure):
  version = 1
  vin[0]:
    prevout   = 0x00…00:0xffffffff
    scriptSig = <height=0> <inscription 71 bytes>
    sequence  = 0xffffffff
  vout[0]:
    value         = 10_000_000_000 una (= 100 DIN)
    scriptPubKey = OP_RETURN <inscription 71 bytes>   (double-commitment)
  lockTime = 0
```

100 DIN are burned into the OP_RETURN output; not added to Utreexo; cannot be spent.

---

## 5. Code changes

Status reflects the p2p-fix tip at commit `0e3c36944` (ring/CT excision) and prior shielded-runtime commit `633cdd66a`.

### 5.1 Consensus identity (bundle + codegen) — TODO for Phase 2

- `docs/chain/genesis_bundle.json` — replace values: `mined_date`, `merkle_root`, `utreexo_root`, `timestamp`, `nonce`, `block_hash`, `coinbase_hex`, `motto`. Keep `premine` section zeroed.
- Run: `python3 tools/gen_constants_from_bundle.py docs/chain/genesis_bundle.json > include/consensus/chain_bundle_generated.h`.
- `src/consensus/genesis_canonical.cpp` — update comment block (lines 8–16) to reference v7.

### 5.2 Chain parameters — TODO

- `include/consensus/chainparams.h` / `src/consensus/chainparams_impl.cpp`:
  - Coin type → `1448`.
  - Network magic stays `0xd1a0c0de`.
  - Confirm no residual freeze-fork / ring / CT activation heights remain (Codex's commit should have cleared these; verify post-mine).

### 5.3 PQ scheme registry — DONE

- `src/consensus/pq/scheme_registry.cpp` already has `ML_DSA_65.activation_height = 0`.
- FALCON-512 + SPHINCS+-128s remain `0xFFFFFFFF`.

### 5.4 Ring/CT deletion — DONE (commit `0e3c36944`)

- CLSAG, ring covenants, hidden-member binding, CTV ring circuit → gone.
- CtOutputIndexDB, KeyImageDB, ring_signature_validator, confidential_validation → gone.
- `sendprivate*`, `methods_wallet_confidential`, confidential RPC handlers, `methods_privacy_status`, `zk_rpc_handlers_context`, stealth RPCs → gone.
- `dina1` payment codes, `confidential_address*`, stealth_addresses, ring_builder, ring_signer, view_key_scanner, zk_wallet_sync, unified_tx_builder → gone.
- Freeze-fork machinery, legacy_private_lane_state → gone.
- 68 files touched in that commit, −19,019 lines net.

### 5.5 Shielded pool — leave as-is

- Code present and compiled. Wallet-side note runtime (`633cdd66a`) in place.
- RPCs exist but tx builder is not wired — users can't actually shield on-chain.
- This is parked-in-place. When/if privacy becomes priority, we wire the builder.

### 5.6 Qt — verify

- Confirm Qt build has no remaining ring/CT references.

---

## 6. Deployment procedure

### Phase 1 — Prepare (local)

- [x] Ring/CT code removed from p2p-fix.
- [x] Shielded pool code preserved and compiled.
- [x] `dinerod` rebuilds cleanly (verified post-excision).
- [ ] Regtest smoke: boot on clean datadir, mine 110 blocks, send Taproot and P2MR transactions, confirm no crashes, Utreexo HEALTHY.
- [ ] Full test suite green (whatever remains after ring/CT test removal).
- [ ] Chainparams coin_type edit committed.
- [ ] Mac local build + Linux cross-build artifacts produced.

### Phase 2 — Mine genesis (single host)

Genesis is mined once, on one host, result committed to `genesis_bundle.json`, deployed to all fleet nodes. Never mine per-node — that produces 5 different hashes.

```
1. On the mining host:
     ./tools/mine_genesis \
        --inscription "Dinero: Real Money For Free People - Post-Quantum Native. April 17 2026" \
        --timestamp <2026-04-17 00:00:00 UTC epoch, or mining-day equivalent> \
        --difficulty 0x1d31ffce \
        --output docs/chain/genesis_bundle.json
2. Regenerate chain_bundle_generated.h.
3. Rebuild dinerod on mining host; BuildCanonicalGenesis() asserts hash matches.
4. Commit: docs/chain/genesis_bundle.json + include/consensus/chain_bundle_generated.h.
5. Tag: v7.0.0-rc1.
```

### Phase 3 — Fleet flip (coordinated)

```
1. Stop all 5 nodes (mac + la + va + mo + cn) via systemctl stop dinerod.service.
2. Archive chain data for rollback kit:
     tar -czf dinero-v5-final-<node>-<date>.tgz <datadir>/{blocks,headers,blockchain,mempool.dat}
3. On each node, wipe:
     blocks/, headers/, blockchain/, mempool.dat, wallet DBs.
   (users must back up 12-word seeds first — v7 uses coin_type 1448, seeds work but produce different addresses → zero balance on v7 by design.)
4. Deploy v7.0.0-rc1 binary.
5. Start nodes. Each loads hardcoded genesis from chain_bundle_generated.h.
   BuildCanonicalGenesis() asserts or the daemon refuses to start.
6. Verify: all 5 nodes report identical `getblockhash 0`.
7. Update seed addnode list (strip stale external-IP self-loops).
```

### Phase 4 — Smoke (first hour)

```
1. Mine 110 blocks from one node.
2. All nodes sync to height 110.
3. Taproot round-trip: generate din1p… → send → confirm.
4. P2MR round-trip: generate din1r… → send → confirm.
5. Mixed Taproot+P2MR tx confirms.
6. wallet.getbalance correct.
7. Utreexo: HEALTHY, zero drift.
8. Mining safety gate: absent (feature removed).
9. Explorer shows new genesis + recent blocks.
```

### Phase 5 — External sync

```
1. wDIN action — default: self-unwrap + park contract.
2. DineroDPI iOS: rebuild with v7 genesis hash when convenient.
3. dinero-releases: rebuild + sign + publish macOS + Linux artifacts.
4. GitHub README: v7 genesis hash, feature activation table.
```

---

## 7. Rollback

Rollback target: v5 chain at the height the fleet stops at for Phase 3 (~3733, safety-gated).

Rollback kit (archived before Phase 3 step 2):
- v5 binary (last tagged pre-v7 release).
- Per-node `blocks/`, `headers/`, `blockchain/`, `mempool.dat` tarballs.
- v5 wallet DBs (pre-wipe snapshot).

Rollback procedure:
```
1. systemctl stop dinerod on all 5 nodes.
2. Extract v5 archive into datadir.
3. Deploy v5 binary.
4. Start. Chain resumes at last v5 height, safety gate still engaged.
5. Investigate failure before retry.
```

Reviving shielded or ring code is *not* part of rollback — those live at
`git show v5.privacy-frozen:<path>` and branch `privacy/v5-archive` (locked,
can't be force-pushed, can't be deleted).

---

## 8. Risk matrix

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Genesis hash mismatch between fleet nodes | Low | Fatal | Single-host mining in Phase 2; all nodes load the same hardcoded value; `BuildCanonicalGenesis` asserts on mismatch. |
| Non-determinism in empty Utreexo root | Low | Fatal | Hardcode the value (v5 approach). |
| Shielded code rotting in-tree | Medium | Low | Accepted. Archive has the canonical copy. If the in-tree copy drifts to not-compile, delete it and revive from archive on demand. |
| Public wDIN float exists and we missed it | Low | Reputational | Pre-Phase-3: confirm supply on Base vs. self-held. If delta, switch to announce + wind-down. |
| DineroDPI users see stale chain | High (by design) | Low | Accepted. Rebuild app post-v7. |
| Seed node self-loop at bootstrap | Low | Recoverable | Strip stale external-IP addnodes from fleet config before Phase 3 step 7. |
| Wallet users assume balance carries over | Medium | Support burden | Comms: "seed works, addresses differ (new coin type), old-chain balances do not migrate." |

---

## 9. Pre-genesis checklist

- [x] Ring/CT code removed from active branch.
- [x] `dinerod` rebuilds and starts.
- [ ] Coin type `1448` edit landed in chainparams.
- [ ] Regtest smoke passes (boot + mine + Taproot + P2MR transfers + Utreexo healthy).
- [ ] Full unit-test suite green on the trimmed codebase.
- [ ] wDIN supply on Base confirmed = self-held (or migration plan opened).
- [ ] Self-held wDIN unwrapped back to v5 DIN.
- [ ] Rollback kit archived for all 5 nodes.
- [ ] User's own wallet seed backed up.
- [ ] `tools/mine_genesis` ready and tested on a throwaway bundle.

When every box is checked, Phase 2 is a `make && mine` afternoon and Phase 3 is a
10-minute coordinated restart.

---

## 10. One-line summary

v7 reuses the v5 genesis pipeline end-to-end; ring/CT is physically removed from the
active tree, shielded stays compiled but parked. The chain that lands is Taproot +
P2MR + Utreexo + VWU — post-quantum native, transparent, no privacy promises it
can't keep.
