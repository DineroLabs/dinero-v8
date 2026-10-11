# Network upgrade notice (`network-upgrade.json`)

Published as an asset on every GitHub release that changes consensus or P2P
compatibility. Wallets fetch it from
`https://github.com/DineroLabs/dinero-v8/releases/latest/download/network-upgrade.json`.
An absent asset (HTTP 404) means "no mandatory upgrade pending".

```json
{
  "schema": "dinero.network-upgrade.v1",
  "network": "mainnet",
  "profile": "compact-v1-60s-v1",
  "release": "8.1.13",
  "activation_height": 125000,
  "min_versions": { "dinero-qt": "8.1.13", "dinerod": "8.1.13", "dinerodpi": "1.4.0" },
  "message": "v8.1.13: 60-second blocks and the new private pool. Update before block 125000."
}
```

Rules:

- `activation_height` is a positive integer below 4294967295.
- Unknown fields are ignored. Any parse or schema error means "no notice".
- Versions are dotted integers; any suffix after the third number
  (for example `-metal-fix1`) is ignored for comparison.
- A client whose own key is missing from `min_versions` treats the notice as
  absent.
- The notice is advisory. Consensus is enforced by the node.

Publishing rule: wallets read the notice from the release GitHub marks as
"latest" (prereleases are skipped). Attach the current `network-upgrade.json`
to **every** normal release from the moment it is first published until a newer
notice replaces it, including hotfix releases such as `v8.1.13-windows1` if they
are published as normal releases. A release without the asset looks like "no
upgrade pending" to older wallets and silences their warning.

The height above is an example. Publish the real activation height only once
it is fixed in the release's chain parameters.

Client behaviour is pinned by `qt/tests/vectors/network_upgrade_policy_v1.json`
(byte-identical copy in DineroDPI `DineroDPI/test-vectors/`).
