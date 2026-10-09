# Catalog membership for nontransparent outputs

Compact Orchard input validation must not accept a peer's claim that an output
is transparent. Both historical and maturity-bound Utreexo leaf preimages omit
the confidential flag and commitment. The existing mixed-block validator refuses
ordinary confidential inputs and outputs; a proof-backed adapter must preserve
that refusal even when a peer clears those fields.

The completed independent parent replay now prepares a third content-addressed
set. It includes every unspent output whose confidential flag is set **or** whose
commitment is nonempty, at every creation height. Each member binds the exact
outpoint with a separate key domain. Preparation rereads membership against every
coin in the completed replay before returning. A missing referenced node is an
error, not proof of absence.

The canonical catalog record uses `DNOCS02` and binds this set's root and count.
`DNOCS01` is refused because it cannot establish absence of this metadata. There
is no conversion that defaults the new set to empty. Rebuilding through the
existing independently validated history owner is required for old local records.
No production data migration is performed here.

Initial enrollment copies the prepared set. Canonical updates check its membership
against each authenticated before-image and stage any removal or insertion in the
same existing batch. Retained parent records preserve the previous root for undo.
The shared transaction validator still refuses ordinary confidential operations;
this catalog extension does not enable them.

Normal qualification passed fresh ON/OFF daemon
and replay builds: 16 selected CTests containing 50 ON and 15 OFF cases. These
cover typed membership, exact outpoint and domain binding, missing nodes,
retained roots, and refusal of incomplete state records, plus existing replay
and canonical transition checks.

A nonempty fixture passed through transaction and block wire
round-trips, actual historical validation, completed parent replay, catalog
preparation, and database reopen. It retains two zero-value coinbase outputs
with opaque nontransparent metadata, on opposite sides of the maturity-leaf
boundary. The fixture does not inject coins into the validated set. Its opaque
fields do not constitute cryptographically valid confidential payment or range
proof material, and it makes no confidential spending claim. The fixture is now
included in the qualified normal replay target.

All 50 selected ON cases also passed with all 322 linked project C++ translation
units and bundled Bech32 freshly instrumented with ASan/UBSan. Other external
libraries, Rust, C and PQClean were not instrumented; macOS leak detection was
disabled. This qualification does not cover the full daemon or OFF binary under
sanitizers.

Three copied-header validation controls each freshly rebuilt the same 323 C++
translation units. Omitting all nontransparent classification, omitting it at
modern creation heights, and omitting root/count consistency each failed its
intended assertion without a fixture exception or sanitizer diagnosis. Restoring
the original code passed all 13 selected canonical and parent catalog cases.
These controls changed data validation only. Exact source, commands, linked
objects, executed cases and results were independently verified and preserved
in the private qualification ledger before this documentation update.

The nonempty fixture exercises the actual parent catalog producer. Existing
canonical enrollment and transition fixtures still use empty nontransparent
sets. A production compact adapter, its selected-parent ownership,
restart/reindex integration, and stripped-metadata end-to-end refusal still
require implementation and qualification. No release readiness is implied.
