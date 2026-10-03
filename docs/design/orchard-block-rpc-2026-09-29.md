# Owned block RPC reads

The registered context-aware getblock handler requests one owned presentation snapshot from the chainstate service. The service holds selected-chain ownership while capturing stored height, exact bytes, header and transaction IDs. Selected post-activation height uses the existing checked typed reader; historical height retains the existing historical reader. Missing, inconsistent or unavailable typed storage refuses without historical conversion or invented height. Backend-OFF cannot serve an active typed profile.

Raw verbosity returns the complete stored mixed wire frame including proofs. Existing verbose fields use the same snapshot and exact transaction identities. Retained side-branch bodies remain readable; this is stored identity, not consensus admission or current canonicality. The handler rejects malformed hash text before lookup.

Locally qualified actual handler cases cover genuine shield block raw/verbose reads, database reopen and disconnected retained body, unavailable/mismatched storage refusal/retry, historical bytes and input refusals. HTTP transport, raw peer propagation, production notification provider, automatic readmission, full shield/send/unshield and release qualification remain open. No unsafe originals. Mainnet unset.
