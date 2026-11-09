# Mempool behavior references

`transcripts/off.json` and `transcripts/on.json` record 30 regtest steps from
the pre-extraction `dinero-main` commit `3d84b96fdeb6406e540a4649cd4ed98d475231d3`.
Both daemons were built from that exact commit with their matching Orchard
build options. The script compares RPC admission, replacement, ancestor and
descendant views, `mempool.dat`, restart, and reorg readmission. It fails if
setup or persistence fails. `record` is for deliberate reference updates;
CI uses `check`.
Run-specific signatures and addresses make the cross-run JSON reference
normalized. Before normalization, the script compares the persisted raw
transaction byte-for-byte with the signed replacement submitted in that run.

The Orchard-on transcript uses ordinary regtest transactions because the
normal regtest build does not activate Orchard. The separate
`MempoolScanSinkOrchardAdmission` test admits a canonical Orchard body through
the real pool, saves it, clears the pool, reloads it, and checks byte-exact
retention. Its selected-chain validator is a test double, so this is a
mempool persistence check, not Orchard proof validation.

Windows has no exact-base MP4 reference yet. The non-required Windows job
builds the daemon and runs the focused contracts, while it reports MP4 as
unmeasured. NodeCore's embedded runtime is a POSIX target; the Linux and macOS
jobs link and run its smoke executable. The POSIX jobs compare against these
reviewed references.
