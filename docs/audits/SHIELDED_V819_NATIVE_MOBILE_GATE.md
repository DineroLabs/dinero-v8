# Shielded v8.1.9 native mobile gate

## Scope

Production mobile clients are:

- iOS/macOS: native Xcode DineroDPI at `/Users/haydarevich/src/apps/DineroDPI`.
- Android: native Kotlin/JNI DineroDPI at
  `/Users/haydarevich/src/apps/DineroDPIAndroid`.

`mobile-tauri/` is an experimental, deprecated prototype. Its Rust plugins,
build status, and UI are outside the shielded consensus and mobile release
gates.

## Capacity audit

- Canonical payload: 75 bytes.
- Encoded address: approximately 132 characters.
- Xcode DineroDPI calls `dinero_shielded_derive_address`, detects
  `DINERO_SHIELDED_ERR_BUFFER_TOO_SMALL`, and retries using the native-reported
  capacity. The final value is a Swift `String`.
- Android DineroDPI derives through JNI, returns JSON/Java strings, and stores
  addresses as Kotlin strings. No fixed 128-byte destination is used. Its
  payment-request parser now accepts canonical `dins1`, `tdins1`, and `rdins1`
  lengths instead of rejecting every address above 100 characters.
- Generic `wallet_ffi` legacy 128-byte structures are not the native shielded
  address derivation path. Versioned 192-byte structures exist for consumers
  that need QR/notification/swap addresses through that ABI.

## Required release evidence

1. Pin the canonical mainnet/testnet/regtest address vectors in Swift and
   Kotlin/JNI tests.
2. Round-trip a 132-character address through receive display, clipboard,
   QR generation, QR scan, URI parsing, and send confirmation.
3. Verify recipient-authority notes remain discoverable and spendable after
   app restart and a reorg crossing ordinary blocks.
4. Test immediately before, at, and after the spend-authority reset boundary.
5. Confirm sender rejection and recipient success for proof version `0x05`.
6. Measure peak RSS and wall time for output proving, spend proving, witness
   construction, and a complete one-note transfer on physical devices.
7. Keep secrets in iOS Keychain/Secure Enclave and Android Keystore; never
   route them through a web/Tauri bridge.

Run both native build gates locally with:

```sh
scripts/shielded-native-mobile-readiness.sh
```

## Status

The capacity design is sound by inspection and test. On 2026-08-30:

- Native Xcode DineroDPI built for both the simulator and a physical iPhone 15
  Pro Max with ShieldedProverKit linked. Seventeen focused address, receive,
  and prover-kit tests passed, including the canonical daemon address vector.
- The signed app installed and launched on the iPhone, and a 10-second physical
  Allocations trace was captured as `ios-physical-allocations.trace`.
- The Android payment parser was corrected to accept shielded HRPs and lengths.
  Focused tests prove that the canonical address is longer than 128 characters
  and survives raw parsing and payment-URI round-trip byte-for-byte.
- Android unit tests and `assembleDebug` passed with arm64-v8a and x86_64 JNI
  artifacts. A safely isolated `.debug` application installed and launched on
  a Pixel 7 without replacing the signed production wallet. Its idle startup
  baseline was 186,302 KB PSS and 329,524 KB RSS.

Evidence is under `artifacts/shielded-native-mobile/`. Peak proof RSS/time and
full QR camera/display confirmation during a real funded-note transfer remain
device gates; an idle baseline is not a substitute for proof measurement.
Tauri has no status in this decision.
