# Shielding with a recorded BIP84 key

Preparation only: one deterministic isolated-chain case is written but not compiled or executed. It follows the separate maturity successor and requires the actual shield-reservations implementation. All original production and fixture bodies, registrations and deadlines are unchanged.

The fixture derives the compressed public key at m/84'/1448'/0'/0/7 from the existing wallet seed. It writes an explicit historical address/path/watch tuple for that exact derived key, with no private scalar export. The fixture does not call getNewAddress with a segwit argument: current issuance deliberately produces Taproot addresses. It does not label a legacy imported scalar as an HD key.

A real ordinary wallet payment funds that P2WPKH script. The normal selected owner retains the payment and actual history before admission/mining at height 103. Normal canonical wallet recovery must discover the output; the fixture never inserts its coin directly. It checks the stored amount, script, height and ordinary-coin flag against that mined output.

The shield owner must queue and prove the exact request, sign using the Orchard transparent digest, retain Ready and retire the proof job. The test requires a two-element witness with the exact compressed public key and low-S DER signature plus SIGHASH_ALL byte. It reopens the wallet and requires the same retained transaction with no proof job. Normal mempool admission and mining at height 104 validate the signature; normal recovery must credit 20,000 una to Orchard account 17. Address/path/watch metadata must remain byte-identical throughout.

The future CI lane checks one enabled 180-second registration and the exact executed case marker, retaining inventory/log. Publication and dispatch remain held while unrelated existing selectors include excluded tests. No existing gate is removed or waived.

Pending: actual API/compiler compatibility, runtime duration, fresh ON/OFF builds, actual case execution and complete linked-project sanitizer graph. This is not qualification of historical SHA256-tweak imported keys, backup completeness, deletion recovery, general account discovery, load, platforms or release readiness. A missing or contradictory historical owner must still refuse; do not weaken the fixture to manufacture successful recovery.
