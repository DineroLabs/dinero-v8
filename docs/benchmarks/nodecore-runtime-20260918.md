# NodeCore runtime ownership qualification foundation

## What is exercised

`NodeCoreRuntimeLifecycle` and `NodeCoreSnapshotConsumers` link and call the
production `nodecore_ffi` library, including real DaemonApp services, ChainDB,
wallet recovery, snapshot import and Utreexo proof RPCs. They do not extract source
functions, substitute a daemon, or load an older packaged NodeCore framework.
The small C++ driver only transports requests and holds an actual shutdown event
callback. No production test hooks or new FFI symbols are added.

The fixture creates isolated regtest directories and disables P2P explicitly with
`p2p.offline=1` and `listen=0` in their config files. HTTP RPC and the wallet socket
are disabled. `max_peers=0` would not establish that isolation. All wallets and
snapshots are synthetic; the mnemonic is the public deterministic test phrase.

### Lifecycle

- Start, duplicate-start refusal, live chainstate RPC, synchronous stop and
  restart against the same datadir through the actual C ABI.
- Hold the real shutdown callback on the emitting worker. `is_running` is already
  false, but stop must wait for that worker. Release the callback, then require
  successful completion and unavailable stopped queries.
- Attempt another real start while that shutdown callback is held. It must wait
  for stop to finish and then acquire a new live context. This tests the existing
  start/stop mutex; it does **not** provide a lease for subsequent external work.
- Callback waits have deadlines; the Python controller and CTest impose outer
  deadlines. The callbacks themselves never synchronously call lifecycle/query
  APIs, which would deadlock with the current join/mutex arrangement.

### Snapshot consumers

The embedded source mines a funded base at 130, exports a v5 snapshot, then mines
eight more headers and exports a distinct generation at 138. Each generation has
a payload and a manifest binding its basename, byte size, height, block hash and
SHA256. Regtest does not qualify the fleet-signature policy used by DPI.

An empty embedded consumer rejects a changed payload against the original
manifest without changing its tip, Utreexo commitment or shielded root. A second
consumer imports the valid base via the real embedded path. After startup/import,
the fixture publishes the newer **separate** generation and imports the funded
mnemonic with rescan enabled. The actual wallet handler reopens the selected old
generation and must recover exactly the base wallet's outpoints. The test generates
and verifies a proof, rejects an altered sibling, restarts, verifies the proof
again, checks the original roots, and requires safe mode to remain off.

This proves the consumers work with retained immutable generations. It does not
prove safety if another writer replaces the selected file in place, implement
generation pinning/garbage collection, or qualify concurrent Swift publication.

## Observed lifetime correction

In the native runtime, successful `nodecore_start` returned at height zero with
`[snapshot] pending` for base 130. Initial bootstrap may be deferred until header
processing. The offline fixture explicitly calls the real `loadtxoutset` RPC to
drive that import; it does not assume running means imported or wait for a network
event that cannot occur offline. Later wallet import demonstrably consumes the
snapshot after startup has returned.

Therefore ownership must cover **selection, deferred initial import, later
recovery reads and required restart anchors**. Neither startup return nor initial
import completion is an artifact-release signal. The public FFI comments now
describe that, the unbounded cleanup portion of startup/stop, callback reentry,
and the fact that false running/ordinary stop do not authorize maintenance.

## Execution and negative controls

Configure `BUILD_NODECORE=ON`, build `nodecore_runtime_driver`, then run:

```sh
ctest --test-dir build-recovery --no-tests=error --output-on-failure \
  -R '^NodeCore(RuntimeLifecycle|SnapshotConsumers)$'
```

Linux's full Tests workflow now explicitly enables the library and runs each new
CTest by exact name with `--no-tests=error`. The native Linux library source list
includes the socket-server implementation needed to link a complete executable;
constructing a static archive alone did not establish that. Logs, replies and
checks are uploaded from `Testing/nodecore-runtime/`. The execution-map parser's
self-tests and selection of both names are checked locally; the full Linux
registration/baseline gate remains a CI responsibility.

The first Linux run reached the real executable link and failed: GNU ld had
already scanned the NodeCore archive when daemon/RPC libraries introduced
references to its legacy globals, HTTP/RPC infrastructure and vault bindings.
The Linux test executable now places those three archives in a rescan group.
This keeps the real implementations and changes no validation behavior. Linux
qualification must pass on that corrected head; the failed link is not a runtime
test result.

Local behavioral negative controls temporarily bypass actual stop, and separately
bypass `RescanWalletFromSnapshotUTXOs`. The first must fail the real worker-wait
assertion; the second must fail late wallet recovery. Mutants are restored before
the final build/test. A failed build is not counted as a behavioral failure.

## Remaining ownership gates

These tests are prerequisites, not a completed maintenance implementation. Still
required: token/intent protocol; reviewed reset allowlist; interrupted maintenance
and fresh-process startup barriers; path aliases; abnormal node-thread exit;
deterministic close/journal failure tests; real Swift expiration/publication
integration; rebuilt iOS/device memory and lifecycle qualification. Native macOS
and Linux locking do not qualify iOS's no-op RocksDB file lock.

No consensus, activation, checkpoint-retention or proof rule changes. No phone
reset, seed rollout, column-family relocation or deletion permission follows from
this qualification. Required import anchors and verified reconstruction remain
mandatory.
