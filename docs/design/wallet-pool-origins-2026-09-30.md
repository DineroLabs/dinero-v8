# Retained pool allocation references

Locally qualified with fresh full dinerod and declared wallet component ON/OFF, fourteen actual selected CTests each, and all 58 selected ON cases across nine groups with the complete linked project-C++ graph freshly ASan/UBSan instrumented. Four new allocation-reference owner/bound-handler cases execute in both configurations; existing request/vault cases and deadlines remain unchanged. Exact linked counts are in the private receipt. External/Rust/C/PQClean uninstrumented; macOS LSan off; daemon/OFF outside this graph. Exact Linux and release qualification remain separate. This creates no pool origins, authenticates no pool allocation and installs no production payout processor.

## Behavior

The existing authenticated wallet pending-payment owner can retain up to 256 sorted, unique, nonzero 32-byte allocation references on a PoolPayout request. Request equality includes the exact reference list. Fully authenticated prior records are checked for overlap before request preview/signing or retaining another payment. Changing the request ID, owner, or grouping cannot reuse an allocation reference already present in that wallet's retained records. An exact request retry returns the same retained signed body. It does not submit another transaction or infer acceptance from a previous unknown outcome.

DNPP04 adds these typed references to the existing authenticated envelope, using its existing persistent-wallet identity and seed-derived AEAD. A canonical decode/re-encode checks every record and refuses duplicate references across records. Reads do not migrate or rewrite DNPP01/02/03; a write uses DNPP04 only when at least one record has references. Earlier vault requests and pool requests with empty references retain their existing format and narrower contract. Existing retained bodies, amounts, fee terms, reservations and timestamps remain unchanged.

The actual bound wallet request handler accepts optional pool_origins only on pool_payout. A supplied list must be nonempty and contain canonical sorted unique nonzero 64-character hex values; malformed fields refuse before signing/preflight/ingress. Listing exposes the references only after authenticating the retained owner. No payment label or audit string becomes an identity.

## Scope and limitations

The pool allocator must establish genuine immutable references within its allocation owner before dispatch. Caller-supplied references do not authenticate a pool, authorize an allocation, certify unpaid balances, or authorize reconstructing missing ownership. This change creates no pool origins and enrolls no historical rows. Older reference-free records cannot prove that a historical allocation is unpaid. Protection is scoped to records retained in this wallet; deletion, whole-wallet backup rollback, cross-wallet duplication, and pool database completeness are not certified.

The limit is an explicit capacity refusal, not truncation or load qualification. This does not implement the durable pool attempt owner, explicit funding-wallet/fee policy, canonical settlement, orphan handling for in-flight payments, or production notification provider. Retention is not confirmation. Release, all-consumer and whole-node gates remain separate; mainnet activation remains unset.

## Planned qualification

Four new patched owner/bound-handler cases exercise exact references and older records across reopen, disjoint allocations, overlapping regroup attempts, malformed lists and an unknown submission outcome with exact retry. Fixtures use synthetic funded wallet coins and an explicit component ingress, not external broadcast or network JSON transport. Existing request/vault tests and deadlines remain unchanged. Fresh full dinerod and declared wallet target ON/OFF will run fourteen selected CTests each; the complete linked project-C++ wallet graph will be freshly ASan/UBSan instrumented for 58 actual cases across nine groups. External/Rust/C/PQClean remain uninstrumented, macOS LSan is off, and daemon/OFF are outside that instrumented graph. No unsafe originals, synchronization-removal controls, or prohibited stress fixtures run.
