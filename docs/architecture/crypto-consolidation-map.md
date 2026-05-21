# Crypto Utility Consolidation Map

**Status:** Map / inventory — not a plan. Produced to give a single accurate
picture of every hash/crypto utility in the tree so a consolidation decision
can be made deliberately.
**Date:** 2026-05-20
**Branch this maps:** `dinero-main` @ `b359475d` (post PR #101 `c7464b51`).

---

## TL;DR

The crypto layer has accreted several overlapping implementations of the same
primitives:

- **SHA-256** has one dominant canonical implementation (`CSHA256`, 81
  consumers) **plus four other runtime entry points** that each carry their own
  SHA-256 code path.
- **RIPEMD-160** has **three compiled implementations**. Two of them
  (`src/crypto/ripemd160.*` and `src/core/crypto/ripemd160.*`) ship a
  **byte-identical header** declaring the **same `dinero::RIPEMD160_*`
  symbols**, each compiled into a *different* library.
- **HASH-160** has two thin wrapper headers plus an independent OpenSSL-backed
  wrapper class.
- `dinero_crypto_minimal` is a self-contained "crypto facade" amalgamation
  (keys + signing + bech32 + hashes in one unit). Its **header exists in two
  byte-identical copies**. 23 source files depend on it.
- Three files are **dead** (present in the tree, compiled by nothing).

PR #101 (`c7464b51`, "restore canonical RIPEMD160 vectors") fixed the
**correctness** of all three RIPEMD-160 implementations and added a shared
3-way vectors test. It did **not** fix the **structure** — there are still
three implementations, two with identical C-APIs. Correctness is closed;
duplication is not.

---

## What is `dinero_crypto_minimal`?

`dinero_crypto_minimal` is a single self-contained "crypto facade" module:
header `dinero_crypto_minimal.h`, implementation
`src/core/crypto/dinero_crypto_minimal.cpp` (~13.5 KB). It was added in the
**initial v8 snapshot** (`12df3cf9`, 2026-05-13) and was not authored as part
of any later design pass.

Its public surface bundles four unrelated concerns into one unit:

| Concern | Functions |
| --- | --- |
| Lifecycle | `CF_Init()`, `CF_Shutdown()` |
| Key material | `CF_GeneratePrivKey`, `CF_GenerateRandomBytes`, `CF_GetCompressedPubkey` |
| Signing | `CF_SignDER`, `CF_VerifyDER` |
| Address / encoding | `GenerateBech32Address`, `WIF_Compressed` |
| Hashes | `sha256`, `ripemd160`, `HASH160`, `DoubleSHA256`, `hmac_sha512` |

The shape — a `CF_`-prefixed C-style API, a "call once at startup" init, every
primitive amalgamated behind one header — is characteristic of a
**bootstrap/prototype-era module**: a quick all-in-one that let early v8 code
compile and run without a designed crypto layer. It then became load-bearing.
Today **23 source files** include it (full list below), spanning auth,
consensus, contracts, daemon, node, p2p, and wallet. The macOS build links the
Security framework specifically because of this file
(`CMakeLists.txt:411`).

It is not "wrong" — it works and is widely depended on — but it is an
**amalgamation that duplicates primitives** the rest of the tree already has
canonical implementations of, and it is the single largest obstacle to a clean
crypto layer.

---

## Inventory

### SHA-256 family

| Implementation (.cpp) | Header | Symbol surface | Compiled into | Consumers | Verdict |
| --- | --- | --- | --- | --- | --- |
| `src/crypto/sha256.cpp` (+ `sha256_simd.cpp`, `sha256_neon.cpp`, `sha256_arm_shani.cpp`, `sha256_arm_shani_wrapper.cpp`) | `include/crypto/sha256.h` | `class CSHA256` | main lib | **81** | **Canonical.** Bitcoin Core lineage, runtime SIMD dispatch. The SIMD `.cpp`s are accelerator backends of this one impl, not separate impls. |
| `src/common/sha256d.cpp` | `include/common/sha256d.h` | `Dinero::Common::sha256` / `double_sha256_raw` | `dinero_common`, `dinero_consensus`, main | **56** | Duplicate. Independent SHA-256 + SHA256d; used by `hash.cpp`. |
| `src/core/crypto/dinero_crypto_minimal.cpp` | `dinero_crypto_minimal.h` (×2 copies) | free fns `sha256()`, `DoubleSHA256()` | main lib | 23 (whole module) | Duplicate. Own SHA-256 code path inside the amalgamation. |
| `src/daemon/crypto_utils.cpp` | `src/daemon/crypto_utils.h` | `CryptoUtils::SHA256`, `CryptoUtils::DoubleSHA256` | daemon | 3 | Duplicate. OpenSSL EVP-backed wrapper class. |
| `src/zk/zkvm/sha256_gadget.cpp` | — | ZK circuit gadget | zk lib | — | **Legitimately separate.** In-circuit SHA, not a runtime hash. |
| `src/mining/gpu/sha256d_cuda_src.cpp.in`, `miner/cmake/sha256d_{cu,cl,metal}_src.cpp.in` | — | GPU kernel source | miner (runtime/NVRTC compiled) | — | **Legitimately separate.** Device code. |
| `src/p2p/sha256d.cpp` | `include/p2p/sha256d.h` | — | **nothing** | — | **DEAD.** Not in any build list. |
| `sha256_minimal.cpp` (repo root) | — | — | **nothing** | — | **DEAD.** Stray file at repo root. |

### RIPEMD-160 family

PR #101 (`c7464b51`) verified all three live implementations against canonical
vectors (`RIPEMD160('')` → `9c1185a5…8d31`) and added
`tests/crypto/test_ripemd160_vectors.cpp`, compiled three times — once against
each implementation (`RIPEMD160SrcVectors`, `RIPEMD160CoreVectors`,
`RIPEMD160StandaloneVectors`). **Correctness: resolved.** Structure below is
unchanged by #101.

| Implementation (.cpp) | Header | Symbol surface | Compiled into | Consumers | Verdict |
| --- | --- | --- | --- | --- | --- |
| `src/crypto/ripemd160.cpp` | `src/crypto/ripemd160.h` | `dinero::RIPEMD160_Init/Update/Final` C-API + inline `dinero::RIPEMD160()` | **`dinero_consensus`** | **19** | Live. Canonical post-#101. |
| `src/core/crypto/ripemd160.cpp` | `include/dinero/core/crypto/ripemd160.h` | `dinero::RIPEMD160_Init/Update/Final` C-API + inline `dinero::RIPEMD160()` | **main lib** | 4 | Live. Canonical post-#101. **Header is byte-identical to the row above** (verified `diff`). |
| `src/crypto/ripemd160_standalone.cpp` | `src/crypto/ripemd160_standalone.h` | `class dinero::crypto::CRIPEMD160` | main lib | 2 | Live. Canonical post-#101. Different API shape (class vs C-API). |

> **The two C-API rows above declare the same symbols in the same namespace
> from a byte-identical header**, each compiled into a different library. See
> "Structural hazards" below.

### HASH-160 family

| Surface | Defined in | Backed by | Consumers | Verdict |
| --- | --- | --- | --- | --- |
| inline `Hash160()`, `SHA256()` | `src/crypto/hash160.h` | `#include`s `dinero_crypto_minimal.h` | 6 | Thin wrapper — not an independent impl. |
| inline `Hash160()`, `SHA256()` | `include/dinero/core/crypto/hash160.h` | `#include`s `dinero_crypto_minimal.h` + core `ripemd160.h` | (via core) | Thin wrapper — not an independent impl. |
| `CryptoUtils::HASH160` | `src/daemon/crypto_utils.h` | OpenSSL EVP | 3 | Independent wrapper. |
| free fn `HASH160()` | `dinero_crypto_minimal.h` | self | 23 (whole module) | Part of the amalgamation. |
| reference impl | `src/wallet/reference/crypto.{h,cpp}` | uses `dinero_crypto_minimal` | header: 0 / `.cpp`: compiled | Header included by nobody — verify whether `.cpp` is still reachable. |
| `HASH160` | `src/crypto/hash_compat.h` | — | **0** | **DEAD.** |

---

## Consumer map

### `dinero_crypto_minimal.h` — 23 source consumers

```
src/auth/auth_store.cpp              src/daemon/node_identity.cpp
src/consensus/cic.cpp                src/daemon/p2p_manager.cpp
src/contracts/daemon_mediator.cpp    src/daemon/tx_mempool.cpp
src/core/consensus/cic.cpp           src/node/node_impl.cpp
src/crypto/hd_keychain.cpp           src/p2p/p2p_message.cpp
src/crypto/pbkdf2.cpp                src/wallet/address_minimal.cpp
src/daemon/cookie_auth.cpp           src/wallet/address.cpp
src/crypto/hash160.h                 src/wallet/bip39.cpp
src/crypto/hash_compat.h  (dead)     src/wallet/hd_wallet.cpp
include/dinero/core/crypto/hash160.h src/wallet/reference/crypto.cpp
                                     src/wallet/sqlite_wallet.cpp
                                     src/wallet/transaction_builder.cpp
                                     src/wallet/wallet_manager.cpp
```

### Other entry points

- `CSHA256` (`include/crypto/sha256.h`) — **81** consumers. The de-facto
  canonical SHA-256 of the project.
- `Dinero::Common` SHA-256d (`include/common/sha256d.h`) — **56** consumers.
- `dinero::RIPEMD160_*` via `src/crypto/ripemd160.h` — **19** consumers
  (consensus-side).
- `dinero::RIPEMD160_*` via `include/dinero/core/crypto/ripemd160.h` — **4**
  consumers (core/main-side).
- `CryptoUtils` (`src/daemon/crypto_utils.h`) — **3** consumers.
- `dinero::crypto::CRIPEMD160` (`ripemd160_standalone.h`) — **2** consumers.

---

## Overlap matrix

| Primitive | Canonical impl | Other reimplementations / wrappers in the tree |
| --- | --- | --- |
| SHA-256 | `CSHA256` | `Dinero::Common`, `dinero_crypto_minimal::sha256`, `CryptoUtils::SHA256`, `hash160.h` inline (×2, wrap minimal) |
| Double-SHA-256 | `CSHA256`-based / `Dinero::Common::double_sha256_raw` | `dinero_crypto_minimal::DoubleSHA256`, `CryptoUtils::DoubleSHA256` |
| RIPEMD-160 | (no single canonical — 3 co-equal) | `src/crypto/ripemd160`, `src/core/crypto/ripemd160` (identical C-API), `ripemd160_standalone` (class), `dinero_crypto_minimal::ripemd160` |
| HASH-160 | (none designated) | `crypto/hash160.h`, `core/crypto/hash160.h`, `CryptoUtils::HASH160`, `dinero_crypto_minimal::HASH160`, `wallet/reference/crypto`, `hash_compat.h` (dead) |
| HMAC-SHA-512 | `dinero_crypto_minimal::hmac_sha512` | (verify whether a second one exists in the wallet BIP32 path) |

---

## Dead files (present in tree, compiled by nothing)

| File | Evidence |
| --- | --- |
| `src/crypto/hash_compat.h` | 0 `#include` consumers anywhere. |
| `src/p2p/sha256d.{h,cpp}` | Not referenced by any `CMakeLists.txt` / `.cmake`. |
| `sha256_minimal.cpp` (repo root) | Stray file at repo root; not in any build list. |

These can be removed in a standalone, zero-risk cleanup PR independent of any
consolidation decision.

---

## Structural hazards

1. **Duplicate `dinero::RIPEMD160_*` symbols.**
   `src/crypto/ripemd160.h` and `include/dinero/core/crypto/ripemd160.h` are
   **byte-identical** (verified). Each declares `dinero::RIPEMD160_Init`,
   `dinero::RIPEMD160_Update`, `dinero::RIPEMD160_Final` with external linkage.
   `src/crypto/ripemd160.cpp` compiles into `dinero_consensus`;
   `src/core/crypto/ripemd160.cpp` compiles into the main library. If any
   binary links both libraries, the same symbol is defined twice — a latent
   one-definition-rule hazard. It builds today, which means the current link
   graph happens to pull only one definition (static-archive resolution
   order), **not** that the duplication is safe. This is exactly the kind of
   landmine consolidation should remove.
   *Not yet verified against a built binary* — see follow-ups.

2. **Two byte-identical copies of `dinero_crypto_minimal.h`.**
   `src/crypto/dinero_crypto_minimal.h` and
   `include/dinero/core/crypto/dinero_crypto_minimal.h` are byte-identical
   (verified). Only one `.cpp` exists
   (`src/core/crypto/dinero_crypto_minimal.cpp`). The duplicate header means an
   edit to one is silently divergent from the other.

---

## Suggested direction (for decision — not yet decided)

The map exists so this can be chosen deliberately. Sketch only:

- **Canonical SHA-256:** `CSHA256` (`include/crypto/sha256.h`). It already has
  81 consumers and the SIMD dispatch story. Everything else should route to it.
- **Canonical RIPEMD-160:** pick **one** of the two identical C-API copies as
  the home and delete the other; route `CRIPEMD160` and
  `dinero_crypto_minimal::ripemd160` to it. Correctness is already locked by
  the #101 vectors test, so the move is mechanical.
- **Canonical HASH-160:** one `Hash160()` built from the two canonicals above;
  retire the wrapper headers.
- **`dinero_crypto_minimal`:** decompose. Its hash functions should forward to
  the canonicals; its key/sign/bech32/WIF surface is a separate concern that
  deserves its own designed home. Migrating 23 consumers is the bulk of the
  work and should be staged.
- **Dead files:** delete `hash_compat.h`, `p2p/sha256d.*`, `sha256_minimal.cpp`
  now — independent, zero-risk.

## Suggested follow-up investigations

1. Run `nm -C <built dinerod> | grep 'dinero::RIPEMD160_Init'` against a real
   build to confirm whether hazard #1 is "avoided by link graph" (count 1) or
   a tolerated duplicate symbol (count 2). The map could not — no built binary
   was available.
2. Confirm whether `src/wallet/reference/crypto.{h,cpp}` is still reachable
   from any build target, or is itself dead.
3. Check the wallet BIP32/BIP39 path for a second HMAC-SHA-512 implementation
   distinct from `dinero_crypto_minimal::hmac_sha512`.
