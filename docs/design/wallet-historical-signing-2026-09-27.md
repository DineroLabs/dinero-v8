# Explicit historical signing material

`SigningKey` carries an internal scalar, the exact consumed script and an explicit
untweaked, canonical TapTweak or historical import policy. Its stored secret
copies are cleansed on destruction and replacement. WalletManager's new typed
resolver obtains the policy in the same checked lookup as the key. The old
scalar-returning method keeps its original meaning. These are as-of copies;
neither interface pins an entire later transaction-signing job.

The actual TransactionSigner requests typed material. MapKeyProvider's explicit
BoundKeys constructor and WalletKeyProvider's signing_keys_by_input configuration
resolve typed keys only by exact outpoint. Existing hex/path providers retain
their canonical behavior; typed historical keys never become path aliases.

The named Taproot SignInputWithKey and SignInputV1WithKey entry points require the
bound script and complete input/outpoint correspondence. Canonical keys retain
existing origin and TapTweak checks. Historical keys use the original
SHA256(internal_xonly || 0x00) tweak with the secp256k1 keypair API, verify the
resulting public key against the consumed output, and sign the existing complete
sighash. A locally verified signature is staged before witness publication.
Historical imports require ordinary P2TR inputs; no HD path is fabricated.

The new component cases cover both internal-public parities, actual encrypted
reopen and typed lookup, both signer epochs, mixed modern/historical providers,
consensus verification for ordinary signatures, and policy/script/key/outpoint
refusals preserving the previous witness. V1 signatures are verified against the
existing V1 sighash; that alone is not whole-node activation qualification.
Production and fixtures were introduced together; no original-source RED claim.

This batch does not connect typed historical material to the old transaction
builder, production RPC selection/raw signing, or either PSBT implementation.
It does not certify complete key/script/account inventory, deletion/backup
completeness, cross-store issuance, whole-session authorization, pending owners,
network admission/broadcast, release readiness, or activation. Existing hex-map
and unrelated cryptographic temporary erasure remain narrower than the new
scoped material. Mainnet activation remains unset.

Local qualification: fresh genuine backend ON/OFF full daemon and declared
wallet targets built; 11 selected CTests passed in each configuration. All 93
linked project C++ translation units were rebuilt with ASan/UBSan; 28 actual
cases passed with 1,312 stable source/header inputs. Three copied omission
controls failed their intended assertions and the restored three cases passed.
Normal discovery uses archives; instrumented/control maps contain no project
C++ archive members. External libraries, Rust, C and PQClean are uninstrumented;
macOS LSan is off. Daemon, OFF and the separate readiness binary are outside
that instrumentation graph. Full ARM RocksDB qualification remains open.
The workflow adds a required three-case lane, retaining all 90 prior CTest
commands. Linux qualification of this exact source remains pending.
