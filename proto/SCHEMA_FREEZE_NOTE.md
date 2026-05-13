# Protocol Buffer Schema Freeze - Utreexo Phase 3

## ⚠️ CONSENSUS-CRITICAL: Wire Format Lockdown

This document records the frozen state of Utreexo-related protobuf schemas for Phase 3 activation.

**Date:** January 9, 2026
**Phase:** Utreexo Phase 3 (Stateless Validation + Reorg Safety)
**Status:** 🔒 **FROZEN** - No modifications allowed without hard fork

---

## Schema Versioning

### Current Schema Version: v3.0.0-utreexo

**Auto-generated files (DO NOT EDIT MANUALLY):**
- `generated/proto/dinerod.pb.h`
- `generated/proto/dinerod.pb.cc`
- `generated/proto/dinerod.grpc.pb.h`
- `generated/proto/dinerod.grpc.pb.cc`

**Source schema:**
- `proto/dinerod.proto` (Utreexo messages added)

---

## Frozen Message Definitions

### Critical: Utreexo Block Data

```protobuf
message BlockUtreexoData {
  // Accumulator root BEFORE applying block (32 bytes)
  bytes accumulator_root_before = 1;

  // Batch proof for all spends in block
  UtreexoBatchProof spend_proof = 2;

  // Spent output metadata (enables stateless validation)
  repeated SpentOutput spent_outputs = 3;
}

message UtreexoBatchProof {
  // Leaf hashes being proven (one per spent UTXO)
  repeated bytes targets = 1;

  // Merkle proof hashes (shared across all targets)
  repeated bytes proof_hashes = 2;
}

message SpentOutput {
  // Output value in una
  uint64 value = 1;

  // ScriptPubKey (locking script)
  bytes scriptPubKey = 2;
}
```

### Field Number Registry (PERMANENT)

**BlockUtreexoData:**
- Field 1: `accumulator_root_before` (bytes, required)
- Field 2: `spend_proof` (UtreexoBatchProof, required)
- Field 3: `spent_outputs` (repeated SpentOutput, required)
- Fields 4-15: **RESERVED** for future Utreexo extensions
- Fields 16+: Available for other block metadata

**UtreexoBatchProof:**
- Field 1: `targets` (repeated bytes, required)
- Field 2: `proof_hashes` (repeated bytes, required)
- Fields 3-15: **RESERVED** for future proof optimizations
- Fields 16+: Available

**SpentOutput:**
- Field 1: `value` (uint64, required)
- Field 2: `scriptPubKey` (bytes, required)
- Fields 3-15: **RESERVED** for future output metadata
- Fields 16+: Available

---

## Schema Compatibility Guarantees

### ✅ SAFE Modifications (Non-Breaking)

1. **Adding NEW optional fields** (fields 16+)
   - Old nodes ignore unknown fields
   - New nodes use defaults for missing fields

2. **Adding NEW message types**
   - Does not affect existing messages

3. **Adding comments/documentation**
   - No wire format impact

### ❌ FORBIDDEN Modifications (Consensus-Breaking)

1. **Changing field numbers** ⚠️ CATASTROPHIC
   - Example: Moving `value` from field 1 to field 3
   - Result: Nodes deserialize wrong data, consensus split

2. **Changing field types** ⚠️ CATASTROPHIC
   - Example: `uint64 value` → `int64 value`
   - Result: Silent data corruption

3. **Changing `optional` ↔ `required`** ⚠️ DANGEROUS
   - Can cause validation failures on old blocks

4. **Removing fields** ⚠️ DANGEROUS
   - Old blocks may contain removed fields
   - Deserialization may fail or corrupt data

5. **Renumbering reserved fields** ⚠️ DANGEROUS
   - Breaks future upgrade paths

---

## Verification Checklist (Before Any Proto Change)

Before modifying `proto/dinerod.proto`, verify:

- [ ] No field number changes in existing messages
- [ ] No field type changes in existing messages
- [ ] No `required` ↔ `optional` changes
- [ ] No field removals (use deprecation instead)
- [ ] Reserved field ranges unchanged
- [ ] New fields start at field 16+ (not in reserved range)
- [ ] Wire compatibility test passes (compare serialized output)
- [ ] Consensus test suite passes with old + new proto

---

## Wire Format Compatibility Test

To verify proto changes don't break wire format:

```bash
# Generate test data with OLD proto
./old_dinerod --dump-utreexo-testdata > old_format.bin

# Rebuild with NEW proto
make clean && make

# Verify NEW code can deserialize OLD data
./new_dinerod --verify-utreexo-testdata old_format.bin

# Expected output: "✅ Wire format compatible"
```

---

## Proto Change Log

### v3.0.0-utreexo (January 9, 2026) - CURRENT
- Added `BlockUtreexoData` message
- Added `UtreexoBatchProof` message
- Added `SpentOutput` message
- Reserved fields 4-15 in each message
- **Status:** 🔒 FROZEN

### Future Versions
- v3.1.0: TBD (must be backward compatible with v3.0.0)
- v4.0.0: Hard fork allowed (if absolutely necessary)

---

## Enforcement

**Automated checks:**
1. CI must run proto diff on every PR
2. Any field number change = auto-reject
3. Any type change = requires manual review + hard fork approval

**Manual review required for:**
- Any proto file modification
- Adding fields to frozen messages
- Touching reserved field ranges

---

## Schema Hash (Checksum)

**Current schema SHA256:**
```
TBD - Run: sha256sum proto/dinerod.proto
```

This hash must be updated ONLY when proto is intentionally modified with proper review.

---

## Contact

For proto schema questions or hard fork proposals:
- GitHub Issues: Tag with `consensus-critical` + `proto-schema`
- Security: Treat wire format changes as security-critical

---

**Last Updated:** January 9, 2026
**Frozen By:** Utreexo Phase 3 Activation
**Next Review:** Before Phase 4 (delta-based undo implementation)
