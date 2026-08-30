# Shielded spend-authority activation profile

**Status:** implemented, dormant in v8.1.9; activation height is
`UINT32_MAX` on all shipped networks.

Activation is a coordinated consensus cutover, not a wallet feature toggle.
At the reset height the block MUST contain no shielded activity and the tree,
anchor window, and nullifier set are reset. Beginning with the following block,
new outputs commit to `pk_d_spend = s*G`, spends use proof version `0x05`, and
legacy proofs and pre-reset notes are rejected.

## Distinct reset implementation

The deployed `shielded_epoch_reset_height` remains the historical CV-binding
cutover at height 61,000. `shielded_spend_auth_epoch_reset_height` is now a
separate dormant parameter, required to equal spend-authority activation and
required to differ from the historical reset. Both reset boundaries feed the
same mempool wall, connect/disconnect snapshot, reindex, stateless undo,
ChainDB purge, and restart stale-state protections. Consensus proof validation
also consumes the spend-authority activation height and requires proof version
`0x05` at the boundary.

No production height has been selected. Mainnet RPC and Qt lockouts remain in
place. The mechanism being present does not authorize activation.

The cutover MUST NOT be scheduled until:

1. an independent cryptographic review is complete;
2. the activation preflight proves the live pool is empty or an explicit
   migration/stranding decision has been approved;
3. restart-plus-reorg anchor restoration passes from persisted v2 state;
4. archival, stateless, snapshot, and reindex nodes produce identical state;
5. sender/recipient tests prove that the recipient can spend and the sender
   cannot;
6. all mobile/prover-kit callers support 75-byte address payloads.

RPC and Qt lockouts remain enabled until the active chain is past the cutover
and runtime state markers agree. Removing either lock independently is a
release-blocking defect.
