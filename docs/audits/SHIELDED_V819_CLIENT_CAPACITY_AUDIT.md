# Shielded v8.1.9 client-capacity audit

## Result

The canonical shielded address payload is 75 bytes and the encoded mainnet,
testnet, and regtest strings are approximately 132 characters. The prover-kit
C ABI is safe when callers use its documented two-call capacity protocol and
has a regression test for `BUFFER_TOO_SMALL`.

Repository-wide inspection found generic wallet ABI structures with 128-byte
address fields. The legacy ABI remains unchanged, while new v2 QR,
notification, and swap structures use 192-byte fields; parsers reject an
oversized legacy result instead of truncating it. The shielded prover-kit uses
its dynamic capacity protocol and does not rely on those generic structs.

The production iOS DineroDPI wrapper deliberately starts address derivation
with a 128-byte buffer, handles `DINERO_SHIELDED_ERR_BUFFER_TOO_SMALL`, and
retries with the exact required capacity. Swift stores the result as `String`.
The Android DineroDPI JNI path returns shielded addresses as Java/Kotlin
strings and persists them as dynamic strings. Neither production path has a
128-byte shielded-address storage ceiling.

The DPI address limit is now 192. Qt uses `QString` and RPC uses dynamic
strings.

## Release gate

- Prover-kit callers must query required capacity and allocate dynamically.
- Swift and Kotlin tests pin the canonical mainnet address vector.
- Android QR/URI parsing covers a canonical address longer than 128 characters
  plus label and amount; physical camera/display confirmation is still
  required.
- Legacy wallet-core ABI callers must migrate to the v2 structures before
  transporting canonical shielded addresses.
- `mobile-tauri/` is a deprecated prototype and is explicitly outside the
  shielded activation gate.
