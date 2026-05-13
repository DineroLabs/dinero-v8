# Covenant Semantic Freeze Policy

**Document Type:** MANDATORY NETWORK GOVERNANCE
**Status:** ACTIVE - Effective Immediately
**Authority:** Network Consensus Stability
**Last Updated:** 2025-12-24

---

## CRITICAL: Covenant Semantics Are Now FROZEN

This document declares a **permanent freeze** on the semantic behavior of all covenant opcodes. Once activated on mainnet, **NO SEMANTIC CHANGES** are permitted under any circumstances except for critical security vulnerabilities.

---

## 1️⃣ Policy Statement

### Frozen Opcodes

The following opcodes have **FROZEN SEMANTICS** as of 2025-12-24:

1. **OP_CHECKTEMPLATEVERIFY** (0xb3 / 179)
2. **OP_CHECKSIGFROMSTACK** (0xbb / 187)
3. **OP_CHECKSIGFROMSTACKVERIFY** (0xbc / 188)
4. **OP_TXHASH** (0xbd / 189)
5. **OP_CHECKCONTRACTVERIFY** (0xbe / 190)

### What This Means

**FORBIDDEN:**
- ❌ Changing hash computation methods
- ❌ Modifying validation rules
- ❌ Adding new features or flags
- ❌ "Improving" or "optimizing" behavior
- ❌ Changing error conditions
- ❌ Altering stack manipulation
- ❌ Tweaking consensus rules "just a little"

**ALLOWED:**
- ✅ Fixing critical security vulnerabilities ONLY
- ✅ Performance optimizations with IDENTICAL output
- ✅ Documentation improvements
- ✅ Code refactoring with IDENTICAL behavior
- ✅ Test additions

**Rationale:**
> "Just one tweak" → silent fork six months later

Semantic changes to consensus-critical code create **chain split risk**. Even "small improvements" can cause nodes to diverge. Users and applications depend on covenant behavior remaining **100% identical** forever.

---

## 2️⃣ Frozen Semantics: Opcode-by-Opcode

### OP_CHECKTEMPLATEVERIFY (0xb3)

**Frozen Behavior (BIP-119 Compliant):**
```
Input: <32-byte template hash>
Output: (verify or fail)

Hash Computation (FROZEN):
  hash = SHA256(SHA256(
    nVersion ||
    nLockTime ||
    scriptSig hash ||
    input count ||
    sequences hash ||
    output count ||
    outputs hash ||
    input index
  ))

Validation (FROZEN):
  1. Pop 32-byte hash from stack
  2. Compute template hash from transaction
  3. Compare hashes (must match exactly)
  4. Fail if any difference
```

**FROZEN Components:**
- Double SHA256 (no algorithm changes)
- Exact component ordering
- 32-byte hash size requirement
- Input index handling
- Error messages and conditions

**Examples of FORBIDDEN Changes:**
- ❌ "Let's use SHA3 instead" → SEMANTIC CHANGE (fork)
- ❌ "Add optional OP_TXHASH integration" → SEMANTIC CHANGE (fork)
- ❌ "Make hash comparison case-insensitive" → SEMANTIC CHANGE (fork)
- ❌ "Allow 31-byte hashes for efficiency" → SEMANTIC CHANGE (fork)

**Examples of ALLOWED Changes:**
- ✅ Optimize SHA256 implementation (same output)
- ✅ Add test vectors
- ✅ Improve error message clarity (same failure conditions)

---

### OP_CHECKSIGFROMSTACK (0xbb) & OP_CHECKSIGFROMSTACKVERIFY (0xbc)

**Frozen Behavior (BIP340 Schnorr):**
```
Input: <signature> <message> <pubkey>
Output: <true/false> (CSFS) or (verify) (CSFSVERIFY)

Signature Verification (FROZEN):
  1. Pop 64-byte Schnorr signature
  2. Pop message (variable length)
  3. Pop 32-byte x-only public key
  4. Verify: signature valid for (message, pubkey) using BIP340
  5. Push true/false (CSFS) or fail if false (CSFSVERIFY)

Signature Scheme (FROZEN):
  - BIP340 Schnorr (secp256k1-zkp library)
  - 64-byte signatures
  - 32-byte x-only public keys
  - Exact BIP340 verification algorithm
```

**FROZEN Components:**
- BIP340 Schnorr algorithm
- Signature/pubkey sizes (64/32 bytes)
- Stack order (sig, msg, pubkey)
- secp256k1 curve parameters
- Verification library (secp256k1-zkp)

**Examples of FORBIDDEN Changes:**
- ❌ "Support ECDSA signatures too" → SEMANTIC CHANGE (fork)
- ❌ "Allow 33-byte compressed pubkeys" → SEMANTIC CHANGE (fork)
- ❌ "Change stack order to (pubkey, msg, sig)" → SEMANTIC CHANGE (fork)
- ❌ "Switch to libsecp256k1 instead of zkp" → RISK (possible behavior change)

**Examples of ALLOWED Changes:**
- ✅ Update secp256k1-zkp to newer version (if output identical)
- ✅ Add batch verification optimization (same result)
- ✅ Improve error message for invalid pubkey size

---

### OP_TXHASH (0xbd)

**Frozen Behavior (Transaction Introspection):**
```
Input: <flag_byte>
Output: <hash> (32 bytes or empty)

Hash Computation (FROZEN):
  1. Pop 1-byte flag
  2. Extract transaction components based on flag:
     - 0x01: Inputs hash
     - 0x02: Outputs hash
     - 0x03: Sequences hash
     - 0x04: ScriptSigs hash
     - 0x05: Input index
     - 0x06: Output count
     - 0x07: Locktime
     - 0x08: Version
     - 0x09: Current input scriptPubKey
     - 0x0A: Current input value
     - 0x0B: Specific output (requires vout index)
     - 0x0C: Specific input (requires vin index)
  3. Hash component with SHA256
  4. Push result (32 bytes) or empty if unknown flag

Unknown Flag Behavior (FROZEN):
  - Returns empty hash (fail-soft)
  - Does NOT fail transaction
  - Design choice: Allows forward compatibility
```

**FROZEN Components:**
- 12 defined flags (0x01-0x0C)
- SHA256 hashing for all components
- Unknown flag → empty hash behavior
- Component serialization format
- Index handling for 0x0B and 0x0C

**Examples of FORBIDDEN Changes:**
- ❌ "Add new flag 0x0D for witness data" → SEMANTIC CHANGE (fork)
- ❌ "Make unknown flags fail instead of empty hash" → SEMANTIC CHANGE (fork)
- ❌ "Use different hash for flag 0x02" → SEMANTIC CHANGE (fork)
- ❌ "Change serialization format" → SEMANTIC CHANGE (fork)

**Examples of ALLOWED Changes:**
- ✅ Optimize hash computation (same output)
- ✅ Add documentation for all flags
- ✅ Add test coverage for unknown flags

**Note on Unknown Flags:**
The fail-soft behavior (empty hash for unknown flags) is **INTENTIONAL** and **FROZEN**. This was documented in Phase 3 audit as a design choice. Do NOT change to fail-hard.

---

### OP_CHECKCONTRACTVERIFY (0xbe)

**Frozen Behavior (Contract State Transitions):**
```
Input: <prev_state_bytes> <new_state_bytes>
Output: (verify or fail)

State Format (FROZEN):
  stateHash (32 bytes) ||
  codeHash (32 bytes) ||
  counter (4 bytes, little-endian uint32) ||
  dataLen (4 bytes, little-endian uint32) ||
  data (variable)

Validation Rules (FROZEN):
  1. Deserialize previous state
  2. Deserialize new state
  3. Verify: newState.counter == prevState.counter + 1 (modulo 2^32)
  4. Verify: newState.codeHash == prevState.codeHash (immutability)
  5. Verify: newState.stateHash == SHA256(codeHash || counter || data)
  6. Fail if any rule violated

Counter Overflow (FROZEN):
  - 0xFFFFFFFF + 1 = 0 is ALLOWED (modular arithmetic)
  - This is intentional design (not a bug)
```

**FROZEN Components:**
- State serialization format (72 + data.size() bytes)
- Counter increment rule (exactly +1, modulo 2^32)
- Code hash immutability
- State hash computation (SHA256)
- Little-endian serialization
- Counter overflow behavior

**Examples of FORBIDDEN Changes:**
- ❌ "Allow counter to skip ahead for batching" → SEMANTIC CHANGE (fork)
- ❌ "Make code hash mutable with signature" → SEMANTIC CHANGE (fork)
- ❌ "Use different state hash algorithm" → SEMANTIC CHANGE (fork)
- ❌ "Prevent counter overflow at 0xFFFFFFFF" → SEMANTIC CHANGE (fork)
- ❌ "Change serialization to big-endian" → SEMANTIC CHANGE (fork)

**Examples of ALLOWED Changes:**
- ✅ Optimize deserialization (same result)
- ✅ Add bounds checking for data length
- ✅ Improve error message clarity

---

## 3️⃣ What Constitutes a "Bugfix"

### Definition

A **bugfix** is a code change that:
1. Fixes a **critical security vulnerability** (funds loss, DoS, consensus split)
2. Does NOT change observable behavior for valid inputs
3. Only affects invalid/malicious inputs
4. Is approved by network governance process

### Security Vulnerability Severity Levels

**CRITICAL (Immediate Fix Required):**
- Consensus split risk
- Permanent funds loss
- Network-wide DoS
- Signature forgery
- Hash collision exploitation

**HIGH (Fix in Next Release):**
- Limited funds loss (specific edge case)
- Single-node DoS
- Memory exhaustion

**MEDIUM (Fix When Convenient):**
- Performance degradation
- Misleading error messages
- Non-critical edge cases

**LOW (Document Only):**
- Documentation errors
- Code style issues
- Non-functional improvements

### Examples of Valid Bugfixes

**Example 1: Stack Overflow Protection (CRITICAL)**
```cpp
// BEFORE (vulnerable to DoS):
void PushStack(ExecutionContext& ctx, const std::vector<uint8_t>& data) {
    ctx.stack.push_back(data);  // No limit
}

// AFTER (bugfix - prevents DoS):
bool PushStack(ExecutionContext& ctx, const std::vector<uint8_t>& data) {
    if (ctx.stack.size() >= 1000) {  // BIP342 limit
        ctx.error = "Stack size limit exceeded";
        return false;
    }
    ctx.stack.push_back(data);
    return true;
}
```
**Why Allowed:** Prevents DoS, only affects malicious inputs, matches BIP342 spec.

**Example 2: Off-by-One Error Fix (CRITICAL)**
```cpp
// BEFORE (vulnerable to buffer overflow):
if (offset + dataLen > bytes.size()) {  // Wrong: > instead of !=
    return false;
}

// AFTER (bugfix - prevents exploit):
if (offset + dataLen != bytes.size()) {  // Correct: exact match required
    return false;
}
```
**Why Allowed:** Fixes buffer overflow, tightens validation (soft fork), prevents funds loss.

### Examples of FORBIDDEN "Bugfixes"

**Example 1: "Improving" Hash Algorithm (FORBIDDEN)**
```cpp
// BEFORE:
hash = SHA256(SHA256(data));  // Double SHA256

// PROPOSED "Fix":
hash = SHA3(data);  // "SHA3 is more modern"
```
**Why FORBIDDEN:** This is a SEMANTIC CHANGE, not a bugfix. Would cause immediate chain split.

**Example 2: "Fixing" Counter Overflow (FORBIDDEN)**
```cpp
// BEFORE:
if (newState.counter != prevState.counter + 1) {  // Allows overflow
    return false;
}

// PROPOSED "Fix":
if (prevState.counter == 0xFFFFFFFF) {  // "Prevent overflow"
    ctx.error = "Counter overflow not allowed";
    return false;
}
```
**Why FORBIDDEN:** Counter overflow is INTENTIONAL design. Changing this is a SEMANTIC CHANGE.

**Example 3: "Optimizing" Unknown Flag Behavior (FORBIDDEN)**
```cpp
// BEFORE:
if (flag > 0x0C) {
    return std::array<uint8_t, 32>{};  // Empty hash (fail-soft)
}

// PROPOSED "Fix":
if (flag > 0x0C) {
    ctx.error = "Unknown TXHASH flag";  // "Fail-hard is better"
    return std::nullopt;
}
```
**Why FORBIDDEN:** Fail-soft behavior is INTENTIONAL design choice. This would cause chain split.

---

## 4️⃣ Emergency Bugfix Process

### When a Critical Vulnerability is Found

**Step 1: IMMEDIATE DISCLOSURE (If Exploited)**
- If vulnerability is actively exploited → PUBLIC DISCLOSURE IMMEDIATELY
- Post to GitHub security advisory
- Announce on all communication channels
- Coordinate with miners and exchanges

**Step 2: IMMEDIATE DISCLOSURE (If Not Exploited)**
- Responsible disclosure period: 0-7 days (depending on severity)
- Notify major operators privately first
- Prepare patch release

**Step 3: PATCH DEVELOPMENT**
- Fix MUST preserve semantics for valid inputs
- Fix MUST only affect invalid/malicious inputs
- Fix MUST be a soft fork (tightening rules, not loosening)

**Step 4: REVIEW AND APPROVAL**
- Minimum 2 independent code reviews
- Security audit of the fix itself
- Testnet deployment first (if time permits)

**Step 5: EMERGENCY RELEASE**
- Publish patched software
- Clear upgrade instructions
- Severity classification
- Impact assessment

**Step 6: NETWORK COORDINATION**
- Miners: Upgrade immediately
- Exchanges: Upgrade before accepting deposits/withdrawals
- Node operators: Upgrade within 48 hours

**Step 7: POST-MORTEM**
- Document vulnerability
- Document fix
- Update test coverage
- Improve documentation

### Example: Hypothetical Critical Vulnerability

**Scenario:** OP_CHECKTEMPLATEVERIFY hash collision found due to implementation error

**Vulnerability:**
```cpp
// BEFORE (vulnerable):
hash = SHA256(data);  // Single SHA256 (wrong!)

// CORRECT (BIP-119 spec):
hash = SHA256(SHA256(data));  // Double SHA256
```

**Emergency Process:**
1. ✅ IMMEDIATE PUBLIC DISCLOSURE (consensus-critical)
2. ✅ Patch: Change to double SHA256 (matches spec)
3. ✅ This is a BUGFIX (implementation error, not semantic change)
4. ✅ Soft fork: Tightens validation (old nodes accept more, new nodes reject)
5. ✅ Emergency release within 24 hours
6. ✅ Coordinate with miners and exchanges

**Why Allowed:**
- Implementation did not match frozen specification (BIP-119)
- Fix restores intended behavior (double SHA256)
- Critical security vulnerability (collision risk)

---

## 5️⃣ Semantic Freeze Exceptions

### ZERO Exceptions

There are **NO EXCEPTIONS** to the semantic freeze policy.

**Not Even For:**
- ❌ "Better" algorithms
- ❌ "More efficient" implementations
- ❌ "Fixing design flaws"
- ❌ "User requests"
- ❌ "Industry standards changes"
- ❌ "Future-proofing"

**Only Path to Semantic Changes:**
- NEW SOFT FORK with NEW OPCODES
- Do NOT modify existing opcodes
- Example: If you want SHA3, add OP_CHECKTEMPLATEVERIFY_SHA3 (new opcode)

### Hard Fork Policy

**Question:** Can we hard fork to change covenant semantics?

**Answer:** NO, except for catastrophic network failure.

**Rationale:**
- Hard forks split the network
- Users lose funds on minority chain
- Exchanges must choose which chain to support
- Covenant contracts become worthless on losing chain

**Catastrophic Failure Definition:**
- Majority of network under attack
- Fundamental cryptographic break (SHA256 collision, secp256k1 break)
- No other option to preserve network

---

## 6️⃣ Enforcement Mechanism

### Code Freeze

**Repository Policy:**
- Pull requests modifying covenant opcodes: REJECTED unless bugfix
- Bugfix PRs: Require security audit + 2 independent reviews
- Semantic change PRs: REJECTED with link to this document

### Review Checklist (For Bugfix PRs)

```
Covenant Bugfix Review Checklist
================================

[ ] Does this PR modify covenant opcode behavior?
    If NO → Approve (not subject to freeze)
    If YES → Continue checklist

[ ] Is this a critical security vulnerability?
    If NO → REJECT (no non-security changes allowed)
    If YES → Continue checklist

[ ] Does the fix change behavior for valid inputs?
    If YES → REJECT (semantic change, not bugfix)
    If NO → Continue checklist

[ ] Is the fix a soft fork (tightening validation)?
    If NO → REJECT (loosening rules = hard fork)
    If YES → Continue checklist

[ ] Has the fix been security audited?
    If NO → REQUEST AUDIT
    If YES → Continue checklist

[ ] Are there at least 2 independent code reviews?
    If NO → REQUEST REVIEWS
    If YES → Continue checklist

[ ] Has the fix been tested on testnet?
    If NO → REQUEST TESTNET DEPLOYMENT
    If YES → Continue checklist

[ ] Is there a clear upgrade plan for operators?
    If NO → REQUEST UPGRADE PLAN
    If YES → APPROVE (with caution)
```

---

## 7️⃣ Developer Guidance

### You Want to Improve Covenant Opcodes?

**DON'T:**
- Modify existing opcode semantics
- "Fix" design choices you disagree with
- Add features to existing opcodes

**DO:**
- Propose new opcodes (new soft fork)
- Improve documentation
- Add test coverage
- Optimize implementation (same output)

### Example: You Want TXHASH to Fail on Unknown Flags

**WRONG Approach:**
```cpp
// Modifying OP_TXHASH behavior (FORBIDDEN)
if (flag > 0x0C) {
    ctx.error = "Unknown flag";
    return false;  // ← SEMANTIC CHANGE (chain split!)
}
```

**CORRECT Approach:**
```cpp
// Propose new opcode: OP_TXHASH_STRICT (new soft fork)
// OP_TXHASH keeps original behavior (fail-soft)
// OP_TXHASH_STRICT uses new behavior (fail-hard)
// Users choose which opcode to use
```

### Example: You Want to Add SHA3 Support to CTV

**WRONG Approach:**
```cpp
// Modifying OP_CHECKTEMPLATEVERIFY (FORBIDDEN)
if (stack.back().size() == 33 && stack.back()[0] == 0x01) {
    use_sha3 = true;  // ← SEMANTIC CHANGE (chain split!)
}
```

**CORRECT Approach:**
```cpp
// Propose new opcode: OP_CHECKTEMPLATEVERIFY_SHA3 (new soft fork)
// OP_CHECKTEMPLATEVERIFY keeps SHA256 (frozen)
// OP_CHECKTEMPLATEVERIFY_SHA3 uses SHA3 (new behavior)
// Users choose which opcode to use
```

---

## 8️⃣ User Protection

### Why This Policy Exists

**Without Semantic Freeze:**
```
Day 0:   User creates CTV vault (v1 semantics)
Day 100: Developer "improves" CTV hash algorithm
Day 101: User's vault becomes unspendable (semantic change)
Day 102: User loses funds (no way to recover)
```

**With Semantic Freeze:**
```
Day 0:   User creates CTV vault (frozen semantics)
Day 100: Developer proposes OP_CTV_V2 (new opcode)
Day 101: User's vault still works (v1 frozen)
Day 102: New users can choose v2, old users keep v1
Day ∞:   User's vault works forever (semantics never change)
```

### Guarantees to Users

**DineroCoin Covenant Guarantee:**

> Once a covenant opcode is activated on mainnet, its behavior will NEVER change.
>
> Scripts written today will work identically in 10 years.
>
> Contracts created today will remain spendable forever.
>
> The only exceptions are critical security bugfixes that tighten validation (soft forks).

**What Users Can Rely On:**
- ✅ CTV template hashes computed the same way forever
- ✅ CSFS signature verification using BIP340 forever
- ✅ TXHASH flags returning same data forever
- ✅ CCV state transitions validating same rules forever
- ✅ Counter overflow behavior unchanged forever
- ✅ Unknown TXHASH flags returning empty hash forever

**What Users CANNOT Rely On:**
- ❌ Performance (optimizations allowed)
- ❌ Error message text (clarity improvements allowed)
- ❌ Internal code structure (refactoring allowed)

---

## 9️⃣ Testnet vs Mainnet

### Testnet Policy

**Testnet:** Semantic changes ALLOWED (for experimentation)

**Purpose:**
- Test new covenant features before proposing soft fork
- Experiment with alternative designs
- Break things without consequences

**Warning:**
> DO NOT use testnet for production contracts.
> Testnet semantics may change without notice.
> Only mainnet has semantic freeze guarantee.

### Mainnet Policy

**Mainnet:** Semantic freeze ENFORCED (permanent)

**Once Activated:**
- Semantics frozen forever
- Only critical bugfixes allowed
- New features require new opcodes

---

## 🔟 Compliance Verification

### How to Verify Semantic Freeze Compliance

**For Code Reviewers:**
1. Check if PR modifies any covenant opcode handler
2. If yes, verify it's a security bugfix (not feature addition)
3. Verify behavior unchanged for valid inputs
4. Require security audit
5. Require 2+ independent reviews

**For Developers:**
1. Run adversarial test suite (Phase 4)
2. Verify all 38 tests still pass
3. Add test for bugfix scenario
4. Verify no regression in valid inputs

**For Auditors:**
1. Review Phase 3 frozen specifications
2. Compare current implementation to frozen spec
3. Flag any deviations
4. Verify bugfix justification (security critical?)

---

## Summary

**Frozen Opcodes:**
- OP_CHECKTEMPLATEVERIFY (0xb3)
- OP_CHECKSIGFROMSTACK (0xbb)
- OP_CHECKSIGFROMSTACKVERIFY (0xbc)
- OP_TXHASH (0xbd)
- OP_CHECKCONTRACTVERIFY (0xbe)

**Frozen Date:** 2025-12-24
**Freeze Scope:** All semantic behavior
**Exceptions:** Critical security bugfixes only
**Enforcement:** Code review + adversarial testing

**User Guarantee:**
> Covenant semantics are frozen forever. Your contracts will work identically in 10 years.

**Developer Guidance:**
> Want new features? Propose new opcodes. Don't modify frozen ones.

**Emergency Contact:**
> Critical vulnerability found? Follow emergency bugfix process (Section 4).

---

**This policy is MANDATORY and PERMANENT.**

Violation of this policy creates chain split risk and endangers user funds.

**Status:** ACTIVE - Effective immediately
**Review Date:** Never (permanent freeze)
**Modification Authority:** None (except critical security bugfixes)

---

**End of Semantic Freeze Policy**
