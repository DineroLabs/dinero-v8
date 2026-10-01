# Vault credit allocation and canonical withdrawals

Locally qualified at the combined final source for the component scope described below. Fresh full declared targets built ON/OFF;75 ON and70 OFF selected CTests passed, plus the enabled OrchardRuntimeReader CTest. All linked project C++ freshly ASan/UBSan:162 replay,122 wallet and35 state-machine cases, plus a separate complete DaemonApp graph with five ordinary process modes. Exact source/map/input and executed-case evidence is retained privately. Nine normal link-regression binaries and enabled reader are outside sanitizer graphs; external/Rust/C/PQClean uninstrumented and macLSanoff. No whole-node, transport, crash-durability, complete send RPC or release claim; mainnet unset.

## Financial owner

Each newly authorized durable withdrawal records its exact credit-origin
sequences and principal amounts in the existing vault ledger before dispatch.
Allocation state is derived by replaying that ledger, not stored in a separate
journal. A new request selects available confirmed origins first, then pending
origins, in retained sequence order. Retries keep the recorded selection.

First enrollment of an account requires its complete present retained credit
prefix to reconcile with its current deposit stages, account balances, advance
counters and loss. Historical aggregate withdrawals or balance adjustments
that do not identify their source principal refuse enrollment. Their existing
records remain intact; a new allocation cannot reconstruct their attribution.

An origin retains nominal, remaining and reserved principal. Inclusion debits
the recorded sources. Undo restores those same sources and reservations.
Maturity moves only remaining principal. Orphaning one origin cannot consume
later deposits, and realized operator loss is only that absent origin's spent
principal. Reinstatement restores its retained amounts after required maturity.
The model checks nominal = remaining + currently included debits.

Dispatch begins durably before the external wallet callback. The authenticated
wallet owner binds the full signed body, recipient output, actual fee and
explicit request terms. Ambiguous outcomes retain the same body and inputs.
No timeout or retry creates a replacement payment or releases those inputs.

## Selected chain observations

The production wallet withdrawal dispatcher captures the actual chain service
lifetime and selected-chain lock before reading wallet state. It authenticates
the retained request and body, then checks the canonical block, header, Merkle
root, transaction ordinal, body digest and exact transparent recipient output.
The service keeps this source lease through its checked wallet transaction and
live publication. Deposit and payment captures must identify the same tip.

An optional transaction-index miss preserves an existing inclusion whose exact
height/hash remains canonical. Only proof that that anchor left the selected
chain authorizes undo. Initial confirmation follows the configured withdrawal
depth; confirmation remains reversible and is not finality.

A reorg can restore reservations after newer requests filled admission limits.
Replay permits that restoration only for recorded inclusion followed by undo.
All restored requests count toward live admission, so new excess requests still
refuse. Historical format admission checks remain unchanged.

## Persistence and compatibility

DNVS06 appends nine entry tags and one confirmed retained-payment state. Existing
DNVS01–05 tags and readers remain. Replay reconciles every present allocated
origin with its saved deposit, and every reservation with its saved request,
payment and inclusion. Wallet authentication covers both retained and confirmed
bodies. An attributed successor must preserve the exact previous ledger prefix,
request payloads and deposit identities.

The existing explicit fee terms are retained. This change does not select an
operator-liquidity or recipient-funded fee policy. Operator selection remains a
separate unresolved decision.

## Qualification required

Fifteen new cases are written and registered, but have not run: seven ledger,
snapshot and codec cases; two actual wallet transaction cases; and six actual
canonical-chain withdrawal cases. The canonical fixture uses encrypted wallet
keys, real admission/mining/undo, authenticated wallet retention and the
production dispatcher. It explicitly enrolls the validated funding output
through the existing wallet API; this does not qualify production notification
installation. Canonical cases require the Orchard backend. Ordinary allocation
cases are required in both configurations.

Fresh full declared targets, ON/OFF tests, and all linked project C++ sanitizer
graphs are required after the final edit. No old build qualifies these changes.
No unsafe-original, race, churn or synchronization-removal controls are used.

This is present-record attribution, not proof against whole-catalog deletion,
backup rollback, arbitrary callers forging a selected-chain observation, or
multi-process ownership. Production notification composition, broad load and
whole-node release gates remain separate. Mainnet activation remains unset.

The prepared runtime-worker case opens the existing authenticated owner, observes its actual canonical retained payment once, and requires an unchanged-tip pass to preserve state, revision and signed body. It uses ordinary Start/pass/Stop and remains uncompiled/unexecuted.
