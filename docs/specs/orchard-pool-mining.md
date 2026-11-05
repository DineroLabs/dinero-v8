# Orchard pool mining contract

Orchard templates are finalized by the selected Core chainstate. A pool must
never reconstruct typed transaction or proof framing with a historical block
serializer. The legacy `getblocktemplate` call refuses at the boundary unless
its caller advertises `capabilities: ["orchard-immutable-jobs-v1"]`. A capable
caller receives the `profile: "orchard-v1"` marker, selected parent, height and
longpoll identity; it then obtains a job through `mining.getjob`.

`mining.getjob` continues to accept an `address`. On Orchard it also accepts:

- `base_job_id`: derive from a live job owned by this selected chainstate and
  parent, preserving its exact non-coinbase bodies and fees.
- `coinbase_outputs`: one through 1024 outputs, each with `script_pubkey_hex`
  and unsigned `value_una`. Their checked sum must equal the exact subsidy plus
  fees. Empty, oversized and OP_RETURN payout scripts are refused. Core adds
  all mandatory witness, filter and state commitments itself.
- `coinbase_extranonce`: a uint32 appended to the coinbase input script, retaining
  the original height prefix and the 100-byte script bound.
- `include_block: true`: return the sealed typed `block_hex` and a
  `coinbase_merkle_path` of raw-byte sibling hashes for coinbase index zero.

The response identifies `profile`, `coinbase_hex`, `coinbase_value_una`,
`transaction_count` and `total_fees_una`. A derived job must preserve the parent,
height, fees and every selected non-coinbase body, and fit the block weight limit.
The existing selected-chain guard, readiness gate and full submit validation
remain required.

The in-process job cache is bounded to 32 entries, uses least-recently-used
replacement and expires jobs after ten minutes. Repeated derivation keeps a
base live within its original lifetime. Pools with many channels retain the
exported frame and submit a solved copy through `submitblock`, so cache eviction
and backend failover do not require a foreign process-local job identifier.
Only the nonce and the advertised bounded timestamp may change. Version rolling,
transaction replacement and coinbase edits require a newly finalized job.

Shared SV2 miners retain their existing header protocol. Solo miners negotiate
immutable coinbase support and verify their chosen payout and coinbase inclusion
before hashing. Older solo clients are explicitly refused at Orchard activation;
there is no silent switch to a pool-owned payout. Historical mining remains
covered separately.

Qualification uses the test-only coupled Orchard schedule at H102 with real
historical PoW and normal block acceptance. It is not evidence that mainnet has
activated. Core regression cases include real shield bodies, exact contributor
payouts, invalid payout refusal, per-channel uniqueness, cache eviction, timestamp
rolling, stale parents and service ownership. SV2 qualification additionally
requires real shared and CPU/GPU solo sessions and an unchanged solo PPLNS journal.
