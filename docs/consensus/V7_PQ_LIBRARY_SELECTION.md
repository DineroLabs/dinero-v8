# Dinero v7 PQ Library Selection

**Status:** Decision doc (Phase 1)
**Scope:** Select the ML-DSA-65 library for v7 consensus, per V7_GENESIS_SPEC.md Phase 1.
**Audit status of Dinero tree:** No PQ library currently vendored. `third_party/openssl-3.3.2` is present but OpenSSL 3.3 does not ship native ML-DSA; that's OpenSSL 3.5+. Starting from zero.

## Decision Criteria

Consensus code imposes hard constraints. Ranked:

1. **Deterministic verify.** Two nodes running the same binary on the same witness bytes MUST return the same verify result, bit-for-bit, across all supported platforms (x86-64 Linux, Apple Silicon, Windows). No platform-specific crypto backends with differing edge-case behavior.
2. **Audit surface.** Fewer lines, isolated per-scheme, easier to read = easier to review. Consensus code is read more often than it's written.
3. **Minimum dependency footprint.** No new build-system dependencies beyond what the lib itself needs (ideally just a C compiler). No OpenSSL / BoringSSL coupling we don't already have.
4. **License compatibility.** Compatible with Dinero's existing license. MIT / BSD / Apache-2 / public domain all fine.
5. **Long-term maintenance.** Lib will be in our consensus for 10+ years. Active upstream, or simple enough that we can fork and own it.
6. **Path to FALCON + SPHINCS+.** When those flip from `DARK_RESERVED` to `ACCEPT`, adding them should be mechanical.

## Candidates

### 1. pq-crystals/ml-dsa

- **URL:** https://github.com/pq-crystals/dilithium (ML-DSA upstream); kyber and dilithium are the two pq-crystals schemes.
- **Origin:** Official reference implementation by the ML-DSA designers.
- **License:** CC0 / public domain for the reference impl.
- **Scope:** ML-DSA only (and Kyber, which we don't need).
- **Build system:** Plain Makefile + C99, single-header-per-scheme structure. ~5,000 LOC for ML-DSA-65 ref.
- **Determinism:** Reference implementation is deterministic by construction. Has AVX2-optimized variants for x86-64, but the ref variant is portable pure-C.
- **Audit status:** Reference implementation. Read by every other impl. Highest conceptual trust.
- **FALCON path:** Would need separate integration (pq-crystals does not publish FALCON; use PQClean or Falcon-team upstream for that).
- **Pros:** Smallest, cleanest, most directly auditable. Canonical ref.
- **Cons:** Integrating FALCON later requires picking a different upstream for FALCON specifically — small duplication of build plumbing.

### 2. PQClean

- **URL:** https://github.com/PQClean/PQClean
- **Origin:** Community project curating clean reference implementations of NIST PQ finalists.
- **License:** Per-scheme; most are CC0 or MIT. ML-DSA-65 is CC0.
- **Scope:** Collection — ML-DSA, FALCON, SPHINCS+, Kyber, etc. One scheme = one subdirectory.
- **Build system:** Per-scheme Makefiles, all uniformly structured. Can be integrated as "vendor only the schemes you need" — we don't have to take the whole tree.
- **Determinism:** Same as pq-crystals (uses the reference implementations). Portable pure-C.
- **Audit status:** Clean reference implementations, community-reviewed. Slightly more machinery than pq-crystals (API normalization layer).
- **Pros:** Single upstream for ML-DSA + FALCON + SPHINCS+. Adding schemes later is drop-in from the same project.
- **Cons:** Small amount of extra shim code around each scheme (normalized API). More LOC to review than pq-crystals ref direct.

### 3. liboqs (Open Quantum Safe)

- **URL:** https://github.com/open-quantum-safe/liboqs
- **Origin:** University of Waterloo / Open Quantum Safe project.
- **License:** MIT.
- **Scope:** Broad — dozens of PQ schemes under a unified C API.
- **Build system:** CMake, with a fair amount of optional-feature discovery. Adds ~50k LOC to the tree if vendored in full (though you can build with only the schemes you need enabled).
- **Determinism:** Inherits the underlying reference impl's determinism. Some build-time choices select between pure-C and optimized backends.
- **Audit status:** Widely used, actively maintained, but wraps many implementations with different provenance.
- **Pros:** If we ever want to support a zoo of PQ schemes, liboqs is the easy answer. Unified API.
- **Cons:** Overkill for v7's one-scheme-at-a-time discipline. CMake configure complexity. Bigger review surface.

## Scoring

| Criterion                     | pq-crystals     | PQClean        | liboqs         |
|-------------------------------|-----------------|----------------|----------------|
| Deterministic verify          | ✅ ref-impl     | ✅ ref-impl    | ✅ inherited   |
| Audit surface (per-scheme LOC)| **~5k**         | ~5.5k          | ~8k (incl. shim) |
| Build footprint               | **Makefile**    | Makefile       | CMake + config |
| License                       | CC0             | CC0/MIT        | MIT            |
| Long-term maintenance         | Upstream is academic & small — may fork | Active community | Active uni-backed |
| Multi-scheme path (FALCON+)   | Separate upstream | **Same upstream** | **Same upstream** |

## Recommendation: PQClean

**Why PQClean over pq-crystals direct:**

- The multi-scheme path matters. When FALCON activates (registry `0x02` → `ACCEPT`), we want to add it by dropping another PQClean subdirectory into `third_party/pqclean/`, not by chasing a separate upstream with its own idioms.
- The ~500 LOC of shim code PQClean adds on top of the ref impls is trivial to audit and buys us uniform API shape across schemes. When SPHINCS+ eventually activates, same pattern.
- PQClean's ML-DSA-65 subdirectory *is* the pq-crystals ref impl, just imported and normalized. We get the pq-crystals audit trust with the PQClean build ergonomics. No loss.

**Why not liboqs:**

- Overkill. liboqs solves "I want to experiment with many PQ schemes." We have the opposite requirement: one scheme, consensus-critical, read more than written.
- CMake configure surface we don't need.
- Bigger audit review surface for identical cryptographic output.

**Why not pq-crystals direct:**

- Fine choice, marginally smaller. But losing the multi-scheme integration path isn't worth the ~500 LOC saved.

## Integration Plan

1. **Vendor PQClean as `third_party/pqclean/`.** Take only the ML-DSA-65 subdirectory initially (`crypto_sign/ml-dsa-65/clean/`). When FALCON activates later, add `crypto_sign/falcon-512/clean/`. Do not vendor schemes we don't use.
2. **Build as a static library** — `third_party/pqclean/CMakeLists.txt` builds `libpqclean_ml_dsa_65.a`. No system package dependency.
3. **Wrap in `src/consensus/pq/ml_dsa_65.{h,cpp}`** — tiny C++ façade (~100 LOC) that exposes `verify(pubkey, sig, msg) -> bool`, `sign(privkey, msg) -> sig`, `keygen() -> (pub, priv)`. Consensus never calls PQClean directly; only via this wrapper.
4. **Registry integration** (per V7_GENESIS_SPEC.md `PQSchemeRegistry`): `scheme_id = 0x01` row dispatches to `ml_dsa_65::verify`. Adding FALCON later means adding `src/consensus/pq/falcon_512.{h,cpp}` and populating row `0x02`. Zero changes to the dispatch loop.

## Commit to Pin

PQClean has release tags. Pin to a specific commit, not a branch. Proposed: tag a specific PQClean release commit in `V7_GENESIS_SPEC.md` once we've run the benchmark and confirmed fit. The pinned commit hash becomes part of the genesis consensus artifact.

## Risks and Flags

- **PQClean is a collection, not a product.** If the project becomes inactive, we fork the subdirectories we use. The ML-DSA-65 ref impl is small and self-contained, so maintaining a fork is cheap.
- **AVX2 / NEON optimized variants exist** in PQClean alongside the clean variants. **Do not use them in consensus.** The clean variant is what goes into verify; the AVX2 variant is optional for sign-side performance only (sign is wallet-side, not consensus).
- **Determinism check at CI:** add a test that signs a fixed message with a fixed key and compares the output byte-for-bit. Failure means the compiler/platform is producing something we can't consensus-verify.

## Decision

**Use PQClean, ML-DSA-65 clean variant only, vendored into `third_party/pqclean/` as a subdirectory copy pinned to a specific upstream commit hash. Sign-side may optionally use optimized variants (wallet only); verify-side uses clean only.**

Phase 2 (benchmark) proceeds on this basis.
