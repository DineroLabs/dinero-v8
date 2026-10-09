# Compact Orchard pool input view

The enrolled compact chainstate owner now captures a transaction-scoped immutable
coin view under its bound writer mutex. It checks the durable tip, validated tip,
header, chainwork, exact catalog record, storage profile, and selected Orchard and
retirement identities before and after capture. Empty owners refuse.
The retirement marker must match the selected height, block and predecessor as
well as the enrolled record. At the activation parent, both Orchard state and
the retirement marker must be absent.

Every supplied transaction must have a new transaction ID in the authenticated
catalog. This proves only its explicitly enumerated output outpoints absent.
Every external input needs an exact-body Utreexo payload, matching stump commitment
and leaf count, unique input, value bounds, and maturity. Legacy creation height
and coinbase metadata come from the independent legacy catalog. Modern metadata
becomes authoritative only after its exact leaf proof succeeds. Nontransparent
membership is checked independently of the peer's claims. Missing or corrupt
catalog nodes refuse; no partial view escapes. Unknown lookup returns an error.

Raw peer metadata remains private; the older VerifiedUtreexoTransaction token is
unchanged. This view cannot be created from a caller-decoded catalog alone. It
uses no full coin database or full forest. Same-package input proofs are not
represented by the current wire format and remain refused. An empty proof is
allowed only for a body with no transparent inputs; this does not validate its
Orchard authorization, anchors, nullifiers, pool capacity or signatures.

The production service still needs its compact startup owner and canonical
admission/selection integration, with full journal/readiness/consumer checks.
This component does not remove any full-storage guard, install a production
provider, authorize live pool publication or complete compact-node readiness.
Reopened real compact-store tests exercise legacy and modern signed spends,
ordered coin checks, absence/error outcomes, malformed proofs and metadata,
selection mismatch and missing/corrupt catalog nodes. Qualification is pending;
no network, whole-node, crash, platform or release claim is made here.
