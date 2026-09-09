# Private payments and covenant UX

Status: local implementation on codex/qt-private-contract-flows, based on
1589bf641. No production activation or consensus changes.

## Supported distinctions

- Public send: transparent wallet payment.
- Private send/conversion: Send offers a visible entry to the existing Shielded
  composer. That composer retains its operation journal and production lockout.
  This does not enable production shielded spending.
- Public covenant: create/fund using wallet.covenant.ctvfund in Send; inspect and
  spend confirmed descriptor-backed outputs in Covenants. The Covenants page
  links to creation and refreshes when selected.
- Shielded covenant: unsupported. A normal shielded note does not encode and
  enforce the current transparent CTV covenant in its authorization proof.
  Unshielding and funding a public covenant does not make that covenant private.

Creation remains in the shared Send form for this patch. Covenants is the
management page. A separate unified covenant composer and descriptor import UI
are not implemented by this patch. Receiving a contract is distinct from receiving
an ordinary payment: recovery metadata and a matching confirmed output matter.

## Correctness fixes

Batch rows with partial or invalid data fail the entire request instead of being
silently omitted. Values and sums use integer una with overflow protection.
The unrelated top-level recipient/amount are disabled for batch mode and ignored
when building its transaction. Review displays the actual funding value, exact
committed outputs and reserved withdrawal fee. Cancel is the default action.

Hour/day estimates use the current 120-second target on all networks, enforce the
65,535-block relative-lock bound, and explain that time starts at confirmation.
The contract-tab refresh now matches the actual Covenants tab name.
Unsupported send modes cannot fall through to public send.

## Verification

- Standalone Qt 6.9.1 Release app compiled and packaged; ad-hoc signature verified.
- All 22 CTest targets passed, including the new CovenantFormPolicy test covering
  smallest-unit sums, malformed amounts, overflow, and relative-lock boundaries.
- Existing shielded intent/restart and production-lockout tests passed.
- Existing contract production RPC wiring guard passed.
- git diff --check passed.

A new daemon-backed GUI create/fund/confirm/restart/spend lifecycle has NOT been
run for this patch. No production funds were moved. The changes have not been
installed into the user's running wallet.

## Private covenant work still required

Specify whether contract policy itself is public or hidden. Then define a
versioned note commitment to policy, bind policy satisfaction and allowed output
transitions into the spend proof, define fees/nullifiers/change and recovery
semantics, implement wallet discovery and descriptors, and add adversarial tests
for bypasses and restart/reorg recovery. This requires consensus and circuit
review and an activation plan, not a Qt visibility toggle. Ordinary private
payments and private covenants must remain separate advertised capabilities.
