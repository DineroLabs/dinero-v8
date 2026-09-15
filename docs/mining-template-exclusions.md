# Request-local mining template exclusions

`getblocktemplate` accepts an optional `exclude_txids` array alongside `address`:

```json
[{"address":"<mining address>","exclude_txids":["<64-character transaction ID>"]}]
```

The array accepts at most 10,000 entries. Each entry must be exactly 64 hexadecimal
characters; case and duplicates do not matter. An omitted or empty array preserves
normal selection. Invalid input returns the RPC's existing embedded `error` response.

The assembler removes matching transactions and their transparent descendants
from its selected candidates before building any commitments. It recalculates
fees and builds the coinbase, DNRW, DNRF, DNRS, merkle root and Utreexo root through
the existing assembly and validation paths. Exclusions do not evict transactions
or add them to the mempool's persistent template quarantine. Selection after
filtering does not refill capacity with additional mempool transactions.

This supports the SV2 pool's per-transaction proof-failure recovery. The pool must
still check that exclusions were honored and that the parent/height did not change;
older daemons silently ignore this request field. Ordinary `getblocktemplate` and
`mining.getjob` retain their existing default behavior. This extension does not
change the SV2 JD protocol or enable nonempty JD templates.

DNRS commits to the complete predicted post-block shielded state, including anchor
history, which advances even for empty blocks. A parent root or current state digest
cannot substitute for the assembler's prediction. Keeping an excluded transaction
in the mempool also does not guarantee its validity at later heights: the existing
shielded anchor rules can make a deferred unshield invalid after another shielded
tree update. Exclusions preserve the existing validation rules.

## Tests

```sh
cmake --build build --target dinerod test_block_template_determinism
ctest --test-dir build --output-on-failure \
  -R '^(BlockTemplateDeterminism|MiningTemplateExclusions)$'
DINEROD="$PWD/build/dinerod" python3 tests/integration/test_state_commitment_mining.py
```

The Python regression failed against the pre-change v8.1.12 release because
`exclude_txids` was ignored. With the extension it verifies malformed parameters,
uppercase/duplicate/unknown IDs, empty exclusions, fee accounting, DNRS changes,
and mempool isolation. It externally mines and connects a filtered block, an
all-excluded block, and a later block containing the previously excluded shield.
The C++ regression checks descendant closure with out-of-order candidates while
preserving ancestors, siblings, independent zero-input transactions and order.

Companion SV2 process test:

```sh
DINEROD_BIN=/absolute/path/to/patched/dinerod cargo test --locked \
  -p dinero-sv2-pool --test shared_split_e2e \
  shared_pool_recovers_from_bad_proof_with_daemon_rebuilt_dnrs \
  -- --ignored --nocapture
```

It uses a local forwarding proxy to fail an otherwise valid shield input proof,
then verifies that the actual pool requests exclusions, mines the unshield in a
DNRS-valid shared block and leaves the failed shield transaction in the mempool.
An old daemon fails this test at the pool's exclusion-support check.

Recorded local validation (macOS arm64, September 15, 2026):

- `dinerod` and the C++ test target build successfully.
- Both CTest entries pass; the C++ binary reports 8 passed and 7 existing
  placeholder tests skipped.
- Existing external GBT and `mining.getjob`/`mining.submit` DNRS acceptance,
  restart persistence and dormant-activation controls pass.
- Both real SV2 process tests pass: ordinary mixed shield/unshield inclusion
  and injected-proof-failure recovery.
- Production deployment and Linux qualification have not been performed.
