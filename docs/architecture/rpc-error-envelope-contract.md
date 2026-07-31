# JSON-RPC error envelope contract (#458)

## Before

Handlers on the live HTTP path signal failure by **returning** an object that
contains `"error"` rather than throwing. The unified dispatcher
(`src/daemon/http_rpc_server.cpp`) placed that object under `"result"` and set
the top-level `"error"` to `null`:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": null,
  "result": { "error": { "code": -32000, "message": "Block not activated: ..." } }
}
```

This is not a valid JSON-RPC response. A hard failure is indistinguishable from
success to any client that checks the top-level `"error"` — which is the correct
thing for a client to check.

The older `RPCServer::handleSingleRequest` already promoted such errors
(`src/daemon/rpc_server.cpp:699`), but that is not the live path.

Handlers also return two different shapes:

```json
{"error": {"code": -32000, "message": "..."}}   // structured
{"error": "some message"}                        // bare string
```

## After

The dispatcher normalises and promotes:

| handler returns | emitted envelope |
|---|---|
| object with integral `code` and string `message` | promoted verbatim to top-level `error` |
| bare string | `{"code": -32603, "message": <the string>}` |
| any other non-null value (number, array, object missing `code`/`message`) | `{"code": -32603, "message": "Handler reported a malformed error value", "data": <original>}` |
| `"error": null` alongside real data | unchanged — keeps its `result` |

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": { "code": -32000, "message": "Block not activated: ..." }
}
```

The daemon emits **only** the valid top-level form after migration. Dual-shape
support belongs temporarily in clients, not in the daemon.

## Consumer audit

### Compatible — read the top-level error already

These improve: they currently cannot see handler-returned errors at all.

- `src/cli/main.cpp:647` — `responseObj.isMember("error") && !isNull()`
- `src/cli/health_client.cpp:68`
- `src/tools/dinero_health_cli.cpp:88`
- `src/tools/dcli_wallet.cpp:159`

### Affected — read the nested `result.error`

**`dinero-qt` (separate repo) — COMPATIBLE, no code changes required.**

An earlier revision of this document listed dinero-qt as a hard blocker with
"9+ nested sites". That was a grep-based classification that did not check what
`result` was bound to at each site. Corrected by reading the data flow:

- `src/rpcclient.cpp:368` — the central client already handles the **top-level**
  error and requires `isObject()`, exactly the normalized shape emitted here.
  Errors are intercepted centrally and routed to `rpcError` before any widget
  callback runs.
- `src/shieldedwidget.cpp:620` — the comment there reads "...instead of a
  JSON-RPC error envelope. **Detect both shapes.**" It already implements
  dual-shape handling.
- `src/hardwarewalletwidget.cpp` (×7) and `src/mainwindow.cpp:7952` — these take
  the **full envelope** (`handleXResult(result.toObject())`, branching on
  `contains("result")` vs `contains("error")`). Already correct.
- `src/walletwizard.cpp:2019` — the one genuine nested read, inside a **success**
  callback that receives the extracted `.result`. It supports both generations
  indirectly: old nested errors reach the success callback, new top-level errors
  route through the central error callback. The fallback becomes dead but
  harmless.

dinero-qt should gain more correct behaviour from this change, not less: errors
previously buried in `result` were invisible to the central client and will now
surface through the standard `rpcError` channel.

**`DineroDPI` (separate repo) — AUDITED, compatible with caveats.** New
top-level errors are handled correctly and embedded NodeCore is unaffected.
**Recommended before deployment:** centralized dual-shape normalization plus
regression tests, rather than per-call-site handling. Not a hard blocker, but
not a no-op either.

**In-repo scripts — ~30 sites** across `scripts/test_key_import*.sh`,
`test_wif_support.sh`, `test_encrypted_import*.sh`, `test_rate_limiting_only.sh`,
`test_final_validation.sh`, `run-a2z.sh`, `continuous_stress_test.sh`. Mostly
diagnostic printing rather than control flow, but several gate on
`jq -e '.result.error'`.

**Integration tests — ~25 sites.** Most were written defensively as
`.result.error == "x" or .error != null` and tolerate the change. Two asserted
the nested shape strictly and are updated in this PR to accept both:
`test_shielded_rpc_getaddress.sh`, `test_shielded_rpc_transfer_addressed_e2e.sh`.

## Rollout

The three nodes are all operator-controlled, which makes a coordinated window
practical. Consumers are the risk, not the nodes.

1. Audit every consumer for `.result.error` (this document; complete — **no
   known hard blocker**).
2. Update clients to accept **both** the old nested and the new top-level shape.
3. Land DineroDPI's centralized dual-shape normalization and its regression
   tests. dinero-qt needs no code change. Do not touch the `dinero-qt`
   `spec/my-node-dashboard` checkout — it has uncommitted work in
   `src/mainwindow.cpp`.
4. Merge and deploy the daemon envelope correction across the three nodes in a
   controlled window.
5. Runtime-verify on every node, with BOTH:
   - a **handler-returned** error that this change promotes (e.g.
     `generatetoaddress` to an invalid address, or `ct.setminfee` with no
     parameter for the string-normalization path) — an unknown-method error is
     NOT sufficient, since that path already produced a top-level error before
     this change and would pass either way;
   - one successful RPC, to confirm the success envelope is unaffected.
6. Later, remove the nested-shape compatibility from clients.

## Verification

`RpcErrorEnvelope` pins the contract structurally with `jq` — not substring
matching, which is overbroad (legitimate result data, or any string containing
`"error":{`, would trip it). It covers the success shape, an unknown-method
error, a structured handler error, and a bare-string handler error
(`ct.setminfee` with no parameter, `src/rpc/ct_fee_rpc.cpp:59`).

Neuter-verified: with promotion disabled the test fails and prints the malformed
envelope verbatim.
