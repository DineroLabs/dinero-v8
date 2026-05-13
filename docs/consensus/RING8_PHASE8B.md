# Ring 8 Phase 8b: Extension Gating & Activation

**Status**: Active
**Date**: 2026-01-03
**Purpose**: Enable safe evolution while maintaining Ring 7 immutability

---

## Mission

**Ring 7 froze meaning. Ring 8a froze the freeze. Ring 8b enables safe evolution.**

Phase 8b provides mechanisms to extend DineroCoin's consensus without breaking Ring 7 semantics (S1-S25). Extensions are strictly isolated through version gating and namespace gating.

---

## The Three Properties (EG1-EG3)

### EG1: Namespace Isolation

**Property**: New opcodes in extension namespaces do not affect Ring 7 (CORE namespace) scripts.

**What this means:**
- CORE namespace (Ring 7 opcodes) is frozen forever (per Phase 8a: BC2)
- Extension opcodes live in separate namespaces (EXTENSION_1, EXTENSION_2, ...)
- Namespaces are strictly isolated - no cross-namespace opcode collisions affect semantics

**Namespace Structure:**
```
OpcodeNamespace::CORE         = 0   (Ring 7 - FROZEN)
OpcodeNamespace::EXTENSION_1  = 1   (First extension namespace)
OpcodeNamespace::EXTENSION_2  = 2   (Second extension namespace)
...
```

**Example (ALLOWED):**
```cpp
// CORE namespace (Ring 7 - frozen)
CORE::OP_1        = 0x51
CORE::OP_ADD      = 0x93
CORE::OP_CHECKSIG = 0xac

// EXTENSION_1 namespace (isolated)
EXTENSION_1::OP_EXT_COVENANT  = 0xe0
EXTENSION_1::OP_EXT_ASSET     = 0xe1

// These namespaces are ISOLATED
// Ring 7 scripts cannot see EXTENSION_1 opcodes
// EXTENSION_1 scripts can use both CORE and EXTENSION_1 opcodes
```

**Example (FORBIDDEN):**
```cpp
// ❌ FORBIDDEN: Cannot modify CORE namespace
CORE::OP_NEW_FEATURE = 0xff  // Violates EG1 + BC2

// ❌ FORBIDDEN: Cannot redefine CORE opcodes
CORE::OP_ADD = 0x93  // Different implementation
```

**Enforcement:**
- OpcodeNamespaceManager tracks namespace assignments
- CORE namespace is initialized once and frozen
- Extension registrations validate against CORE immutability

---

### EG2: Version Isolation

**Property**: Script version 0 (Ring 7) never accesses version 1+ features.

**What this means:**
- Script versions are strictly isolated
- VERSION_0 (Ring 7) scripts execute in frozen Ring 7 environment
- VERSION_1+ scripts can access extension features
- No cross-version feature leakage

**Version Structure:**
```
ScriptVersion::VERSION_0 = 0   (Ring 7 - FROZEN - S1-S25)
ScriptVersion::VERSION_1 = 1   (First extension version)
ScriptVersion::VERSION_2 = 2   (Second extension version)
...
```

**Example (VERSION_0 - Ring 7):**
```
Script: [OP_1, OP_2, OP_ADD]
Version: VERSION_0
Available opcodes: CORE namespace only
Execution: Ring 7 executor (frozen semantics S1-S25)
Result: ✅ Executes with Ring 7 semantics
```

**Example (VERSION_1 - Extension):**
```
Script: [OP_1, OP_EXT_COVENANT, OP_VERIFY]
Version: VERSION_1
Available opcodes: CORE + EXTENSION_1 namespaces
Execution: Extension-aware executor
Result: ✅ Executes with VERSION_1 semantics (if activated)
```

**Example (FORBIDDEN):**
```
Script: [OP_1, OP_EXT_COVENANT]
Version: VERSION_0
Result: ❌ REJECTED - VERSION_0 cannot use extension opcodes (EG2 violation)
```

**Enforcement:**
- ScriptVersionDispatcher routes execution by version
- VERSION_0 always uses frozen Ring 7 executor
- VERSION_1+ requires extension activation

---

### EG3: Activation Safety

**Property**: Extensions activate only when explicitly gated and approved by network.

**What this means:**
- No implicit activation (all extensions require explicit gating)
- Extensions must be version-gated OR namespace-gated
- Dependencies must be satisfied before activation
- Conflicting extensions cannot both activate

**Gating Mechanisms:**

1. **Version Gating**: Extension targets specific script version
2. **Namespace Gating**: Extension targets specific opcode namespace
3. **Activation Height**: Extension activates at specific block height
4. **Dependencies**: Extension requires other extensions first
5. **Conflicts**: Extension declares incompatibilities

**Extension Lifecycle:**
```
PROPOSED → LOCKED_IN → ACTIVATED
    ↓
REJECTED / EXPIRED
```

**Example (Valid Extension):**
```cpp
Extension covenant_v2("covenant_v2", "Covenant extensions v2");
covenant_v2.target_version = ScriptVersion::VERSION_1;      // Version gating
covenant_v2.target_namespace = OpcodeNamespace::EXTENSION_1; // Namespace gating
covenant_v2.activation_height = 500000;                      // Activation height
covenant_v2.depends_on = {};                                 // No dependencies
covenant_v2.conflicts_with = {};                             // No conflicts

// ✅ Valid: Extension is explicitly gated (version + namespace)
```

**Example (Invalid Extension - No Gating):**
```cpp
Extension implicit_ext("bad_ext", "Implicit extension");
// ❌ No target_version
// ❌ No target_namespace
// ❌ REJECTED: EG3 violation - no explicit gating
```

**Example (Invalid Extension - Modifying Ring 7):**
```cpp
Extension ring7_mod("bad_ext", "Ring 7 modification");
ring7_mod.target_version = ScriptVersion::VERSION_0;  // ❌ Cannot modify VERSION_0
// ❌ REJECTED: EG2 violation - VERSION_0 frozen
```

**Enforcement:**
- ExtensionRegistry validates proposals
- Extensions without gating are rejected
- Activation requires dependency satisfaction
- Conflicts prevent simultaneous activation

---

## Framework Components

### 1. Extension Types (`framework/extension_types.h`)

**ScriptVersion enum:**
- VERSION_0 = Ring 7 (frozen)
- VERSION_1+ = Extension versions (gated)

**OpcodeNamespace enum:**
- CORE = Ring 7 opcodes (frozen)
- EXTENSION_1+ = Extension namespaces (gated)

**ExtensionStatus enum:**
- PROPOSED, LOCKED_IN, ACTIVATED, REJECTED, EXPIRED

**Extension struct:**
- Metadata for extension proposals
- Gating parameters (version/namespace)
- Activation parameters (height, dependencies, conflicts)

---

### 2. Extension Registry (`framework/extension_registry.h/.cpp`)

**Purpose**: Track and validate extension proposals

**Key Methods:**
- `registerExtension()` - Register new extension proposal
- `activateExtension()` - Activate extension at height
- `isActive()` - Check if extension active at height
- `validateProposal()` - Validate extension against EG1-EG3
- `checkNoImplicitActivation()` - EG3 property check
- `checkActivationConflicts()` - Conflict detection

**Invariants:**
- No implicit activation (EG3)
- No conflicting extensions simultaneously active
- Dependencies satisfied before activation

---

### 3. Script Version Dispatcher (`framework/script_version_dispatcher.h/.cpp`)

**Purpose**: Route script execution by version with strict isolation

**Key Methods:**
- `executeScript()` - Execute script with version isolation
- `checkVersionIsolation()` - EG2 property check
- `isVersionActive()` - Check if version activated
- `getAvailableOpcodes()` - Get opcodes for version

**Execution Flow:**
```
VERSION_0 scripts → Ring 7 Executor (frozen S1-S25)
VERSION_1+ scripts → Extension Executor (if activated)
```

**Invariants:**
- VERSION_0 uses only CORE namespace opcodes
- VERSION_1+ checked for activation before execution
- No cross-version feature access

---

### 4. Opcode Namespace Manager (`framework/opcode_namespace_manager.h/.cpp`)

**Purpose**: Manage opcode namespaces with strict isolation

**Key Methods:**
- `registerOpcode()` - Register opcode in namespace
- `isOpcodeRegistered()` - Check opcode registration
- `getOpcodeName()` - Resolve opcode in namespace
- `checkNamespaceIsolation()` - EG1 property check
- `checkCoreNamespaceFrozen()` - CORE immutability check

**Invariants:**
- CORE namespace frozen (Ring 7 opcodes)
- Extension namespaces isolated from CORE
- Opcode collisions detected across namespaces

---

## Property Tests (EG1-EG3)

### EG1 Tests (6 tests)
1. `EG1_CoreNamespaceImmutable` - CORE namespace cannot be modified
2. `EG1_ExtensionNamespaceIsolated` - Extension namespaces isolated
3. `EG1_NoOpcodeCollisions` - Opcode collisions detected
4. `EG1_NamespaceIsolationProperty` - Isolation property holds
5. `EG1_CoreOpcodeCount` - CORE has fixed 17 opcodes
6. `EG1_ExtensionOpcodeIndependent` - Extensions don't affect CORE

### EG2 Tests (6 tests)
1. `EG2_Version0UsesCoreOnly` - VERSION_0 uses only CORE opcodes
2. `EG2_Version0CannotUseExtensionOpcodes` - VERSION_0 rejects extension opcodes
3. `EG2_Version1NotActivated` - VERSION_1 not activated yet
4. `EG2_Version0AlwaysActive` - VERSION_0 always active
5. `EG2_VersionIsolationExecution` - VERSION_0 execution works
6. `EG2_ExtensionOpcodeRejectedInVersion0` - Extension opcodes rejected in VERSION_0

### EG3 Tests (6 tests)
1. `EG3_ExtensionRequiresGating` - Extensions require gating
2. `EG3_VersionGatingValid` - Version gating is valid
3. `EG3_NamespaceGatingValid` - Namespace gating is valid
4. `EG3_CannotModifyVersion0` - Cannot gate to VERSION_0
5. `EG3_CannotModifyCoreNamespace` - Cannot gate to CORE namespace
6. `EG3_NoImplicitActivation` - All extensions have explicit gating
7. `EG3_ExtensionRegistration` - Extension registration works
8. `EG3_ExtensionActivationRequiresDependencies` - Dependencies enforced
9. `EG3_ExtensionActivationWithDependencies` - Activation with dependencies
10. `EG3_ConflictingExtensionsCannotActivate` - Conflicts prevent activation

**Total: 18 tests**

---

## What Changes Are Allowed?

### ✅ ALLOWED (Safe Evolution)

1. **New Script Versions** (version-gated)
   - VERSION_1, VERSION_2, ... for new features
   - Each version isolated from VERSION_0 (Ring 7)

2. **New Opcode Namespaces** (namespace-gated)
   - EXTENSION_1, EXTENSION_2, ... for new opcodes
   - Each namespace isolated from CORE (Ring 7)

3. **Extension Activation** (explicitly gated)
   - Extensions activate only at specified heights
   - Dependencies and conflicts enforced

4. **Soft Forks** (gated via extensions)
   - Soft forks packaged as extensions
   - Require explicit version or namespace gating

### 🚫 FORBIDDEN (Violates EG1-EG3)

1. **Modifying VERSION_0** (violates EG2)
   - VERSION_0 = Ring 7 (frozen S1-S25)
   - Cannot add features to VERSION_0

2. **Modifying CORE Namespace** (violates EG1)
   - CORE = Ring 7 opcodes (frozen per BC2)
   - Cannot add/change CORE opcodes

3. **Implicit Activation** (violates EG3)
   - Extensions without version/namespace gating
   - Features that activate without explicit approval

4. **Cross-Version Feature Access** (violates EG2)
   - VERSION_0 scripts using VERSION_1+ features
   - Breaking version isolation

5. **Cross-Namespace Pollution** (violates EG1)
   - Extension opcodes affecting CORE namespace
   - Breaking namespace isolation

---

## Verification Workflow

### For Developers

**Proposing an Extension:**

1. Define extension with explicit gating:
   ```cpp
   Extension my_ext("my_extension", "Description");
   my_ext.target_version = ScriptVersion::VERSION_1;      // Version gate
   my_ext.target_namespace = OpcodeNamespace::EXTENSION_1; // Namespace gate
   my_ext.activation_height = 500000;
   ```

2. Validate proposal:
   ```bash
   # Run EG1-EG3 tests
   ctest -R Ring8_ExtensionGating --verbose
   ```

3. Register extension in registry

4. Activate extension at specified height

**Testing Extensions:**

1. Test namespace isolation (EG1):
   - Verify CORE namespace unchanged
   - Verify extension opcodes isolated

2. Test version isolation (EG2):
   - Verify VERSION_0 scripts still work
   - Verify VERSION_0 rejects extension opcodes

3. Test activation safety (EG3):
   - Verify extension has explicit gating
   - Verify dependencies satisfied
   - Verify no conflicts

---

## Relationship to Other Rings

| Ring | Purpose | EG Protection |
|------|---------|---------------|
| Ring 7 | Script execution semantics | EG2: VERSION_0 frozen |
| Ring 8a | Backward compatibility | EG1: CORE namespace frozen |
| Ring 8b | Extension gating | EG1-EG3: Safe evolution |
| Ring 8c | Change legitimacy | Audit extensions |

Ring 8a **froze** Ring 7.
Ring 8b **enables evolution** while maintaining the freeze.

---

## FAQ

### Q: Can I add new opcodes to DineroCoin?

**A:** Yes, via extension namespaces (EXTENSION_1+). You cannot add to CORE namespace (Ring 7 frozen).

### Q: Can I create new script versions?

**A:** Yes, via VERSION_1+. VERSION_0 is Ring 7 (frozen). New versions must be explicitly gated.

### Q: What happens to VERSION_0 scripts?

**A:** VERSION_0 scripts execute forever with Ring 7 semantics (S1-S25). They cannot access extension features.

### Q: Can extensions modify Ring 7 behavior?

**A:** NO. Ring 7 (VERSION_0, CORE namespace) is frozen per Phase 8a (BC1-BC4). Extensions create NEW versions/namespaces.

### Q: How do soft forks work with gating?

**A:** Soft forks are packaged as extensions with version/namespace gating. They activate only when explicitly approved.

### Q: Can I propose multiple extensions simultaneously?

**A:** Yes, as long as they don't conflict. Extensions can declare dependencies and conflicts.

---

## Summary

Ring 8 Phase 8b enables safe evolution through:

- **EG1**: Namespace Isolation - CORE namespace (Ring 7) frozen, extensions in separate namespaces
- **EG2**: Version Isolation - VERSION_0 (Ring 7) frozen, extensions in VERSION_1+
- **EG3**: Activation Safety - Extensions require explicit gating, no implicit activation

**Framework components:**
- ✅ Extension Registry (proposal validation, activation)
- ✅ Script Version Dispatcher (version isolation)
- ✅ Opcode Namespace Manager (namespace isolation)
- ✅ EG1-EG3 property tests (18 tests)

**Result:** DineroCoin can evolve safely. Ring 7 remains frozen. Extensions are strictly isolated.

---

*"Ring 7 froze meaning. Ring 8a froze the freeze. Ring 8b enables safe evolution."*

🔓 **Phase 8b Status**: ACTIVE
📅 **Sealed Date**: 2026-01-03
🏷️ **Tag**: `ring8-phase8b`
