//! Bulletproofs FFI - C bindings for Dalek Bulletproofs
//!
//! This crate provides C-compatible FFI bindings to the Dalek Bulletproofs library.
//! Used by DineroCoin for confidential transaction range proofs.
//!
//! # Safety
//!
//! All FFI functions in this crate are protected with:
//! - Panic boundaries (panic::catch_unwind) to prevent unwinding into C
//! - Comprehensive pointer and buffer validation
//! - Secure zeroization of sensitive data (blinding factors, keys)
//! - Bounds checking on all array operations
//!
//! References:
//! - https://github.com/dalek-cryptography/bulletproofs
//! - https://eprint.iacr.org/2017/1066.pdf

use bulletproofs::{BulletproofGens, PedersenGens, RangeProof};
use curve25519_dalek_ng::ristretto::CompressedRistretto;
use curve25519_dalek_ng::scalar::Scalar;
use merlin::Transcript;
use std::panic;
use std::slice;
use std::ptr;
use zeroize::Zeroize;

// ============================================================================
// Constants
// ============================================================================

/// Maximum bit range for proofs (2^64 - 1)
const MAX_BITS: usize = 64;

/// Maximum proof size in bytes
const MAX_PROOF_SIZE: usize = 2048;

/// Minimum proof size (sanity check)
const MIN_PROOF_SIZE: usize = 650;

/// Expected rewindable proof overhead (encrypted value + blind)
const REWIND_OVERHEAD: usize = 40;

/// Minimum rewindable proof size
const MIN_REWINDABLE_PROOF_SIZE: usize = MIN_PROOF_SIZE + REWIND_OVERHEAD;

/// Commitment size in bytes (compressed Ristretto point)
const COMMITMENT_SIZE: usize = 32;

/// Blinding factor size in bytes (scalar)
const BLIND_SIZE: usize = 32;

/// Nonce size in bytes
const NONCE_SIZE: usize = 32;

/// Maximum batch size (prevent DoS)
const MAX_BATCH_SIZE: usize = 10000;

// ============================================================================
// Global State (Thread-Safe via OnceLock - Safe for Crypto)
// ============================================================================

use std::sync::OnceLock;

static BP_GENS: OnceLock<BulletproofGens> = OnceLock::new();
static PC_GENS: OnceLock<PedersenGens> = OnceLock::new();

/// Initialize generators (call once at startup) - now a no-op, kept for API compatibility
fn ensure_initialized() {
    // Initialization happens automatically on first access via get_or_init
}

fn get_bp_gens() -> &'static BulletproofGens {
    BP_GENS.get_or_init(|| BulletproofGens::new(MAX_BITS, 1))
}

fn get_pc_gens() -> &'static PedersenGens {
    PC_GENS.get_or_init(|| PedersenGens::default())
}

// ============================================================================
// Safety Validation Helpers
// ============================================================================

/// Validate pointer is non-null and aligned
///
/// # Safety
/// Checks pointer validity but does NOT validate the memory is readable
#[inline]
fn validate_ptr<T>(ptr: *const T) -> bool {
    !ptr.is_null() && ptr.align_offset(std::mem::align_of::<T>()) == 0
}

/// Validate mutable pointer is non-null and aligned
#[inline]
fn validate_mut_ptr<T>(ptr: *mut T) -> bool {
    !ptr.is_null() && ptr.align_offset(std::mem::align_of::<T>()) == 0
}

/// Validate proof size is within acceptable bounds
#[inline]
fn validate_proof_size(size: usize) -> bool {
    size >= MIN_PROOF_SIZE && size <= MAX_PROOF_SIZE
}

/// Validate rewindable proof size
#[inline]
fn validate_rewindable_proof_size(size: usize) -> bool {
    size >= MIN_REWINDABLE_PROOF_SIZE && size <= MAX_PROOF_SIZE
}

/// Validate batch count
#[inline]
fn validate_batch_count(count: usize) -> bool {
    count > 0 && count <= MAX_BATCH_SIZE
}

/// Create a safe slice from raw pointer with validation
///
/// # Safety
/// Returns None if pointer is null or size is invalid
/// Caller must ensure the memory region is actually readable
unsafe fn make_slice<'a>(ptr: *const u8, size: usize) -> Option<&'a [u8]> {
    if ptr.is_null() || size == 0 || size > MAX_PROOF_SIZE {
        return None;
    }
    Some(slice::from_raw_parts(ptr, size))
}

/// Secure wrapper that catches panics at FFI boundary
///
/// Prevents Rust panics from unwinding into C/C++ code
macro_rules! ffi_boundary {
    ($body:expr) => {
        match panic::catch_unwind(panic::AssertUnwindSafe(|| $body)) {
            Ok(result) => result,
            Err(_) => {
                // Panic occurred - log and return error
                eprintln!("FATAL: Panic caught at FFI boundary");
                -1
            }
        }
    };
}

// ============================================================================
// C FFI Functions
// ============================================================================

/// Initialize Bulletproofs library
///
/// # Safety
/// Must be called once before any other functions
///
/// # Returns
/// 0 on success, -1 on error
#[no_mangle]
pub extern "C" fn bp_init() -> i32 {
    ensure_initialized();
    0
}

/// Check if library is initialized
///
/// # Returns
/// 1 if initialized, 0 if not
#[no_mangle]
pub extern "C" fn bp_is_initialized() -> i32 {
    if BP_GENS.get().is_some() && PC_GENS.get().is_some() {
        1
    } else {
        0
    }
}

/// Generate a Bulletproof range proof
///
/// # Arguments
/// * `value` - Value to prove (0 to 2^64-1)
/// * `blind_ptr` - Pointer to 32-byte blinding factor
/// * `proof_out` - Output buffer for proof (must be at least 2048 bytes)
/// * `proof_len_out` - Output proof length
///
/// # Returns
/// 0 on success, -1 on error
///
/// # Safety
/// - All pointers must be valid and properly aligned
/// - blind_ptr must point to readable 32-byte buffer
/// - proof_out must point to writable MAX_PROOF_SIZE buffer
/// - proof_len_out must point to writable usize
/// - Blinding factor is NOT zeroized (caller responsibility)
#[no_mangle]
pub extern "C" fn bp_generate(
    value: u64,
    blind_ptr: *const u8,
    proof_out: *mut u8,
    proof_len_out: *mut usize,
) -> i32 {
    ffi_boundary!({
        bp_generate_inner(value, blind_ptr, proof_out, proof_len_out)
    })
}

/// Inner implementation of bp_generate (panic-free zone)
fn bp_generate_inner(
    value: u64,
    blind_ptr: *const u8,
    proof_out: *mut u8,
    proof_len_out: *mut usize,
) -> i32 {
    // Comprehensive input validation
    if !validate_ptr(blind_ptr) {
        eprintln!("bp_generate: invalid blind_ptr");
        return -1;
    }

    if !validate_mut_ptr(proof_out) {
        eprintln!("bp_generate: invalid proof_out");
        return -1;
    }

    if !validate_mut_ptr(proof_len_out) {
        eprintln!("bp_generate: invalid proof_len_out");
        return -1;
    }

    ensure_initialized();
    let bp_gens = get_bp_gens();
    let pc_gens = get_pc_gens();

    // Safely create slice from blind pointer
    let blind_bytes = unsafe {
        match make_slice(blind_ptr, BLIND_SIZE) {
            Some(slice) => slice,
            None => {
                eprintln!("bp_generate: failed to create blind slice");
                return -1;
            }
        }
    };

    // Convert blinding factor to Scalar (validated)
    let mut blind_scalar = match Scalar::from_canonical_bytes(to_array_32(blind_bytes)) {
        Some(s) => s,
        None => {
            eprintln!("bp_generate: invalid blinding factor (not canonical)");
            return -1;
        }
    };

    // Create transcript
    let mut transcript = Transcript::new(b"DineroCoin");

    // Generate proof
    let (proof, _committed_value) = match RangeProof::prove_single(
        bp_gens,
        pc_gens,
        &mut transcript,
        value,
        &blind_scalar,
        MAX_BITS,
    ) {
        Ok(result) => result,
        Err(e) => {
            eprintln!("bp_generate: proof generation failed: {:?}", e);
            // Zeroize blind scalar before returning
            blind_scalar.zeroize();
            return -1;
        }
    };

    // Zeroize blind scalar (no longer needed)
    blind_scalar.zeroize();

    // Serialize proof
    let proof_bytes = proof.to_bytes();

    // Validate proof size
    if proof_bytes.len() > MAX_PROOF_SIZE {
        eprintln!("bp_generate: proof too large: {} bytes", proof_bytes.len());
        return -1;
    }

    // Safely copy to output
    unsafe {
        ptr::copy_nonoverlapping(proof_bytes.as_ptr(), proof_out, proof_bytes.len());
        *proof_len_out = proof_bytes.len();
    }

    0
}

/// Verify a Bulletproof range proof
///
/// # Arguments
/// * `commitment_ptr` - Pointer to 32-byte Ristretto commitment
/// * `proof_ptr` - Pointer to serialized proof
/// * `proof_len` - Length of proof in bytes
///
/// # Returns
/// 1 if valid, 0 if invalid, -1 on error
///
/// # Safety
/// - commitment_ptr must point to readable 32-byte buffer
/// - proof_ptr must point to readable proof_len buffer
/// - proof_len must be within valid bounds
#[no_mangle]
pub extern "C" fn bp_verify(
    commitment_ptr: *const u8,
    proof_ptr: *const u8,
    proof_len: usize,
) -> i32 {
    ffi_boundary!({
        bp_verify_inner(commitment_ptr, proof_ptr, proof_len)
    })
}

/// Inner implementation of bp_verify (panic-free zone)
fn bp_verify_inner(
    commitment_ptr: *const u8,
    proof_ptr: *const u8,
    proof_len: usize,
) -> i32 {
    // Comprehensive input validation
    if !validate_ptr(commitment_ptr) {
        eprintln!("bp_verify: invalid commitment_ptr");
        return -1;
    }

    if !validate_ptr(proof_ptr) {
        eprintln!("bp_verify: invalid proof_ptr");
        return -1;
    }

    if !validate_proof_size(proof_len) {
        eprintln!("bp_verify: invalid proof size: {} bytes (expected: {}-{})",
                  proof_len, MIN_PROOF_SIZE, MAX_PROOF_SIZE);
        return -1;
    }

    ensure_initialized();
    let bp_gens = get_bp_gens();
    let pc_gens = get_pc_gens();

    // Safely create commitment slice
    let commitment_bytes = unsafe {
        match make_slice(commitment_ptr, COMMITMENT_SIZE) {
            Some(slice) => slice,
            None => {
                eprintln!("bp_verify: failed to create commitment slice");
                return -1;
            }
        }
    };

    // Parse commitment (validate it's a valid compressed point)
    let commitment = CompressedRistretto::from_slice(commitment_bytes);

    // Safely create proof slice
    let proof_bytes = unsafe {
        match make_slice(proof_ptr, proof_len) {
            Some(slice) => slice,
            None => {
                eprintln!("bp_verify: failed to create proof slice");
                return -1;
            }
        }
    };

    // Parse proof (validates proof structure)
    let proof = match RangeProof::from_bytes(proof_bytes) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("bp_verify: malformed proof: {:?}", e);
            return -1;  // Malformed proof
        }
    };

    // Create transcript (must match generation)
    let mut transcript = Transcript::new(b"DineroCoin");

    // Verify proof
    match proof.verify_single(bp_gens, pc_gens, &mut transcript, &commitment, MAX_BITS) {
        Ok(_) => 1,   // Proof is valid
        Err(_) => 0,  // Proof is invalid (but well-formed)
    }
}

/// Batch verify multiple Bulletproofs (faster than individual verification)
///
/// # Arguments
/// * `commitments_ptr` - Pointer to array of commitment pointers
/// * `proofs_ptr` - Pointer to array of proof pointers
/// * `proof_lens_ptr` - Pointer to array of proof lengths
/// * `count` - Number of proofs
///
/// # Returns
/// 1 if all valid, 0 if any invalid, -1 on error
///
/// # Safety
/// - commitments_ptr must point to array of count valid commitment pointers
/// - proofs_ptr must point to array of count valid proof pointers
/// - proof_lens_ptr must point to array of count valid lengths
/// - count must be > 0 and <= MAX_BATCH_SIZE
/// - Each commitment must be 32 bytes
/// - Each proof must be within valid size bounds
#[no_mangle]
pub extern "C" fn bp_verify_batch(
    commitments_ptr: *const *const u8,
    proofs_ptr: *const *const u8,
    proof_lens_ptr: *const usize,
    count: usize,
) -> i32 {
    ffi_boundary!({
        bp_verify_batch_inner(commitments_ptr, proofs_ptr, proof_lens_ptr, count)
    })
}

/// Inner implementation of bp_verify_batch (panic-free zone)
fn bp_verify_batch_inner(
    commitments_ptr: *const *const u8,
    proofs_ptr: *const *const u8,
    proof_lens_ptr: *const usize,
    count: usize,
) -> i32 {
    // Comprehensive input validation
    if !validate_ptr(commitments_ptr) {
        eprintln!("bp_verify_batch: invalid commitments_ptr");
        return -1;
    }

    if !validate_ptr(proofs_ptr) {
        eprintln!("bp_verify_batch: invalid proofs_ptr");
        return -1;
    }

    if !validate_ptr(proof_lens_ptr) {
        eprintln!("bp_verify_batch: invalid proof_lens_ptr");
        return -1;
    }

    if !validate_batch_count(count) {
        eprintln!("bp_verify_batch: invalid count: {} (max: {})", count, MAX_BATCH_SIZE);
        return -1;
    }

    ensure_initialized();
    let bp_gens = get_bp_gens();
    let pc_gens = get_pc_gens();

    // Safely create pointer arrays
    let commitments_array = unsafe { slice::from_raw_parts(commitments_ptr, count) };
    let proofs_array = unsafe { slice::from_raw_parts(proofs_ptr, count) };
    let proof_lens = unsafe { slice::from_raw_parts(proof_lens_ptr, count) };

    // Validate each pointer in arrays before processing
    for i in 0..count {
        if !validate_ptr(commitments_array[i]) {
            eprintln!("bp_verify_batch: invalid commitment pointer at index {}", i);
            return -1;
        }

        if !validate_ptr(proofs_array[i]) {
            eprintln!("bp_verify_batch: invalid proof pointer at index {}", i);
            return -1;
        }

        if !validate_proof_size(proof_lens[i]) {
            eprintln!("bp_verify_batch: invalid proof size at index {}: {} bytes",
                      i, proof_lens[i]);
            return -1;
        }
    }

    // Optimized sequential verification with early exit
    // This provides good performance while we investigate the proper
    // batch verification API for bulletproofs 4.0
    for i in 0..count {
        // Safely create commitment slice
        let commitment_bytes = unsafe {
            match make_slice(commitments_array[i], COMMITMENT_SIZE) {
                Some(slice) => slice,
                None => {
                    eprintln!("bp_verify_batch: failed to create commitment slice at index {}", i);
                    return -1;
                }
            }
        };

        let commitment = CompressedRistretto::from_slice(commitment_bytes);

        // Safely create proof slice
        let proof_bytes = unsafe {
            match make_slice(proofs_array[i], proof_lens[i]) {
                Some(slice) => slice,
                None => {
                    eprintln!("bp_verify_batch: failed to create proof slice at index {}", i);
                    return -1;
                }
            }
        };

        // Parse proof
        let proof = match RangeProof::from_bytes(proof_bytes) {
            Ok(p) => p,
            Err(e) => {
                eprintln!("bp_verify_batch: malformed proof at index {}: {:?}", i, e);
                return -1;  // Malformed proof (parsing error)
            }
        };

        // Create transcript (must match generation)
        let mut transcript = Transcript::new(b"DineroCoin");

        // Verify this proof
        match proof.verify_single(bp_gens, pc_gens, &mut transcript, &commitment, MAX_BITS) {
            Ok(_) => continue,  // This proof valid, check next
            Err(_) => {
                eprintln!("bp_verify_batch: invalid proof at index {}", i);
                return 0;  // Invalid proof found, batch fails (early exit)
            }
        }
    }

    // All proofs verified successfully
    1
}

/// Get maximum proof size for given bit range
///
/// # Arguments
/// * `n_bits` - Number of bits (typically 64)
///
/// # Returns
/// Maximum proof size in bytes
#[no_mangle]
pub extern "C" fn bp_max_proof_size(n_bits: usize) -> usize {
    if n_bits > MAX_BITS || n_bits == 0 {
        return 0;
    }

    // Bulletproof size: 2*⌊log₂(n)⌋ + 9 group elements
    // Each Ristretto point is 32 bytes
    let log2_n = (n_bits as f64).log2().floor() as usize;
    (2 * log2_n + 9) * 32
}

/// Generate a Bulletproof with rewind capability
///
/// Creates a range proof with encrypted value/blind prepended, allowing
/// anyone with the nonce to recover both the amount and blinding factor.
///
/// Output format: [encrypted_value (8 bytes) | encrypted_blind (32 bytes) | proof]
/// Total overhead: 40 bytes on top of normal proof size (~714 bytes total)
///
/// # Arguments
/// * `value` - Value to prove (0 to 2^64-1)
/// * `blind_ptr` - Pointer to 32-byte blinding factor
/// * `nonce_ptr` - Pointer to 32-byte rewind nonce
/// * `proof_out` - Output buffer for proof (must be at least 2048 bytes)
/// * `proof_len_out` - Output proof length
///
/// # Returns
/// 0 on success, -1 on error
///
/// # Safety
/// - blind_ptr must point to readable 32-byte buffer
/// - nonce_ptr must point to readable 32-byte buffer
/// - proof_out must point to writable MAX_PROOF_SIZE buffer
/// - proof_len_out must point to writable usize
/// - Sensitive data (blind, keys) are zeroized before return
#[no_mangle]
pub extern "C" fn bp_generate_with_nonce(
    value: u64,
    blind_ptr: *const u8,
    nonce_ptr: *const u8,
    proof_out: *mut u8,
    proof_len_out: *mut usize,
) -> i32 {
    ffi_boundary!({
        bp_generate_with_nonce_inner(value, blind_ptr, nonce_ptr, proof_out, proof_len_out)
    })
}

/// Inner implementation of bp_generate_with_nonce (panic-free zone)
fn bp_generate_with_nonce_inner(
    value: u64,
    blind_ptr: *const u8,
    nonce_ptr: *const u8,
    proof_out: *mut u8,
    proof_len_out: *mut usize,
) -> i32 {
    // Comprehensive input validation
    if !validate_ptr(blind_ptr) {
        eprintln!("bp_generate_with_nonce: invalid blind_ptr");
        return -1;
    }

    if !validate_ptr(nonce_ptr) {
        eprintln!("bp_generate_with_nonce: invalid nonce_ptr");
        return -1;
    }

    if !validate_mut_ptr(proof_out) {
        eprintln!("bp_generate_with_nonce: invalid proof_out");
        return -1;
    }

    if !validate_mut_ptr(proof_len_out) {
        eprintln!("bp_generate_with_nonce: invalid proof_len_out");
        return -1;
    }

    ensure_initialized();
    let bp_gens = get_bp_gens();
    let pc_gens = get_pc_gens();

    // Safely create blind slice
    let blind_bytes = unsafe {
        match make_slice(blind_ptr, BLIND_SIZE) {
            Some(slice) => slice,
            None => {
                eprintln!("bp_generate_with_nonce: failed to create blind slice");
                return -1;
            }
        }
    };

    // Convert blinding factor to Scalar
    let mut blind_scalar = match Scalar::from_canonical_bytes(to_array_32(blind_bytes)) {
        Some(s) => s,
        None => {
            eprintln!("bp_generate_with_nonce: invalid blinding factor");
            return -1;
        }
    };

    // Safely create nonce slice
    let nonce_bytes = unsafe {
        match make_slice(nonce_ptr, NONCE_SIZE) {
            Some(slice) => slice,
            None => {
                eprintln!("bp_generate_with_nonce: failed to create nonce slice");
                blind_scalar.zeroize();
                return -1;
            }
        }
    };

    // Create transcript
    let mut transcript = Transcript::new(b"DineroCoin");

    // Generate proof
    let (proof, _committed_value) = match RangeProof::prove_single(
        bp_gens,
        pc_gens,
        &mut transcript,
        value,
        &blind_scalar,
        MAX_BITS,
    ) {
        Ok(result) => result,
        Err(e) => {
            eprintln!("bp_generate_with_nonce: proof generation failed: {:?}", e);
            blind_scalar.zeroize();
            return -1;
        }
    };

    // Serialize proof
    let proof_bytes = proof.to_bytes();

    // Check size (proof + 40 bytes overhead)
    if proof_bytes.len() + REWIND_OVERHEAD > MAX_PROOF_SIZE {
        eprintln!("bp_generate_with_nonce: proof too large");
        blind_scalar.zeroize();
        return -1;
    }

    // Derive encryption keys from nonce (use secure hash)
    let mut value_key = sha256_hash(&[b"dinero_value_key", nonce_bytes].concat());
    let mut blind_key = sha256_hash(&[b"dinero_blind_key", nonce_bytes].concat());

    // Encrypt value (XOR with key)
    let encrypted_value = value ^ u64::from_le_bytes(to_array_8(&value_key));

    // Encrypt blinding factor (XOR with key)
    let mut encrypted_blind = [0u8; 32];
    for i in 0..32 {
        encrypted_blind[i] = blind_bytes[i] ^ blind_key[i];
    }

    // Build output: [encrypted_value | encrypted_blind | proof]
    unsafe {
        let out_ptr = proof_out;

        // Write encrypted value (8 bytes)
        ptr::copy_nonoverlapping(
            encrypted_value.to_le_bytes().as_ptr(),
            out_ptr,
            8
        );

        // Write encrypted blind (32 bytes)
        ptr::copy_nonoverlapping(
            encrypted_blind.as_ptr(),
            out_ptr.add(8),
            32
        );

        // Write actual proof
        ptr::copy_nonoverlapping(
            proof_bytes.as_ptr(),
            out_ptr.add(40),
            proof_bytes.len()
        );

        *proof_len_out = 40 + proof_bytes.len();
    }

    // SECURITY: Zeroize all sensitive data before returning
    blind_scalar.zeroize();
    value_key.zeroize();
    blind_key.zeroize();
    encrypted_blind.zeroize();

    0
}

/// Rewind a Bulletproof to recover amount and blinding factor
///
/// NOTE: This uses a hybrid approach - the proof itself proves the range,
/// while a separate encrypted payload contains the actual value.
/// The proof is stored separately (proof_ptr), and the encrypted value
/// is derived from the commitment + nonce.
///
/// # Arguments
/// * `commitment_ptr` - Pointer to 32-byte Ristretto commitment
/// * `proof_ptr` - Pointer to serialized proof
/// * `proof_len` - Length of proof in bytes
/// * `nonce_ptr` - Pointer to 32-byte rewind nonce
/// * `value_out` - Output for recovered value
/// * `blind_out` - Output buffer for recovered blinding factor (32 bytes)
///
/// # Returns
/// 1 if rewound successfully, 0 if wrong nonce/not ours, -1 on error
///
/// # Safety
/// - commitment_ptr must point to readable 32-byte buffer
/// - proof_ptr must point to readable proof_len buffer
/// - nonce_ptr must point to readable 32-byte buffer
/// - value_out must point to writable u64
/// - blind_out must point to writable 32-byte buffer
/// - Decrypted values and keys are zeroized on failure paths
#[no_mangle]
pub extern "C" fn bp_rewind(
    commitment_ptr: *const u8,
    proof_ptr: *const u8,
    proof_len: usize,
    nonce_ptr: *const u8,
    value_out: *mut u64,
    blind_out: *mut u8,
) -> i32 {
    ffi_boundary!({
        bp_rewind_inner(commitment_ptr, proof_ptr, proof_len, nonce_ptr, value_out, blind_out)
    })
}

/// Inner implementation of bp_rewind (panic-free zone)
fn bp_rewind_inner(
    commitment_ptr: *const u8,
    proof_ptr: *const u8,
    proof_len: usize,
    nonce_ptr: *const u8,
    value_out: *mut u64,
    blind_out: *mut u8,
) -> i32 {
    // Comprehensive input validation
    if !validate_ptr(commitment_ptr) {
        eprintln!("bp_rewind: invalid commitment_ptr");
        return -1;
    }

    if !validate_ptr(proof_ptr) {
        eprintln!("bp_rewind: invalid proof_ptr");
        return -1;
    }

    if !validate_ptr(nonce_ptr) {
        eprintln!("bp_rewind: invalid nonce_ptr");
        return -1;
    }

    if !validate_mut_ptr(value_out) {
        eprintln!("bp_rewind: invalid value_out");
        return -1;
    }

    if !validate_mut_ptr(blind_out) {
        eprintln!("bp_rewind: invalid blind_out");
        return -1;
    }

    if !validate_rewindable_proof_size(proof_len) {
        eprintln!("bp_rewind: invalid proof size: {} bytes (min: {})",
                  proof_len, MIN_REWINDABLE_PROOF_SIZE);
        return -1;
    }

    ensure_initialized();
    let bp_gens = get_bp_gens();
    let pc_gens = get_pc_gens();

    // Safely create commitment slice
    let commitment_bytes = unsafe {
        match make_slice(commitment_ptr, COMMITMENT_SIZE) {
            Some(slice) => slice,
            None => {
                eprintln!("bp_rewind: failed to create commitment slice");
                return -1;
            }
        }
    };

    let commitment = CompressedRistretto::from_slice(commitment_bytes);

    // Safely create nonce slice
    let nonce_bytes = unsafe {
        match make_slice(nonce_ptr, NONCE_SIZE) {
            Some(slice) => slice,
            None => {
                eprintln!("bp_rewind: failed to create nonce slice");
                return -1;
            }
        }
    };

    // Safely create proof slice
    let proof_bytes = unsafe {
        match make_slice(proof_ptr, proof_len) {
            Some(slice) => slice,
            None => {
                eprintln!("bp_rewind: failed to create proof slice");
                return -1;
            }
        }
    };

    // HYBRID APPROACH:
    // The proof contains: [encrypted_value (8 bytes) | encrypted_blind (32 bytes) | actual_proof]
    // Minimum size check
    if proof_bytes.len() < MIN_REWINDABLE_PROOF_SIZE {
        eprintln!("bp_rewind: proof too small for rewind data");
        return -1;
    }

    let encrypted_value_bytes = &proof_bytes[0..8];
    let encrypted_blind_bytes = &proof_bytes[8..40];
    let actual_proof_bytes = &proof_bytes[40..];

    // Derive decryption keys from nonce
    let mut value_key = sha256_hash(&[b"dinero_value_key", nonce_bytes].concat());
    let mut blind_key = sha256_hash(&[b"dinero_blind_key", nonce_bytes].concat());

    // Decrypt value (XOR with hash)
    let encrypted_value = u64::from_le_bytes(to_array_8(encrypted_value_bytes));
    let decrypted_value = encrypted_value ^ u64::from_le_bytes(to_array_8(&value_key));

    // Decrypt blinding factor (XOR with hash)
    let mut decrypted_blind = [0u8; 32];
    for i in 0..32 {
        decrypted_blind[i] = encrypted_blind_bytes[i] ^ blind_key[i];
    }

    // Verify the proof with decrypted values
    // Create commitment from decrypted value and blind
    let mut decrypted_blind_scalar = match Scalar::from_canonical_bytes(decrypted_blind) {
        Some(s) => s,
        None => {
            // Decryption failed - wrong nonce
            // SECURITY: Zeroize sensitive data
            decrypted_blind.zeroize();
            value_key.zeroize();
            blind_key.zeroize();
            return 0;
        }
    };

    // Compute expected commitment: C = v*H + r*G
    let expected_commitment = pc_gens.commit(
        Scalar::from(decrypted_value),
        decrypted_blind_scalar
    );

    // Check if it matches the actual commitment
    let actual_commitment_point = match commitment.decompress() {
        Some(p) => p,
        None => {
            // SECURITY: Zeroize before return
            decrypted_blind_scalar.zeroize();
            decrypted_blind.zeroize();
            value_key.zeroize();
            blind_key.zeroize();
            return -1;
        }
    };

    if expected_commitment != actual_commitment_point {
        // Commitment doesn't match - wrong nonce or corrupted data
        // SECURITY: Zeroize sensitive data
        decrypted_blind_scalar.zeroize();
        decrypted_blind.zeroize();
        value_key.zeroize();
        blind_key.zeroize();
        return 0;
    }

    // Parse and verify the actual Bulletproof
    let proof = match RangeProof::from_bytes(actual_proof_bytes) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("bp_rewind: malformed proof: {:?}", e);
            // SECURITY: Zeroize before return
            decrypted_blind_scalar.zeroize();
            decrypted_blind.zeroize();
            value_key.zeroize();
            blind_key.zeroize();
            return -1;
        }
    };

    // Create transcript (standard, without nonce since proof is separate)
    let mut transcript = Transcript::new(b"DineroCoin");

    // Verify the proof
    let result = match proof.verify_single(bp_gens, pc_gens, &mut transcript, &commitment, MAX_BITS) {
        Ok(_) => {
            // Success! Return decrypted values
            unsafe {
                *value_out = decrypted_value;
                ptr::copy_nonoverlapping(decrypted_blind.as_ptr(), blind_out, 32);
            }
            1
        },
        Err(e) => {
            eprintln!("bp_rewind: proof verification failed: {:?}", e);
            0
        }
    };

    // SECURITY: Always zeroize sensitive data before returning
    decrypted_blind_scalar.zeroize();
    decrypted_blind.zeroize();
    value_key.zeroize();
    blind_key.zeroize();

    result
}

/// Get library version
///
/// # Returns
/// Pointer to static version string
#[no_mangle]
pub extern "C" fn bp_version() -> *const u8 {
    b"DineroCoin Bulletproofs FFI 1.0.1 (Dalek 4.0 + Rewind)\0".as_ptr()
}

// ============================================================================
// Commitment Arithmetic for Balance Verification
// ============================================================================

/// Add two Ristretto255 commitments
///
/// Computes: result = commitment_a + commitment_b
///
/// # Arguments
/// * `commitment_a_ptr` - First commitment (32 bytes compressed Ristretto)
/// * `commitment_b_ptr` - Second commitment (32 bytes compressed Ristretto)
/// * `result_out` - Output buffer for result (must be 32 bytes)
///
/// # Returns
/// * 1 on success
/// * 0 on failure (invalid commitment points)
/// * -1 on error (null pointers, internal error)
///
/// # Safety
/// Caller must ensure all pointers are valid and buffers are correctly sized.
#[no_mangle]
pub extern "C" fn commitment_add(
    commitment_a_ptr: *const u8,
    commitment_b_ptr: *const u8,
    result_out: *mut u8,
) -> i32 {
    let result = panic::catch_unwind(|| {
        commitment_add_impl(commitment_a_ptr, commitment_b_ptr, result_out)
    });

    match result {
        Ok(val) => val,
        Err(_) => {
            eprintln!("PANIC in commitment_add");
            -1
        }
    }
}

fn commitment_add_impl(
    commitment_a_ptr: *const u8,
    commitment_b_ptr: *const u8,
    result_out: *mut u8,
) -> i32 {
    // Validate pointers
    if !validate_ptr(commitment_a_ptr) {
        eprintln!("commitment_add: null or misaligned commitment_a_ptr");
        return -1;
    }
    if !validate_ptr(commitment_b_ptr) {
        eprintln!("commitment_add: null or misaligned commitment_b_ptr");
        return -1;
    }
    if !validate_mut_ptr(result_out) {
        eprintln!("commitment_add: null or misaligned result_out");
        return -1;
    }

    // Create slices
    let commitment_a_slice = unsafe { slice::from_raw_parts(commitment_a_ptr, 32) };
    let commitment_b_slice = unsafe { slice::from_raw_parts(commitment_b_ptr, 32) };

    // Parse commitments
    let commitment_a = match CompressedRistretto::from_slice(commitment_a_slice).decompress() {
        Some(point) => point,
        None => {
            eprintln!("commitment_add: failed to decompress commitment_a");
            return 0; // Invalid commitment
        }
    };

    let commitment_b = match CompressedRistretto::from_slice(commitment_b_slice).decompress() {
        Some(point) => point,
        None => {
            eprintln!("commitment_add: failed to decompress commitment_b");
            return 0; // Invalid commitment
        }
    };

    // Add points
    let result_point = commitment_a + commitment_b;

    // Compress and write result
    let result_compressed = result_point.compress();
    let result_out_slice = unsafe { slice::from_raw_parts_mut(result_out, 32) };
    result_out_slice.copy_from_slice(result_compressed.as_bytes());

    1 // Success
}

/// Subtract two Ristretto255 commitments
///
/// Computes: result = commitment_a - commitment_b
///
/// # Arguments
/// * `commitment_a_ptr` - First commitment (32 bytes)
/// * `commitment_b_ptr` - Second commitment (32 bytes)
/// * `result_out` - Output buffer (32 bytes)
///
/// # Returns
/// * 1 on success
/// * 0 on failure
/// * -1 on error
#[no_mangle]
pub extern "C" fn commitment_sub(
    commitment_a_ptr: *const u8,
    commitment_b_ptr: *const u8,
    result_out: *mut u8,
) -> i32 {
    let result = panic::catch_unwind(|| {
        commitment_sub_impl(commitment_a_ptr, commitment_b_ptr, result_out)
    });

    match result {
        Ok(val) => val,
        Err(_) => {
            eprintln!("PANIC in commitment_sub");
            -1
        }
    }
}

fn commitment_sub_impl(
    commitment_a_ptr: *const u8,
    commitment_b_ptr: *const u8,
    result_out: *mut u8,
) -> i32 {
    // Validate pointers
    if !validate_ptr(commitment_a_ptr)
        || !validate_ptr(commitment_b_ptr)
        || !validate_mut_ptr(result_out)
    {
        return -1;
    }

    // Create slices
    let commitment_a_slice = unsafe { slice::from_raw_parts(commitment_a_ptr, 32) };
    let commitment_b_slice = unsafe { slice::from_raw_parts(commitment_b_ptr, 32) };

    // Parse commitments
    let commitment_a = match CompressedRistretto::from_slice(commitment_a_slice).decompress() {
        Some(point) => point,
        None => return 0,
    };

    let commitment_b = match CompressedRistretto::from_slice(commitment_b_slice).decompress() {
        Some(point) => point,
        None => return 0,
    };

    // Subtract points
    let result_point = commitment_a - commitment_b;

    // Compress and write result
    let result_compressed = result_point.compress();
    let result_out_slice = unsafe { slice::from_raw_parts_mut(result_out, 32) };
    result_out_slice.copy_from_slice(result_compressed.as_bytes());

    1 // Success
}

/// Create a commitment from a transparent value
///
/// Creates: commitment = value * H + 0 * G
/// (Pedersen commitment with zero blinding factor)
///
/// Used for converting transparent outputs to commitments for balance verification.
///
/// # Arguments
/// * `value` - Transparent value (uint64)
/// * `commitment_out` - Output buffer (32 bytes)
///
/// # Returns
/// * 1 on success
/// * -1 on error
#[no_mangle]
pub extern "C" fn commitment_from_value(
    value: u64,
    commitment_out: *mut u8,
) -> i32 {
    let result = panic::catch_unwind(|| {
        commitment_from_value_impl(value, commitment_out)
    });

    match result {
        Ok(val) => val,
        Err(_) => {
            eprintln!("PANIC in commitment_from_value");
            -1
        }
    }
}

fn commitment_from_value_impl(value: u64, commitment_out: *mut u8) -> i32 {
    // Validate pointer
    if !validate_mut_ptr(commitment_out) {
        eprintln!("commitment_from_value: null or misaligned commitment_out");
        return -1;
    }

    ensure_initialized();
    let pc_gens = get_pc_gens();

    // Create commitment: value * H + 0 * G
    let value_scalar = Scalar::from(value);
    let commitment_point = value_scalar * pc_gens.B_blinding; // H generator

    // Compress and write
    let commitment_compressed = commitment_point.compress();
    let commitment_out_slice = unsafe { slice::from_raw_parts_mut(commitment_out, 32) };
    commitment_out_slice.copy_from_slice(commitment_compressed.as_bytes());

    1 // Success
}

/// Create a Pedersen commitment from value and blinding factor
///
/// Creates: C = value*H + blinding*G
///
/// This is the FULL Pedersen commitment used in confidential transactions.
/// The blinding factor hides the value and enables commitment arithmetic.
///
/// # Arguments
/// * `value` - The value to commit to (uint64)
/// * `blinding_ptr` - Pointer to 32-byte blinding factor (canonical scalar)
/// * `commitment_out` - Output buffer for 32-byte commitment
///
/// # Returns
/// * 1 on success
/// * -1 on error (null pointer, invalid blinding factor)
///
/// # Safety
/// - blinding_ptr must point to readable 32-byte buffer containing canonical scalar
/// - commitment_out must point to writable 32-byte buffer
/// - Blinding factor is NOT zeroized (caller owns the data)
///
/// # Example
/// ```c
/// uint8_t blinding[32];
/// generate_random_blinding(blinding);
///
/// uint8_t commitment[32];
/// commitment_create(1000, blinding, commitment);
/// ```
#[no_mangle]
pub extern "C" fn commitment_create(
    value: u64,
    blinding_ptr: *const u8,
    commitment_out: *mut u8,
) -> i32 {
    ffi_boundary!({
        commitment_create_impl(value, blinding_ptr, commitment_out)
    })
}

fn commitment_create_impl(
    value: u64,
    blinding_ptr: *const u8,
    commitment_out: *mut u8,
) -> i32 {
    // Validate pointers
    if !validate_ptr(blinding_ptr) {
        eprintln!("commitment_create: null or misaligned blinding_ptr");
        return -1;
    }

    if !validate_mut_ptr(commitment_out) {
        eprintln!("commitment_create: null or misaligned commitment_out");
        return -1;
    }

    ensure_initialized();
    let pc_gens = get_pc_gens();

    // Safely create blinding slice
    let blinding_bytes = unsafe {
        match make_slice(blinding_ptr, BLIND_SIZE) {
            Some(slice) => slice,
            None => {
                eprintln!("commitment_create: failed to create blinding slice");
                return -1;
            }
        }
    };

    // Convert blinding factor to Scalar
    let blinding_scalar = match Scalar::from_canonical_bytes(to_array_32(blinding_bytes)) {
        Some(s) => s,
        None => {
            eprintln!("commitment_create: invalid blinding factor (not canonical)");
            return -1;
        }
    };

    // Create Pedersen commitment: C = value*H + blinding*G
    let value_scalar = Scalar::from(value);
    let commitment_point = pc_gens.commit(value_scalar, blinding_scalar);

    // Compress and write to output
    let commitment_compressed = commitment_point.compress();
    let commitment_out_slice = unsafe { slice::from_raw_parts_mut(commitment_out, COMMITMENT_SIZE) };
    commitment_out_slice.copy_from_slice(commitment_compressed.as_bytes());

    1 // Success
}

/// Check if a commitment is the identity point (zero)
///
/// # Arguments
/// * `commitment_ptr` - Commitment to check (32 bytes)
///
/// # Returns
/// * 1 if commitment is identity
/// * 0 if commitment is not identity
/// * -1 on error
#[no_mangle]
pub extern "C" fn commitment_is_identity(commitment_ptr: *const u8) -> i32 {
    let result = panic::catch_unwind(|| {
        commitment_is_identity_impl(commitment_ptr)
    });

    match result {
        Ok(val) => val,
        Err(_) => {
            eprintln!("PANIC in commitment_is_identity");
            -1
        }
    }
}

fn commitment_is_identity_impl(commitment_ptr: *const u8) -> i32 {
    if !validate_ptr(commitment_ptr) {
        return -1;
    }

    let commitment_slice = unsafe { slice::from_raw_parts(commitment_ptr, 32) };

    let commitment = match CompressedRistretto::from_slice(commitment_slice).decompress() {
        Some(point) => point,
        None => return -1, // Invalid commitment
    };

    use curve25519_dalek_ng::traits::Identity;
    use curve25519_dalek_ng::ristretto::RistrettoPoint;

    if commitment == RistrettoPoint::identity() {
        1 // Is identity
    } else {
        0 // Not identity
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

/// Convert slice to 32-byte array
fn to_array_32(slice: &[u8]) -> [u8; 32] {
    let mut array = [0u8; 32];
    array.copy_from_slice(&slice[0..32]);
    array
}

/// Convert slice to 8-byte array
fn to_array_8(slice: &[u8]) -> [u8; 8] {
    let mut array = [0u8; 8];
    array.copy_from_slice(&slice[0..8]);
    array
}

/// Simple SHA256 hash helper
fn sha256_hash(data: &[u8]) -> Vec<u8> {
    use sha2::{Sha256, Digest};
    let mut hasher = Sha256::new();
    hasher.update(data);
    hasher.finalize().to_vec()
}

// ============================================================================
// Test Utilities - Random Scalar Generation
// ============================================================================

/// Generate a random canonical Curve25519 scalar (blinding factor)
///
/// This function generates a cryptographically secure random scalar that is
/// guaranteed to be canonical (< curve order). This is useful for testing
/// and for wallet implementations that need random blinding factors.
///
/// # Arguments
/// * `blind_out` - Output buffer for the 32-byte scalar (must not be null)
///
/// # Returns
/// * `0` on success
/// * `-1` on error (null pointer)
///
/// # Safety
/// The output buffer must be at least 32 bytes.
#[no_mangle]
pub extern "C" fn generate_random_blinding(blind_out: *mut u8) -> i32 {
    // Validate pointer
    if blind_out.is_null() {
        return -1;
    }

    // Generate random scalar using OsRng
    let result = panic::catch_unwind(|| {
        use rand_core::OsRng;
        let mut rng = OsRng;
        let scalar = Scalar::random(&mut rng);

        // Copy to output buffer
        unsafe {
            let blind_slice = slice::from_raw_parts_mut(blind_out, BLIND_SIZE);
            blind_slice.copy_from_slice(scalar.as_bytes());
        }

        0
    });

    result.unwrap_or(-1)
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_init() {
        assert_eq!(bp_init(), 0);
        assert_eq!(bp_is_initialized(), 1);
    }

    #[test]
    fn test_proof_generation() {
        bp_init();

        let value: u64 = 12345;
        let blind = [1u8; 32];
        let mut proof = vec![0u8; MAX_PROOF_SIZE];
        let mut proof_len = 0;

        let result = bp_generate(
            value,
            blind.as_ptr(),
            proof.as_mut_ptr(),
            &mut proof_len,
        );

        assert_eq!(result, 0);
        assert!(proof_len > 0);
        assert!(proof_len < MAX_PROOF_SIZE);
    }

    #[test]
    fn test_max_proof_size() {
        let size_64 = bp_max_proof_size(64);
        assert!(size_64 > 0);
        assert!(size_64 < MAX_PROOF_SIZE);

        // Should be around 674 bytes for 64-bit
        assert!(size_64 > 600 && size_64 < 900);
    }
}
