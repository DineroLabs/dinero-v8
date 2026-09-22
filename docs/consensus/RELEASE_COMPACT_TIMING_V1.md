# Joint compact / 60-second release profile

Status: implementation candidate. Public activation heights remain `UINT32_MAX`.
This change implements the joint profile and the P2P portion of UPG-1. It does
not complete RPC/template/SV2 compatibility enforcement or qualify deployment.

## Profile and boundary

`compact-v1-60s-v1` couples `release_v8113_activation_height`,
`shielded_compact_activation_height`, and `sixty_second_activation_height`.
Public chain selection rejects a partial or mismatched schedule. The scheduled
height and profile enter the consensus checksum; dormant fingerprints stay
unchanged. `ConfigureReleaseV8113` checks a candidate before replacing parameters.
Existing regtest experiments may retain independent compact/timing heights and
have no release service cutoff unless the joint profile is explicitly selected.

The isolated `--consensus-release-height=H` switch requires a compact-test build
and regtest, refuses zero/sentinel/malformed heights and conflicts with either
individual compact/timing switch. It runs before the PoW profile fingerprint is
bound to the disposable datadir. Production heights can only be set in source.

Service cutoff starts when the published active tip is `H-1`: the next candidate
block is subject to the new rules. A reorg below `H-1` restores legacy service
eligibility; this is not a permanent ban. P2P policy reads the existing synchronized
published-tip value, not a peer's advertised height, header height, or a bare
`active_tip_` pointer. With a scheduled profile and no published tip it requires
capable peers rather than assuming a pre-activation height.

Consensus ASERT, rewards and proof checks remain the existing height-aware
implementations. Initial reward remains 100 DIN, tail becomes 0.5 DIN, maturity
remains 100 blocks. This service policy does not alter historical validity or
fingerprint the wallet that originally constructed a valid transaction.

## P2P capability and enforcement

`NODE_COMPACT_TIMING_V1 = 1ULL << 29` declares production v6/DZE1 proof and
60-second ASERT/reward support. Protocol 70016 and user-agent text alone do not
establish this capability. The bit is a peer's claim, not binary attestation;
normal consensus validation remains mandatory for every transaction and block.

Both handshake directions inspect it and recheck policy after the handshake's
waits. Existing peers are checked before message dispatch and direct sends;
the asynchronous outbox rechecks at dequeue, including retries. The existing
30-second keepalive sweep closes idle unsupported sessions, using an atomically
published handshake-complete flag so it does not mistake a partially read
upgraded handshake for an unsupported peer. An in-progress dispatch that began
before the boundary cannot be recalled; consensus still validates its result.
The shared peer paths also cover relay-virtual peers; cross-transport canary
qualification is still required. Rejections log `upgrade-required`; handshake refusal also sends the standard
`reject` message with `REJECT_OBSOLETE` and that reason.

## Outstanding UPG-1 work and release gates

- RPC transaction submission, template/mining negotiation, issued-job policy,
  updated maintained clients and the separate SV2 pool.
- Machine-readable RPC upgrade-required errors and maintained-client behavior;
  local history/read access must survive the cutoff.
- Real v8.1.12 versus candidate binaries across activation, reorg, restart and
  reconnect, including relay transport and preserved reward/pool accounting.
- Final combined migration/proof/timing qualification, independent consensus
  review, consistent seed copies, resource measurements and Dell–Mac rehearsal.
- Final NodeCore ownership/recovery and platform/device qualification.

No capability declaration or column-family separation authorizes deleting data.
