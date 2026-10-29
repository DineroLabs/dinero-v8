# Daemon stored-body routing

The optional Orchard build now gives `ChainstateService` a typed stored-body
query. `getRuntimeBlockByHash` acquires the activation lock and resolves the
indexed height. `getBlock(height)` holds that same lock across selected height
lookup and reading, and uses the typed query path at Orchard-active heights.
Before activation, `getBlock` retains its historical reader and serialization.

`ReadRuntimeBlockUnderLock` chooses the format from fixed selected network
parameters and the index height, never from an envelope's claimed version. It
rejects an invalid local profile and inconsistent height metadata. Historical
reads use the existing strict archival reader. Post-boundary reads require an
indexed flatfile, then check its framing/checksum, stored header/key/parent,
exact mixed decoding, size/weight and transaction and witness commitments.
Witness enforcement comes from the selected network height. An embedded copy
cannot hide a missing or corrupt strict flatfile source.

The returned `RuntimeBlockBody` keeps historical and mixed bodies as distinct
immutable alternatives. A mixed body cannot be obtained as `Block`; there is
no fabricated historical transaction shell. Its complete encoded bytes are
retained for the daemon's hex query. The existing historical `getBlockByHash`
API remains historical. Builds without the optional backend return `Internal`
from the new typed API rather than claiming Orchard support.

## Scope

These are read-only stored-body queries. They do not prove PoW, authenticate
ancestry or historical validity, authorize a transaction, change quarantine or
validity flags, commit chainstate, or enable connection. Stored header metadata
is a locator, not a consensus certificate. Params must remain fixed and the
caller must keep the selected chain lock held when using the lower-level API.

ConnectTip/DisconnectTip, admission, relay and mining still need their typed
integration. The daemon must not activate Orchard merely because this reader
can return a body. Mainnet remains inactive and no production branch ID is
selected. The installed app is not replaced by this work.

## Qualification

`OrchardRuntimeReader` calls the actual ChainstateService implementation with a
generated temporary ChainDB and real BlockStorage. It covers historical and
mixed query results, type separation, exact bytes, selected activation/domain,
height and parent metadata disagreement, read-only reopen, missing strict
flatfiles, and transaction/witness commitment failures. It does not start a
node or run consensus. Existing test-only mempool/vault link stubs isolate
unrelated service references; these paths are not exercised by this test.

The root Orchard workflow explicitly builds, inventories and runs this test;
its expected inventory grows from 32 to 33. This is separate from the native
backend-only job, which does not link the daemon service.

Local qualification: the daemon and all selected executable targets rebuild;
48 selected CTests pass (33 mandatory Orchard registrations, 15 compatibility
lanes, including 21 internal Rust tests). The affected service translation unit
also compiles with the optional reader disabled. The focused real-service regression passes. ASan/UBSan passed with 196 linked project C++ translation units
instrumented. Copied-source controls that omit body-identity checking or the
service's mixed hex-query routing both fail the regression. Rust and external
libraries are uninstrumented and macOS leak detection is disabled. These are
working-source checks, not released-binary provenance or whole-node lifecycle
qualification. Exact-head Linux qualification is required after commit.
