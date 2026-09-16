# Compact Spartan format review and activation/Utreexo qualification plan

Status: **research only; no format activation or daemon integration**, 2026-09-16.
This follows the [native prototype](shielded-compact-spartan-proposal.md).
`Vcompact` and `Hcompact` below are symbols, not assigned protocol values.

## Review conclusions

1. Omit only the ordinary proof's zero error-commitment row encodings. Restore
   the exact circuit-derived count of 33-byte identity encodings before the
   unchanged verifier consumes the proof. Keep the E dimensions, circuit hash,
   evaluations and transcript inputs. Keep `require_zero_error=true`; this is
   not the relaxed/folding proof format.
2. Validate the entire layout against a trusted circuit before allocating the
   expanded buffer. Every count must equal the expected count, not merely fit a
   maximum. Reject trailing bytes, unsupported profiles, mixed layouts and
   nonzero omitted rows. Never select a circuit from wire dimensions.
3. The first prototype checked retained points/scalars only indirectly through
   the historical verifier. The reviewed experimental decoder adds explicit
   canonical checks: every scalar is a big-endian integer in `[0,n)`, where n is
   the secp256k1 group order; every point is exactly 33 zero bytes (identity) or a
   valid 33-byte compressed curve point. No reduction modulo n, point repair,
   normalization or alternate infinity encoding is permitted. Zero scalars and
   identity points remain legal *encodings*; proof validity is a separate check.
   These requirements apply only to this experimental compact codec, not to
   historical proof parsing. No historical consensus vulnerability is asserted.
4. A successful decode is not a successful proof. The original circuit,
   transcript, public-input, authorization, range-proof, value-conservation,
   nullifier and anchor checks remain required. Canonical but false claims must
   still fail verification without changing state.
5. Require a separately signed transaction version and exactly one accepted
   compact encoding in that version. Do not accept compact and expanded forms
   interchangeably under v6. Proof bytes affect v6 txid but are not directly
   covered by its binding signature, allowing unauthenticated repacking if both
   encodings were accepted there. The signed version prevents that particular
   conversion; it is not a general proof of transaction non-malleability.
6. Keep expansion inside a read-only verification view. The original compact
   transaction supplies txid, wtxid, merkle/DINW commitments, outpoints, wallet
   records, Utreexo leaves, pool templates and persisted transaction bytes.
   Never expand in place, hash the verification view, or cache acceptance without
   the version, circuit/profile and applicable consensus context.
7. Lower byte size does not lower proof-verification work. Preserve independent
   spend/output/proof-count limits. Bound expanded bytes and decoder work from
   trusted circuit dimensions before construction. Benchmark the added point
   parsing; no additional proving-speed or paid-fee reduction is established here.

### Decisions still requiring protocol review

- Assign the transaction version and proof/container discriminators. `DZE1` is
  an experimental file tag; do not reserve or activate it by implication.
- Specify the complete new transaction serialization, fee marker, canonical
  bundle length encodings and signature preimage. A signature-domain change is
  a review decision; the existing binding preimage already includes version.
- Preserve v5/v6 historical block validity. Whether new legacy transactions
  remain relayable after activation is a separately documented policy decision.
- Specify activation and rollback behavior on every network. Current unchanged
  nodes reject compact candidates: this is a consensus compatibility change
  requiring coordinated activation, not a wallet-only deployment.
- Extend the byte/resource profile explicitly. Adding only `IsShieldedVersion`
  would miss v6-specific serialization, size limits and wallet checks.

## Integration inventory (no changes to these paths in this study)

| Boundary | Existing implementation to review | Required invariant |
| --- | --- | --- |
| Version and identity | `include/primitives/transaction.h`; `src/primitives/transaction.cpp` | Compact bundle enters base and witness serialization; exact bytes determine identities |
| Wire parsing | `src/primitives/transaction_serializer.cpp`; `src/consensus/shielded/shielded_serialization.cpp` | One unambiguous encoding, bounded lengths before allocation, full consumption |
| Binding signature | `src/consensus/shielded/binding_sig.cpp` | Version/transparent envelope/value balance/commitments bound; conversion invalidates signature |
| Proof dispatch | `src/consensus/shielded/shielded_validation.cpp`; `src/zk/zkvm/` | Contextual version/profile selection; temporary expansion; unchanged verifier and zero-error guard |
| Resource envelope | `include/consensus/shielded/resource_limits.h` | New version explicitly covered by byte, weight, proof-count and package limits |
| Wallet construction | `src/wallet/shielded_wallet_ops.cpp`; `src/wallet/shielded_wallet_runtime.cpp` | Compact size used before final fee/signing; one final immutable txid |
| Mempool and policy | `src/daemon/mempool.cpp`; `src/daemon/validation_mempool.cpp`; `src/policy/mempool_policy.cpp` | Admission/template selection use the actual candidate height; revalidate after reorg |
| Connect/replay | `src/consensus/block_validation.cpp`; `src/consensus/reindexer.cpp` | Same validation/identity rules on connect, disk replay and reindex |
| Output discovery | `src/consensus/shielded/shielded_output_feed.cpp`; wallet scans | New version visible to recipients without rewriting transaction bytes |
| Full node/CSN/Utreexo | chainstate connect/disconnect, delta and proof relay paths | One outpoint/leaf identity; matching committed roots and undo |
| Mining/pool | daemon template exclusions/DNRS; separate `dinero-sv2-pool` repository | Zero transparent inputs need no transparent proof; every output uses final compact txid |

This inventory is a starting checklist, not a claim that every version dispatch
site has already been changed or audited. Search for direct v6 comparisons as
well as shared predicates when implementing.

## Test matrix to implement with dormant integration

**All tests in this section are designed, not implemented by this review.**
Use ephemeral regtest datadirs and an explicit test-network Hcompact. Mainnet
acceptance must never depend on a local enable/disable flag. Each test must
prove its precondition and assert its postcondition; timeout/log silence alone
is not a success oracle.

### A. Activation and historical compatibility

| Case | Setup | Required result |
| --- | --- | --- |
| A1 disabled | Hcompact unset; genuine signed Vcompact shield/unshield/transfer | Reject at all heights and leave all consensus state unchanged |
| A2 boundary | Same valid bytes, candidate heights H−1, H, H+1 | Reject at H−1; accept at/after H; never infer activation from the tip alone |
| A3 mempool boundary | Tip H−2 then H−1; use next-block height | Reject then admit; template/ConnectBlock agree on acceptance at H |
| A4 rollback | Admit at tip H−1; reorg back below boundary | Evict or quarantine compact transactions and descendants; exclude from preactivation template; safely re-admit once eligible |
| A5 historical replay | Fixed v5/v6 blocks on both sides of H; restart/reindex | Preserve all historical verdicts, txids, state roots and persisted bytes |
| A6 wrong format | Full proofs in Vcompact; compact proofs in v5/v6; mixed spend/output encodings; unsupported/private-covenant profile | Reject even if expanded proof is mathematically valid; no fallback decoder |
| A7 old peer | Relay/mine Vcompact using upgraded peers; send to unchanged 8.1.13 peer | Record explicit rejection; do not count old-node acceptance as the success criterion |
| A8 signatures | Change only signed version, envelope, balance or commitments; reuse signature | Reject; owner must construct and sign a new transaction for conversion |
| A9 economic fields | Tamper explicit fee/marker, conservation equations and wallet size estimates | Either signature or conservation/serialization validation rejects, as specified; no value creation or silent fee reinterpretation |
| A10 cached verdicts | Verify at H, reuse same candidate/cache at H−1; switch circuit/profile | Reject under new context; cached proof success cannot bypass contextual checks |

Boundary assertions must cover RPC admission, P2P, block templates,
ConnectBlock, stored-block processing and reindex. Use the actual block height
being validated, not current wall-clock time or a globally cached activation bit.

### B. Serialization, signatures and resource budgets

- Commit byte fixtures for real shield, unshield, 1-in/2-out transfer and maximum
  supported spend/output counts. Record original/compact/expanded proof bytes,
  transaction bytes, txid/wtxid, signing preimage, circuit hash and expected
  errors. Compare **fixed bytes** on Linux x86_64, Linux arm64 and macOS arm64;
  do not demand deterministic equality between independently randomized proofs.
- Check serialize/deserialize/serialize equality and exact input consumption.
  Exercise zero, boundary, oversized and nonminimal length prefixes at the
  transaction, bundle and individual proof layers. Embedded truncation must not
  borrow bytes from the following proof/field. Require rejection before state
  mutation and before untrusted-size allocation.
- Demonstrate that a canonical altered scalar/point can decode and still fail
  the unchanged verifier. Reject invalid curve points, scalars >=n, wrong
  circuit hash, every inconsistent dimension/round count, and nonzero-error
  relaxed forgeries. Test both compact profiles and every scalar/point section.
- Resource cases: exact proof-count limits and +1; 8 block proofs and 9; exact
  transaction/block/package byte and weight limits and +1. Failed accumulation
  must leave counters unchanged. Malformed compact proofs must not undercount
  work; smaller bytes must not permit extra verification slots.
- Wallet sizing: measure compact serialization including variable-length
  prefix changes, settle the fee, sign the final envelope, publish the final
  txid once. Test fee adjustment, change/no change and amount boundaries. Count
  prover invocations to preserve the earlier one-pass unshield optimization.

### C. Real unshield and signed-child Utreexo lifecycle

Extend `test_shielded_auth_relay_lifecycle.sh` with **two upgraded full nodes**,
one bridge/CSN and an unchanged legacy peer used only for A7. Reuse the
persist/reorg checks in `test_csn_shielded_reorg_invertibility.sh` and the
reindex-equivalence harnesses. Assert all peers have reached the same named
block hash before comparing them; volatile peer counts are not state digests.

1. Fund and mature a transparent coin with a safe maturity margin; shield it,
   confirm the note, then create a signed Vcompact unshield at/after H. Obtain
   the final raw transaction and compute its txid independently from those
   exact bytes. Capture proof/output position and shielded state before/after.
2. Relay to the second full node and mine through the intended template path.
   Require block inclusion of the exact txid and raw bytes. Compare UTXO view,
   forest/stump commitment, shielded root, anchor history, nullifiers and
   consensus fee/merkle/DINW/DNRS commitments across full node, bridge and CSN.
3. Compute `HashUTXOV2(final_compact_txid, vout, amount, script, inclusion_height,
   false)` independently. Require an inclusion proof of that leaf. A leaf using
   the expanded-view txid, different vout/amount/script/height must fail against
   the same root. Historical/current proof requests must name their height/root.
4. Create a **signed transparent child** spending this exact compact outpoint;
   relay and mine it. Check script validation, miner fee and recipient output.
   The spent unshield output must disappear from the current UTXO set and
   current accumulator proof service. Its previously captured proof must still
   verify against the historical root: current spentness is not historical data
   loss. A second spend must fail without changing any state.
5. Restart full node and CSN after parent confirmation and again after child
   confirmation. Require the same bytes, identities, spentness and state
   commitments at each named tip; verify the proof service agrees.
6. Disconnect the child block: restore the parent's output/leaf and proof.
   Disconnect the unshield block: remove its transparent leaf and undo the
   shielded spend/nullifier. Reconnect an alternate branch, then the original
   branch. At every step compare roots and state digests to clean replay.
7. Cross H with the same reorg; check A4 and descendant handling. No compact
   transaction may re-enter an H−1 template from a restored mempool entry.
8. Reindex one full node from blocks; rebuild/resume CSN from a checkpoint plus
   kept deltas. Require equality at the same block hash. Exercise checkpoint
   reconstruction separately from compact proof expansion; neither may alter
   the other's commitment bytes.

Required negative controls: substitute the expanded-view txid in one leaf;
retain the spent leaf; omit disconnect restoration; skip the activation check.
Each isolated mutation must make its corresponding test fail, then be removed.
A toy forest add/remove or unsigned child is insufficient for this gate.

### D. Pool, templates and recovery

- Shielded-only unshield has zero transparent inputs. Confirm no malformed
  transparent proof RPC, template refusal or idle mining loop; validate shielded
  work through its own lane. Shielding transactions still prove their actual
  transparent inputs; children of an unshield refer to its compact txid.
- Fault-inject a bad per-transaction proof. Daemon excludes that transaction and
  descendants and rebuilds fees, merkle/DINW/DNRS and Utreexo commitments. Good
  transactions remain mineable; rejected transactions never mutate chainstate.
- Try same-template parent/child only on a protocol path that explicitly
  supports it. Mine in successive blocks otherwise. Solo/JD nonempty-template
  protocol work remains a separate gate; this format work must not assume it.
- Reorg between template creation and submission, then rebuild for the new
  height/roots. An expired template may fail; the next eligible template must
  recover and preserve all commitments.

## Evidence and rollout gate

For each run retain commit IDs, compiler/architecture, network parameters,
activation values, binary hashes, ctest/JUnit results, sanitizer/fuzzer logs,
fixed vectors, named tips and state/proof receipts. Record skipped/unimplemented
coverage explicitly. Register new ctests in an executing CI lane and verify
`scripts/ci/check_mandatory_tests_execute.py`; do not silence it with an exemption.

This review's deterministic malformed-input tests execute inside the existing
mandatory `SpartanSoundness` test. The optional fuzzer targets the experimental
codec only. A codec-only sanitizer pass does not qualify uninstrumented crypto,
wallet or daemon code. Full Linux sanitizer qualification, independent protocol
review, dormant integration and the A–D matrix remain production gates. Choose
an activation height and coordinated rollout only after those pass and the
owner authorizes activation.
