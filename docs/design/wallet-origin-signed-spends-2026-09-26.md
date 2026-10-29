# Signed-spend origin recovery and retained history

The service recovery fixture now includes a genesis-bound regtest ancestry through
height 102. An actual Schnorr-signed transaction spends the height-1 coinbase at
height 101, after the ordinary validator's 100-block maturity requirement. The
actual indexed Orchard boundary at height 103 spends that transaction's change.
Both the selected-history replay and boundary preparation verify their signatures.
A changed historical signature preserves the header hash and transaction ID but
causes origin capture to refuse, demonstrating consensus replay beyond identity
checks. The previous immutable projection remains unchanged.

Known-script origin adoption refuses missing originated history. Once existing
send metadata is supplied through the real wallet recording API, adoption retains
its address, category, label, amount and timestamp while deriving confirmations
from selected history. An unrelated unconfirmed history row also survives. These
rows are fixture setup; they do not establish durable pending bodies, reservations,
broadcast ownership or mempool admission.

## Undo correction

The new signed case exposed deletion of the recorded send history on ordinary
undo. Checked ordinary delivery now examines actual source inputs and their
recorded owned spender identity/height before removing block-created coins. It
unconfirms existing history for those owned spends, retaining all other fields.
It then removes the other block history and applies coin undo. Reading first also
covers an owned output created and spent in the same block. This behavior does
not infer a send category, fee, or local intent and does not recreate missing
history. Incoming/mining history continues to follow the existing disconnect
policy. Live lock/abandonment sets are untouched.

The checked reads, unconfirmation, coin effects and existing DNOW01 receipt share
the same FULL transaction. SQL failure cannot commit an unconfirmed history row
without its coin undo. The independent index transaction may already have
committed and is retried through its existing receipt after wallet reopen.
Reconnect confirms the retained metadata again. There is no new receipt, journal,
identity, cursor reset or provider installation.

## Qualification scope

Fresh declared service/index/replay targets and the full daemon build passed.
Three CTests passed: AssumeUtxoReplay (4.07s, eleven internal cases),
OrchardServiceDeliverySource (9.89s), and OrchardIndexDelivery (78.52s). Both
unchanged workflow selectors enumerate 46 enabled root registrations; only these
three suites ran locally. CI now requires the actual signed-spend marker.

All 203 linked project C++ translation units were freshly ASan/UBSan instrumented,
including the actual service capture/adoption and ordinary consumer. The service
fixture passed, maps exclude project C++ archive members, and three copied
controls (original consumer, ignored unconfirmation SQL error, erased label)
failed their intended assertions without sanitizer diagnostics. The restored
fixture passed. Rust/external dependencies are uninstrumented, macOS leak
detection is disabled, and the full RocksDB ARM gate is open. The changed C++
files are absent from the genuine backend-OFF graph qualified at b1b3; this is
an exclusion check, not a new OFF build. Inherited labels/prebuilt OpenSSL are
not release provenance. There is no new fresh-process or physical-power-loss
qualification.

The initial signed test failed against the preceding ordinary consumer after a
real canonical undo removed the history row. An expanded SQL-failure test also
failed before the production fix. The first fixture compile used the consensus
OutPoint type to index a TxOutPoint map; the fixture was corrected without
changing any production check. No deadline or existing assertion was relaxed.

The signing keys are public synthetic fixture keys for known watched scripts,
not a wallet signing RPC or complete key-discovery qualification. Subsequent
undo/reconnect consumes the actual checked canonical outbox through the bound
consumers; no startup/notification provider is installed. Same-block coin effects
remain covered by existing tests, but this new send-history case spends change
from a previous block. Missing first-event originated history and durable
pending/mempool ownership still need a reconciliation policy.

The ancestry uses the existing regtest header/PoW policy, with pre-boundary state
commitment enforcement inactive and actual boundary commitments required. It is
not mainnet historical certification, full startup, first-activation bootstrap,
CT epoch/value qualification, general long-history/load qualification, or a
running recovery provider. Steps 1–4 and the release gates remain incomplete.
