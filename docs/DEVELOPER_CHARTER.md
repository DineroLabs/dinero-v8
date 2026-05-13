# 🏛️ DineroCoin Developer Discipline Charter

**Version:** 1.0
**Date:** 2025-01-02
**Applies to:** All contributors (daemon, wallet, GUI, miner, FFI, SDKs)
**Purpose:** To preserve the integrity, predictability, and long-term maintainability of the DineroCoin codebase.

---

## 📚 Table of Contents
1. [Single Source of Truth](#1--single-source-of-truth)
2. [No Mock Builders in Production](#2--no-mock-builders-in-production)
3. [Architectural Fidelity](#3--architectural-fidelity)
4. [Zero Code Duplication Policy](#4--zero-code-duplication-policy)
5. [Test and Verify Before Merge](#5--test-and-verify-before-merge)
6. [Mainnet-Safe by Default](#6--mainnet-safe-by-default)
7. [Peer Review Is Mandatory](#7--peer-review-is-mandatory)
8. [Commit Discipline](#8--commit-discipline)
9. [Cultural Core Values](#9--cultural-core-values)
10. [Final Word from the Founder](#10--final-word-from-the-founder)
11. [License](#license)

---

## 1. 🔐 Single Source of Truth

There is exactly one authoritative source for wallet state and key derivation: **`HDWallet`**.
All transaction creation, PSBT generation, and address derivations **must** route through it.

**Reason:** Avoid duplicated derivation logic and inconsistent key paths.

**✅ Allowed**
```cpp
wallet.CreateTransaction(...);
wallet.FillPSBT(psbt);
wallet.GetDerivationInfo(address, info);
```

**❌ Forbidden**
```cpp
PSBTBuilder::AddInputManually(...);
TxBuilderV2::MockBip32Path(...);
generate_random_address();  // Bypass HD derivation
```

---

## 2. 🧩 No Mock Builders in Production

Test scaffolds (e.g., `tx_builder_v2.cpp`, `multi_account_rpc_handlers.cpp`) are for simulation only.
Any module that fakes PSBT metadata, change addresses, or keypaths must be behind:

```cpp
#ifdef MOCK_BUILD
// Mock code here
#endif
```

Mainnet builds must **not** compile mock code.

**Policy Enforcement:**
```cpp
#ifdef MAINNET_BUILD
    static_assert(false, "Mock builders are forbidden in mainnet mode");
#endif
```

---

## 3. ⚙️ Architectural Fidelity

- **Transaction creation** belongs in `HDWallet::CreateTransaction()`
- **PSBT filling** belongs in `HDWallet::FillPSBT()`
- **Consensus rules** belong in `chainparams.cpp` and `consensus/*.h`

**Never reproduce core logic elsewhere "just for testing."**

If you find yourself writing "temporary duplicate logic," you're breaking this charter.

---

## 4. 🔁 Zero Code Duplication Policy

Before adding new functionality:
1. Check whether a similar function already exists
2. Extend or reuse it instead of rewriting from scratch

Duplicate functions lead to inconsistent behavior between:
- GUI vs RPC
- FFI vs Daemon
- Tests vs Mainnet

**If two modules compute the same thing differently — one of them is wrong.**

---

## 5. 🧪 Test and Verify Before Merge

Every feature must include:
- At least one RPC-level test or CLI validation
- Automated CI check (Linux/macOS minimum)
- A decode or verification step showing expected results

**Example:**
```bash
./dinero-cli walletcreatefundedpsbt '[{"address":"din1q...", "amount":10.0}]'
./dinero-cli decodepsbt <psbt>
# Verify bip32_derivs are present and correct
```

---

## 6. 🧱 Mainnet-Safe by Default

All code paths compiled into mainnet must:
- Use verified consensus parameters
- Avoid mock UTXOs or fake derivations
- Reject incomplete data structures

If a function can produce invalid PSBTs or incomplete metadata, it must **fail fast**:

```cpp
throw std::runtime_error("PSBT missing HD metadata; use HDWallet::CreatePSBT()");
```

**No silent failures. No "it works on my machine." No shortcuts.**

---

## 7. 🔄 Peer Review Is Mandatory

Any PR touching:
- Wallet logic
- Consensus parameters
- Serialization or PSBT code

… must be reviewed by at least one senior contributor.

**Reviewers must verify:**
- No "shortcuts" or "temporary hacks" were introduced
- Code adheres to this Charter
- Feature is fully tested in mocknet/regtest mode

---

## 8. 📜 Commit Discipline

Each commit must:
- Have a descriptive title (`Fix PSBT derivation bug in HDWallet`)
- Include rationale in the message body
- Avoid vague terms

**Forbidden commit messages:**
- "quick fix"
- "temporary patch"
- "hack"
- "bypass for now"
- "TODO: fix later"

---

## 9. 🧭 Cultural Core Values

| Value | Meaning |
|-------|---------|
| **Integrity** | Code must do what it claims — no silent shortcuts |
| **Reproducibility** | Every binary must build identically from source |
| **Transparency** | Every change must be traceable and explainable |
| **Respect for the Chain** | Mainnet code is sacred — treat it like money |

---

## 10. 🚀 Final Word from the Founder

> "Dinero is money for free people.
> Freedom comes from trust, and trust comes from discipline.
> Every line of code we write reflects that principle."
>
> — Project Lead, DineroCoin

---

## License

© 2025 DineroCoin Project.
Licensed under the MIT License — see LICENSE for details.

**File:** `/docs/DEVELOPER_CHARTER.md`
**Last Updated:** 2025-01-02
