# Same-block originated history qualification

The actual service recovery fixture now includes a second signed transaction in
the canonical first event. It spends the known-owned change created by the first
transaction in that same event. The origin projection cannot contain that input:
its ownership must come from ordered first-event effects.

With existing pre-origin and first-transaction metadata but no child metadata,
explicit adoption must refuse before either receipt commits. Supplying the child
record through the existing wallet API permits adoption. The fixture checks the
intermediate coin's exact child spender and height in the ordinary store and the
index, idempotent adoption, and the final unspent child output.

An injected child-spend SQL failure occurs after earlier first-event effects;
ordinary baseline, effects, history heights and receipt roll back together while
the independently committed index prefix remains available for reopen/retry.
During actual canonical undo, an injected child-history unconfirmation failure
occurs after the parent history update. Both confirmations and all ordinary coins
must roll back. Reopen/retry removes both block-created outputs and preserves both
send records unconfirmed. Canonical reconnect confirms both records again with
unchanged local categories, amounts, labels, addresses and timestamps.

Production code and receipt formats are unchanged. This extends the known-script
qualification of the existing explicit service adopter and bound consumers. It
does not provide missing metadata, key/account discovery, authenticated global
inventory, durable pending bodies, mempool admission, reservation recovery or a
production provider. The fixture uses public synthetic signing keys, explicit
service/layout setup, regtest proof-of-work policy, pre-boundary state commitment
enforcement disabled, and required actual boundary commitments. It is not full
startup, first-activation bootstrap, CT epoch, mainnet, Orchard private activity,
fresh-process crash, physical-power-loss, load or release qualification.

## Validation

Fresh declared service/index/replay targets and the full daemon build passed.
Three CTests passed: AssumeUtxoReplay (4.05s, eleven internal cases),
OrchardServiceDeliverySource (12.30s), and OrchardIndexDelivery (78.63s).
Both unchanged root selectors enumerate 46 enabled tests; only these three ran
locally. CI requires a unique same-block execution marker in the retained log.

All 203 linked project C++ translation units were freshly ASan/UBSan instrumented,
including the actual service and consumers. Copied controls that omit ownership
of first-event creations or delete block creations before inspecting history
failed the intended assertions. The second control bypasses child unconfirmation
and fails the required SQL-error refusal. The restored fixture passed. Normal and
control link maps contain no project C++ archive members. Rust/external libraries
are uninstrumented, macOS leak detection is disabled, and the ARM dependency gate
remains open. No original-source test failure is claimed: production already
handled these cases. The changed test C++ source is absent from the genuine OFF
graph previously qualified; there was no new OFF build. Inherited build labels
and prebuilt OpenSSL do not establish release provenance.
