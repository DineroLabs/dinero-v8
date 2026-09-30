# Typed vault RPC arguments

Vault observe and withdraw require a one-object positional array. Account identifiers must be nonempty strings without NUL. Deposit indices must be JSON integers in the uint32 range, and withdrawal amounts must be positive uint64 JSON integers. Strings, booleans and floating values are not coerced into money or indices. Deposit amount/height/hash continue to come from the checked canonical source; legacy caller fields remain ignored.

All transaction/request/script hex is decoded nibble by nibble with exact fixed identifier lengths; both letter cases are accepted. A typed destination_address retains its existing precedence over the raw script field. A present address of the wrong type refuses instead of falling back to another destination. Existing destination policy and account authority are unchanged.

Operator updates require an explicit string address; the empty string remains the explicit disable action. An optional account must be a string. Missing/mistyped values no longer select the disable path. Existing account read/status handlers require a one-string array. These checks run before state-changing service calls.

Three common patched-path cases use the actual runtime, service, and handlers with synthetic deposits. They verify valid integer enqueue/status, malformed typed inputs preserving ledger/metrics/queue, exact hex refusal and explicit operator binding/disable semantics. No signing callback, unsafe-original, race, deadlock or synchronization-removal controls are run. This does not provide durable vault ownership, signing request integration, authorization beyond existing RPC policy, or release readiness. Locally qualified.
