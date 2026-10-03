# Fresh wallet restoration uses the supplied initial identity

Historical qualification for commit 8600cb4. The [new-name restore contract](wallet-restore-new-name-2026-09-27.md) now supersedes the existing-target replacement behavior and regression described below.

`RpcRestoreWallet` now routes a previously absent target through `createFromBip39`. The existing creation owner rejects registered names and existing database files and persists the supplied seed before registry publication. A validated recovery phrase is bound by that same creation path. Restoration no longer initializes an unrelated random seed and then replaces it, nor clears fresh encryption settings before optional `encryptWallet`.

The creation API accepts an explicit checksum-bypass argument, defaulting to false. Bypass derives the supplied recovery seed without recording an authoritative mnemonic. The RPC keeps its existing checksum warning and preflight expected-address check. Its existing twenty receive and twenty change derivations remain.

## Scope and open gates

This is the initial database path for restoration into an absent target. It is not evidence that the phrase has no historical wallet or P2MR master. It does not authorize PQ master generation, provide an initialization-versus-recovery certificate, or reconstruct imported keys, nonzero accounts, historical scripts, or archives from a mnemonic. No new owner marker or journal is written.

Existing-wallet replacement remains an open transition. Its old encryption-reset behavior and existing regression are retained, including the known restore/re-encrypt policy failure. Clearing policy settings alone would not establish ownership of existing encrypted imports or a random PQ master. Staged unlock, preservation of those owners, and the outer restore transaction still require implementation.

The initial seed, mnemonic binding, registry publication, later address issuance and optional encryption are separate operations. This change does not establish whole-restore atomicity, crash/power-loss recovery, complete discovery, chain readiness, pending ownership, production notification installation, or release readiness. Mainnet activation remains unset.

## Required qualification

Three new actual RPC-component cases cover one initial seed write with mnemonic binding and existing-name refusal, encrypted restore/reopen, and explicit checksum bypass with encrypted reopen and no authoritative mnemonic. The initial versions of all three fail against the predecessor before the production edit. Later existing-name/stored-seed assertions have no initial RED claim.

The independent Orchard workflow requires all three enabled CTest registrations and all three exact execution markers and retains their inventory/log. Existing root selectors and existing test assertions/deadlines remain unchanged. Actual fresh ON/OFF builds, selected regression execution and linked project-C++ sanitizer scope are recorded in private qualification evidence. Future Linux results must be verified on the exact commit; registration or predecessor success is not execution qualification.

Local results: fresh genuine ON/OFF full daemons and both declared wallet targets build; thirteen selected CTests pass in each configuration. All sixty-nine linked project C++ files are freshly instrumented with ASan/UBSan, including the actual RPC handler and wallet manager; seven recovery cases pass with 1,287 unchanged source/header input hashes. Two copied controls fail intended assertions and the restored three cases pass. External/Rust/C/PQClean dependencies, daemon and OFF binaries are outside that sanitizer scope; macOS leak detection is off and the full ARM RocksDB gate remains open. The existing replacement regression still fails in both configurations.
