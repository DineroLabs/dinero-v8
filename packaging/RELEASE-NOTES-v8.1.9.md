# Dinero v8.1.9

Dinero v8.1.9 is a network resilience, mining continuity, desktop usability,
and fail-closed hardening release. Every validating node, miner, desktop
wallet, server, and embedded NodeCore consumer should upgrade. The active
mainnet consensus rules, network magic, peer wire identity, wallet format, and
the height-73,035 and height-84,131 V4 AssumeUTXO trust anchors are unchanged.

## Mining continuity and chain convergence

- A lifecycle-safe recurring convergence driver closes transient gaps between
  the best accepted header and the fully validated active tip.
- Canonical bodies are requested immediately from the announcing capable peer,
  retried through bounded alternate-peer rotation, persisted durably, and
  activated in strict height order.
- Already stored canonical bodies are adopted without redundant downloads, and
  recovery resumes after restart from persisted header/body metadata.
- Mining continues only on the last fully validated active tip while recovery
  proceeds. It never mines on an unvalidated header, and all genuine safety
  gates remain hard stops.
- Active tip, best header, first missing body, request peer, retry count,
  progress age, and continuity-job state are exposed for diagnosis.

## Decentralized connectivity

- Dynamic AddrMan peers, feelers, peer gossip, persisted addresses, direct
  IPv4/IPv6, DNS bootstrap, explicit emergency peers, Dinero relay hints, and
  Tor v3 addresses can operate concurrently.
- CGNAT nodes gain encrypted Dinero relay fallback. Reachable nodes can enable
  a bounded Dinero-only relay service with conservative automatic limits.
- Dinero Qt can run a pinned, verified Tor component privately for Dinero P2P.
  Tor failure never disables ordinary P2P, and no wallet RPC credentials or
  onion private keys are exposed in the interface.
- Overview provides simple Tor and relay toggles and a read-only public onion
  address. Command-K remains a keyboard-only diagnostics panel.

## Mining applications

- Embedded CPU and GPU solo mining share the live serialized-header display,
  real candidate sampling, compact session-find history, and authoritative
  height/difficulty/hashrate status.
- Recoverable template errors expire from the live canvas after 15 seconds;
  fatal errors that stop mining remain visible and diagnostics remain logged.
- The bundled and standalone SV2 v0.2.8 CPU/GPU miners use the responsive
  authenticated terminal dashboard on macOS, Windows, and Linux. GPU binaries
  use Metal on macOS and OpenCL on Linux/Windows.

## Reliability and security

- Chain activation rejects disconnect candidates before mutating active state,
  closes the restart/reorg livelock found by stress testing, and preserves
  evidence from failing serial E2E runs.
- Unreadable active-chain bodies are explicitly re-requested outside unsafe
  lock ordering.
- Embedded-daemon wallet checkpoints survive clean restarts, preventing stale
  checkpoint startup failures and misleading wallet connection errors.
- Shielded fail-closed, spend-authority, anchor-history, restart, and cost-bound
  findings are addressed. All new shielded transfer activation remains dormant
  at `UINT32_MAX`; this release does not activate shielded value transfer.
- Integration harnesses honor the exact daemon path supplied by CMake and fail
  immediately with the real path error instead of reporting false RPC timeouts.

## Compatibility and operations

- No reindex or wallet migration is required.
- One active validated chain remains the only chain-selection model.
- Existing peers remain protocol-compatible, but the full fleet upgrade is
  recommended because daemon recovery, connectivity, mining, Qt, and embedded
  NodeCore behavior changed.
- The release includes refreshed NodeCore and DineroDPI artifacts for iOS
  arm64, Apple Silicon simulator, and universal macOS arm64/x86_64.

## Release artifact matrix

The public release must not be published until all of the following are
present and verified:

- Linux x86_64 server tarball and Debian package.
- Linux x86_64 Qt AppImage, portable Qt tarball, and desktop Debian package.
- macOS Apple Silicon Qt ZIP, DMG, and operator tarball.
- macOS Intel Qt ZIP, DMG, and operator tarball, including the Ventura lane.
- Windows x86_64 portable package, desktop installer, and server installer.
- Standalone SV2 v0.2.8 CPU and GPU miners for every supported platform.
- Primary and fallback AssumeUTXO artifacts, manifests, publisher signature,
  checksums, and build provenance.
- NodeCore XCFramework and DineroDPI iOS/macOS build outputs.

macOS applications, embedded executables, standalone miners, and disk images
are Developer ID signed, submitted to Apple notarization, stapled where the
artifact format supports stapling, and verified. Windows executables and
installers are Authenticode signed and timestamped. Checksums are generated
only after signing and notarization.

## Source provenance

Dinero platform artifacts are built from the immutable signed `v8.1.9` tag.
SV2 miners are built from pinned commit
`8f9d7f688080483fbeb2f62b234031d5d5567ec5` and tag `miner-v0.2.8`.
Platform checksum manifests and provenance records are attached to the release.
