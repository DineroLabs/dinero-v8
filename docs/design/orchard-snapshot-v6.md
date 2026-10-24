# Orchard snapshots for full and compact nodes

Status: implemented on source `683daa1ca`, with a source-selected consensus
gate. Full and compact container export/import, authenticated atomic storage
publication, fresh-store bootstrap, post-base validation/reorg, restart and
exact-base marker recovery are wired into the daemon service. Tests explicitly
activate the new rule on regtest. The owner-approved v8.2.0 candidate now selects
DNORST02 at mainnet H150,000, together with Orchard. Testnet/regtest defaults
remain dormant. Scheduling the candidate is not release qualification.

Both full and compact/mobile nodes are required. This includes NodeCore on
iOS/macOS and DineroDPI on Android. A successful component test is not a
receipt for a production bootstrap, fleet qualification or either mobile SDK.

## Authentication and the coinbase cycle

The existing `DNORST01` state root covers Orchard frontier/root/size/balance,
canonical nullifier and anchor-set digests/counts, and the legacy retirement
record. It does not cover the catalog's transaction, legacy-coin or
nontransparent-output membership roots. A catalog's local checksum cannot
supply that missing chain authority.

The current catalog includes the current coinbase txid. Inserting its
post-block root into that coinbase would create a cycle:

```
coinbase bytes -> coinbase txid -> current catalog root -> coinbase bytes
```

The explicit `DNORST02` rule commits the complete canonical **parent** catalog
record instead. Its preimage is the existing Orchard state preimage with the
eight-byte domain changed from `DNORST01` to `DNORST02`, followed by
`SHA256(parent_catalog.Encode())`. Parent domain, activation, height and block
hash must match the state-root context. The current block hash remains absent
from the preimage.

This binds the parent's three roots and counts, maturity-leaf rule, stump,
domain, height, block, parent, work, previous-record digest and undo digest.
`DNORST01` remains byte-identical. Mining, full/compact connection, inverse
validation and detached replay use the same height selector. The new
`orchard_catalog_commitment_activation_height` has a `UINT32_MAX` struct default;
mainnet explicitly selects H150,000. Testnet/regtest remain dormant unless
explicitly configured in source/tests. A scheduled height must be at or after Orchard activation.
The dormant profile preserves existing consensus fingerprints. Scheduling a
cutover requires separate coordinated deployment, including private histories.

A v6 verifier authenticates the complete base body against the selected base
header and verifies `DNORST02` using the imported logical state and parent
catalog. It resolves the base block's inputs using the parent catalog and
its exact Utreexo proof, runs the genuine block coin checks, computes the
compact stump transition and conventional undo, and derives the successor
catalog through `AdvanceOrchardCatalog`. Every byte of that successor record
must equal the claimed base catalog. Merely checking the claimed base catalog's
local checksum or stump is insufficient.

The parent and base headers, work, burial ancestry and branch MTP lookup are
inputs from the caller's validated selected chain, never imported authority.
The caller must keep network parameters fixed and recheck selected-chain
ownership through publication. The detached section verifier grants no live
owner, validated-history status or durable state by itself.

## Container and section encoding

All integers are little-endian. `blob` means a uint64 byte length followed by
exactly that many bytes. Hashes use their 32 raw wire bytes. The section is
inside the v6 container and is covered by that container's checksum.

The outer layout is a canonical 68-byte `UTXO` header (version 6, base hash,
height, coin count, authenticated base timestamp and zero reserved field),
sorted transparent coin records, a u64 section byte length, the section below,
and a 32-byte SHA256 footer over all preceding bytes. Integers are explicitly
little-endian. Compact files have zero coin records. Profile mismatches refuse.

| Field | Encoding |
| --- | --- |
| Magic, section version, profile | `ORCH`, uint32 `1`, uint8 `1=full / 2=compact` |
| Orchard state | height u32, block hash, anchor, balance u64, tree size u64, frontier blob |
| Retirement | network u8, genesis, branch u32, activation u32, legacy epoch u32, boundary parent, legacy composite root, retired value u64, tree root, tree size u64, nullifier count u64, selected height u32, selected block, selected parent |
| Orchard nullifiers | count u64, count raw hashes in strict raw-byte lexical order |
| Orchard anchors | count u64, count pairs of raw hash and positive reference count u64 in strict raw-byte lexical order |
| Parent catalog, base catalog | two canonical `DNOCS02` blobs |
| Catalog nodes | count u64, then `(node hash, node blob)` in strict raw-hash order |
| Base body | complete Orchard block wire blob, including its exact Utreexo payload |
| Accumulator | full serialized forest blob, or exact canonical base stump blob |

Full containers carry complete transparent UTXO records. Every ordinary coin
must correspond to a distinct live forest position, and every live position
must be covered. Legacy height/coinbase metadata must match the authenticated
catalog. The compiled genesis outputs are the sole exact-value exception:
genesis seeding retains these rows without inserting them into Utreexo.
Confidential records are refused rather than silently dropping uncommitted
fields. Compact files omit coin records and the full forest entirely.

Catalog nodes must be exactly the union reachable from the parent and base
records' six roots. Missing off-path nodes, corrupted hashes, malformed Patricia
paths, wrong kinds, wrong counts, duplicate keys and unreachable extras refuse.
Traversal uses at most 257 pending frames and checks the complete tree, rather
than relying on a few successful lookups. Shared nodes are serialized once.

Logical nullifier/anchor commitments reproduce ChainDB's `ONF1` and `OAN1`
encodings, including terminal counts and anchor reference counts. Raw nullifier
owner hashes and pre-base undo are excluded from the logical snapshot. An
importer must establish and enforce a reorg floor at the base before assigning
imported nullifiers their base owner. Post-base transitions retain their actual
owners and undo. No synthetic history may be advertised as independently
validated history.

The codec has a 1 GiB total section ceiling, a 4096-byte frontier
ceiling, a 32 MiB base-body ceiling and an 82-byte catalog-node ceiling. It
validates lengths before copying and rejects trailing bytes. These are local
resource policies, not consensus rules. The transport checks actual file size
before allocations; the ceiling is not a 1 GiB resident-memory guarantee.
Production capture, file import, baseline restart decoding and transition
overlays use a disk-backed catalog, not a node map. Sealed inventories share
immutable ownership. A small span-codec API still supports RAM fixtures; a
section cannot combine a RAM map and a disk inventory.

The disposable SQLite inventory has a 1 MiB pager, mmap disabled, fixed three
prepared statements and 1,024-record write batches. Reachability uses a second
disk inventory for visited hashes and at most 257 traversal frames. Every read
checks its process-local MAC, node hash and canonical node encoding; final walks
also check physical record counts. A cached structural walk is keyed by all
six roots/kinds/counts and still scans every record for corruption. It caches no
chain, domain, body or commitment authority. An installer still authenticates
the actual block and selected chain before publication.

Mobile callers provide their own writable workspace instead of relying on
Android's system temporary directory. Each spool exclusively creates a random
private child there; normal final-close removes only that child's known SQLite
files and directory, never caller data. Scratch writes are not fsynced or
reopened as durable authority; canonical ChainDB publication remains synced.
An abrupt process exit can leave disposable scratch files; they are not
discoverable import candidates and confer no state ownership.
Callers need temporary disk space beyond the snapshot file: a proof round-trip
can retain two catalog inventories, plus a reachability inventory during a
walk. Disk/I/O failure refuses the operation; it never switches to an unbounded
RAM catalog. Fixed pager memory trades additional I/O and latency for lower RSS.

Outer import/export streams the section. Immutable proof installation and
restart decoding stream 1 MiB database chunks, preserving SHA256 identity and
exact framing. Recovery compares streamed proof digests instead of retaining
encoded inventories. Full forest decoding rebuilds once; export emits the
canonical leaf-only frame directly without serializing its internal cache.
Coin coverage uses a disk-backed live-leaf position index, a bit per leaf and
one metadata-tree walk instead of rebuilding a full forest/index, a per-coin
ordered set and random catalog lookups. Canonical root auditing uses the
existing stump carry algorithm, with ZERO_HASH for each canonical tombstone,
and checks inline roots, exact tombstone ordering/coverage and the claimed
stump. Duplicate live leaves refuse. Pre-canonical flags retain the original
strict decoder rather than reinterpreting historical deleted-root semantics.
This temporary auditor is not a full-node owner; live full-node restore still
uses the strict forest decoder. Nullifier/anchor vectors and
full-profile coins/forest remain resident: this is not constant-memory support
for arbitrary history, nor a cap on the combined mobile application's heap.

## Runtime lifecycle

Export captures one selected state under the activation owner and verifies it
against validated headers. A sibling temporary file is written, reread,
synced and atomically renamed. Errors before publication preserve the existing
destination; a post-rename durability error explicitly reports publication.

Import screens format/activation, profile, selected ancestry and burial before
reading the body. `OrchardSnapshotStore` privately authenticates the complete
section and coin inventory. It stages immutable headers, catalog nodes, proof
chunks and the synced base body before one final synced batch selects the
Orchard sets/state, retirement receipt, catalog records, full coins/checkpoint
or compact binding, journal, markers, base proof pointer and reorg floor.
Unselected preparation records cannot enroll an owner. An uncertain final
commit or failed post-commit publication stops service through safe mode.

The full publication preserves the existing UTXO object's address, including
borrowed validator, mempool and bridge providers. Compact bootstrap is
configured before initialization and never exposes a temporary full owner.
Fresh READY layout creation cannot replace or silently migrate an existing
database. An explicit fresh-factory marker is consumed with genesis or the
snapshot head; an unmarked empty READY store or a surviving non-genesis
canonical height map is not treated as a fresh store. A compact load failure
refuses rather than falling back to full state.

The durable baseline stores a checksummed proof of the authenticated base.
Restart revalidates its headers and commitment, the current selected state,
profile binding and accumulator before admitting reads. Marker-above-tip repair
accepts only an exact baseline with matching state, sets, owners, retirement,
catalog, coins/accumulator and both markers. It never rewinds post-base state
or repairs from a plausible marker alone. Frozen legacy audits use the
authenticated retirement receipt, not omitted retired-pool bytes.

The base has no fabricated undo. Reorg/invalidation at or below it refuses,
including after historical validation finishes. Detached replacement-branch
replay can start from the sealed baseline, using catalog membership and exact
Utreexo proofs without a full coin set or pre-base bodies. Each ordinary writer
still rebinds its selected before-image and durable undo before committing.

`validatedTip` retains its existing state/forest-consistency meaning; it is not
a certificate that pre-base scripts executed. Imported base metadata omits
`CHAIN`, `SCRIPTS` and `HAVE_UNDO`. Full-node background validation independently
executes historical and Orchard bodies and compares Orchard state/sets,
retirement, catalog, forest and coin coverage to the baseline. Only then is
historical validation acknowledged. It never overwrites an advanced selected
tip or post-base coins. Compact nodes do not run archival replay or claim
pre-base execution. Snapshot authentication is not a wallet-history coverage
acknowledgement.

Legacy v3/v4/v5 files refuse Orchard-era bases even while DNORST02 is dormant.
Version 6 refuses before DNORST02 activation. Both profiles become eligible at
mainnet H150,000; a loader still requires the configured selected-chain burial.
Private-testnet cutover remains separately coordinated.

## Mainnet schedule and fingerprint review (2026-10-24)

The owner approved one release branch, with the standalone v5 restart repair
first, its regressions next, and logical consensus, snapshot/runtime and mobile
commits following. DNORST02 starts at the first Orchard block, H150,000; its
parent catalog is derived from independently validated history through H149,999.

The mainnet consensus fingerprint changes from
`9675a39fa147bab33c3e35da6df08fca598d90e54e6da7a08b0a1157f60c3111`
to `d81a7474229102455fb552224c63c8b0e13947f16cb59e86d9b5d02630057ad2`.
An independent explicit field-order transcription matched the production API;
external SHA256 confirmed both preimages. The complete byte difference is the
addition of these two newline-terminated lines after `orchard_branch_id`:

```
orchard_catalog_commitment_profile=DNORST02
orchard_catalog_commitment_activation_height=150000
```

Testnet remains `d999b0b59cc9f9575e85f424e763a15a53e50c8e24b50c5a96268cb11d93cbf8`;
regtest remains `0caa01581b5cf97410957a2f72a29ec5bc9c61b6a42c49db805ad298bd353ff9`.
The historical fully dormant mainnet vector is preserved by explicitly disabling
the catalog schedule along with the other release schedules in that fixture.
Tests pin these values and the real mainnet H149,999/H150,000 format boundary.

Known v5 limitation at bases H111,000..H149,999: pre-H60,000 coin height and
coinbase status are not authenticated at load by the legacy accumulator leaf or
the shielded-state coinbase proof. The checksum is publisher-recomputable; a
trusted manifest pins the publisher's file, not chain truth. Independent replay
checks the full transparent records digest and detects a metadata lie before
successful promotion. This is not a demonstrated promotion bypass. DNORST02's
catalog supplies this metadata authentication for v6; the experimental legacy
origin proof section is not part of the shipping v6 format.

Release go/no-go: November 2, 2026, noon America/New_York, earlier if measured
block production reduces deployment runway. H150,000, not an estimated date,
is authoritative. Remaining gates include production-diff review, the complete
applicable suite, mutation controls, crash/power-loss testing, real initial
catalog cost, coordinated three-host DNORST02 soak/migration/DC3/DC4 and both
snapshot profiles, released-asset provenance/hashes, and physical mobile devices.

## Recorded local validation

The native macOS arm64 development build completed `dinerod` and the real
service/replay test target with Orchard enabled. It uses system OpenSSL with
the existing unsupported-version development override; this is not a hermetic
release or cross-platform build receipt.

Passing CTest targets for this revision:

- `SnapshotFormatPolicy`: independent activation/dormancy and exact Orchard
  boundary checks, including v6 refusing while DNORST02 is dormant.
- `OrchardSnapshotSection`: full/compact canonical encoding, every truncation
  point, malformed framing, raw-set ordering/counts, complete catalog traversal,
  missing/corrupt/unreachable nodes, and genuine detached authentication with
  both accumulator profiles, full-coin coverage, hostile forest counts and
  atomic file publication. Forged sections are re-encoded by the fixture
  before verification so transport canonicality cannot mask an authentication
  omission. The block is a synthetic coinbase-only component fixture.
- `OrchardStateRoot`: existing independent Orchard vectors plus the explicit
  parent-catalog preimage, field binding, legacy-root separation and absence of
  a current-block-hash cycle.
- `OrchardSnapshotPolicyRuntime`: actual loader refusals before body parsing,
  exact persistent-row equality after database reopen, and preservation of an
  existing export destination.
- `ShieldedStateStartup` and `OrchardServicePersistence`: existing startup and
  retirement behavior with the retained restart-repair edits.
- `OrchardCanonicalCatalog`: existing canonical full/compact publication,
  disconnect/reconnect, startup, storage, history and corruption regressions
  against the shared catalog/undo producer.
- `OrchardSnapshotRuntime`: real PoW regtest activation, nonempty Orchard state,
  both storage profiles, private installer/reopen/corruption and exact marker
  repair, plus actual service initialization/start, export/import, stable full
  provider pointers, a real pre-base Orchard-note spend plus transparent spends,
  restart, floor refusal and replacement fork
  validation. Full historical replay completes while the selected tip is ahead;
  compact mode retains no full coin owner or forest.
  A separately later regtest gate preserves DNORST01 before selecting DNORST02.

The expanded local run also includes `OrchardCycle`, `OrchardCompactService`,
`OrchardCanonicalPool` and `OrchardParentCatalog` (12 focused CTest groups total).
After disk-backed processing, all 20 focused groups pass, additionally covering
`RuntimeReplaySpool`, `OrchardSnapshotMemorySmoke`,
`NodeCoreMobilePackagingContracts`, `UtreexoProofInputCapture`,
`WalletProofSnapshot`, `PoolGenesisSource`, `NodeCoreRuntimeLifecycle` and
`NodeCoreSnapshotConsumers`. New regressions check disk/RAM byte identity,
shared inventory lifetime, exact reachable union, private workspace cleanup,
MAC corruption even after structural caching, metadata height/coinbase/coverage,
direct canonical serialization and disk coin auditing against strict decoding
over every small canonical deletion subset.

The narrower `test_orchard_block_staging` executable also compiled its sources
but its existing link graph lacks relay/replay/header symbols on this checkout.
It was not reported as a passing test; canonical-writer regression validation
used the linked real `test_assumeutxo_replay` suite instead. Backend-disabled
syntax checks are separate from a complete backend-off build. No sanitizer,
abrupt-process/power-loss matrix or three-host v6 qualification is claimed.
Mobile SDK/capacity evidence below is narrower than production app qualification.
Coordinated network activation remains separate deployment work.

## Mobile SDK and memory qualification (2026-10-23)

`DINERO_ORCHARD_MOBILE_QUALIFICATION=ON` explicitly permits only iOS arm64
device/simulator, macOS arm64/x86_64 and Android arm64-v8a/x86_64 build targets.
It is OFF by default and is not release approval. Cargo receives the exact
target triple and target-scoped deployment/NDK compiler flags. The native-only
and Windows qualification guards remain intact. Orchard and Bulletproofs share
the pinned Rust compiler and unwind runtime, without folding a second runtime
into the Bulletproofs FFI object through LTO. This fixes the genuine Android
duplicate-runtime link failure; duplicate-symbol errors are not suppressed.
No chain parameters, public network schedules or activation CLI overrides were
changed by mobile qualification. The SDK smoke executable checks that all three
network defaults leave DNORST02 at `UINT32_MAX` in those historical receipts.
The current smoke instead requires mainnet H150,000 and dormant testnet/regtest;
the older SDK receipts must not be represented as qualification of this schedule.

The old `build_nodecore_xcframework.sh` allowlist is not a v6 packaging recipe.
Use `scripts/mobile/build_nodecore_slice.py` with a **new** build directory and
an OpenSSL prefix already built for the exact platform. For example:

```sh
python3 scripts/mobile/build_nodecore_slice.py \
  --platform ios-simulator --build build-v6-ios-simulator \
  --openssl /path/to/ios-simulator-openssl --memory-benchmark
python3 scripts/mobile/build_nodecore_slice.py \
  --platform android-arm64 --build build-v6-android-arm64 \
  --openssl /path/to/android-arm64-openssl \
  --ndk /path/to/ndk/29.0.14206865 --memory-benchmark
```

Apple packaging takes the archive closure from a successfully linked real
`nodecore_link_smoke`, not a directory glob or a stale dependency allowlist.
It requires the v6 codec/store and real Orchard Rust archive, merges exactly
those production inputs, then relinks using **only the merged archive** plus
the original system dependencies. SHA256 receipts cover every input and output.
`package_nodecore_xcframework.py` combines the four qualified Apple builds into
iOS device, iOS simulator and universal macOS slices. Android packaging via
`package_nodecore_android.py` checks both ELF architectures, 16 KiB page
alignment, retained C/JNI exports and build receipts; it stages stripped
`jniLibs` and separate debug symbols. These scripts refuse existing output
directories. Neither workflow replaces app libraries, changes consumer Git
pins, signs an app or publishes an artifact.

Capacity measurements use `tests/mobile/orchard_snapshot_memory.cpp` and
`run_orchard_snapshot_memory.py`. Enable `DINERO_ORCHARD_MEMORY_BENCHMARK=ON`.
Each phase runs in a fresh process and reports `getrusage` high-water RSS,
wire bytes, inventory counts and elapsed time. `--max-rss-mib` fails the run
when any phase exceeds its budget, while preserving all completed measurements.
The generated corpus has canonical catalog/framing/coin-forest bindings, but
synthetic frontier/body authority: it is **not** a valid chain transition or a
large authenticated service installation/restart test. Do not install it in a
node. `proof-roundtrip` exercises the streaming proof codec while retaining the
imported inventory; it is not RocksDB commit or crash-recovery qualification.

The six SDK builds and merged Apple archive relinks succeeded locally with
Xcode 27.0, Rust 1.91.1, OpenSSL 3.5.7 and Android NDK 29.0.14206865/API 28.
The final disk-backed Apple candidate is
`build-v6-sdk-apple-disk-audit/NodeCore.xcframework`; the Android candidate is
`build-v6-sdk-android-disk-audit/jniLibs` with separate symbols and
SHA256 receipts. The packaged simulator and macOS arm64 smoke executables ran;
Android arm64 loaded the actual JNI shared library and checked dormant network
defaults. macOS x86_64 and Android x86_64 are cross-link evidence only (no
Rosetta/Intel runtime or x86_64 Android device was used). A physical iOS device,
signed app archive, minimum-OS matrix and consumer app replacement remain
unqualified. System-dependency inspection of the static-OpenSSL macOS smoke
found no Homebrew runtime dependency.

The real legacy-v5 NodeCore snapshot consumer now passes. Pre-Orchard mobile
bootstrap retains a full authenticated forest even when its transport profile
is compact; accumulator RPCs report that actual full backing rather than
inventing a root-only owner. Exact assumed-base reads rebind the frozen coin
marker, expected root, selected header/work and PoW ancestry while leaving the
historical durable tip and consistency marker unchanged. Foreign canonical
rows, failed lifecycle state and all Orchard-era profile mismatches refuse.
Compiled/tripwire-checked genesis now publishes its consistency marker in the
existing atomic initialization batch. The real v6 service fixture separately
checks both selected-owner profiles, cold restart and rejection of a
post-Orchard configured-profile/actual-owner mismatch. The integration fixture
uses an ephemeral loopback RPC port, leaving unrelated daemons untouched.

```sh
python3 tests/mobile/run_orchard_snapshot_memory.py \
  --binary build-v6-android-arm64/rust/orchard_backend/orchard_snapshot_memory \
  --adb-serial emulator-5580 --counts 1000000 --max-rss-mib 512 \
  --report build-v6-android-arm64/v6-memory-disk-audit-million.json
```

The earlier 100k corpus contains 100k nullifiers, 100k anchors and 399,998 full
or 599,997 compact catalog records. Full carries 100k transparent coins; compact
carries three large membership trees but no coin table/forest. Sizes are 56.5
and 70.3 MiB respectively. These are baseline measurements before disk-backed
catalog processing, not the final results below. Rounded peak RSS in MiB:

| Executor | Full read / proof round-trip | Compact read / proof round-trip |
| --- | --- | --- |
| macOS arm64, before streaming | 215 / 344 | 231 / 464 |
| macOS arm64, streaming | 140 / 213 | 131 / 239 |
| Android arm64 API 35 emulator, streaming | 142 / 224 | 140 / 264 |
| iOS 27 arm64 simulator, streaming | 150 / 223 | 145 / 252 |

Both mobile emulators passed 10k and 100k codec phases with a 512 MiB budget.
Simulator RSS is not a physical iPhone jetsam allowance, and an Android shell
executable is not the DineroDPI app's combined native/Java heap. No physical
device low-memory, background eviction or signed application result is claimed.
The earlier RAM-catalog Mac million-entry stress case **failed** that budget:
the full file is 564.6 MiB and compact is 702.9 MiB, with proof-roundtrip peaks of
2,036 and 2,299 MiB respectively.

The final disk-backed million-entry runs pass **all 18 phases** under the same
512 MiB budget. Each fixture has one million nullifiers and anchors, with
3,999,998 full or 5,999,997 compact catalog nodes. Full also has one million
coins. File sizes and canonical bytes are unchanged. Rounded RSS in MiB:

| Executor | Full read / proof | Compact read / proof | Maximum of all six phases |
| --- | --- | --- | --- |
| macOS arm64 | 280 / 381 | 85 / 155 | 482 |
| Android arm64 API 35 emulator | 300 / 394 | 81 / 151 | 495 |
| iOS 27 arm64 simulator | 289 / 391 | 104 / 174 | 501 |

The largest peaks are full fixture generation, not compact import. These
results remove the measured RAM-catalog blocker, not every possible mobile
memory limit. Full-node live owners, wallet/Java/Swift heaps, genuine large-state
installation/restart, device jetsam and deployment remain separate qualification.
The process-local codec/proof fixture cannot certify these application limits.

Final reports (all six rows have `within_budget=true` in each):

- `build-snapshot-v6/v6-memory-disk-audit-million.json`
- `build-v6-android-arm64/v6-memory-disk-audit-million.json`
- `build-v6-ios-simulator/v6-memory-disk-audit-million.json`

Generated corpora and private spools are removed after verification. The
dedicated read-only Android emulator is shut down without changing the
consumer AVD's saved state, and the dedicated iOS simulator is deleted.
SDK candidates, manifests and measurement reports are retained. No consumer
app, public schedule or DNORST02 default was changed by those measurement runs.
