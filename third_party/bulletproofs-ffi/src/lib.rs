// Bulletproofs FFI - C bindings for Dalek Bulletproofs
//
// This provides a C-compatible interface to the Dalek Bulletproofs library
// for use in DineroCoin's confidential transactions.
//
// References:
// - https://github.com/dalek-cryptography/bulletproofs
// - https://eprint.iacr.org/2017/1066.pdf (Bulletproofs paper)

use bulletproofs::{BulletproofGens, PedersenGens, RangeProof};
use curve25519_dalek::scalar::Scalar;
use merlin::Transcript;
use rand::thread_rng;
use std::ptr;
use std::slice;

// ============================================================================
// Constants
// ============================================================================

/// Maximum number of bits for range proofs (2^64 - 1)
pub const MAX_RANGE_BITS: usize = 64;

/// Bulletproof generators (reused for performance)
static mut BP_GENS: Option<BulletproofGens> = None;
static mut PC_GENS: Option<PedersenGens> = None;

// ============================================================================
// Initialization
// ============================================================================

/// Initialize Bulletproofs generators
/// Must be called once before using any other functions
///
/// # Safety
/// This function is not thread-safe and should be called once at startup
#[no_mangle]
pub unsafe extern "C" fn bulletproofs_init() -> i32 {
    BP_GENS = Some(BulletproofGens::new(MAX_RANGE_BITS, 1));
    PC_GENS = Some(PedersenGens::default());
    0 // Success
}

/// Check if Bulletproofs is initialized
#[no_mangle]
pub extern "C" fn bulletproofs_is_initialized() -> i32 {
    unsafe {
        if BP_GENS.is_some() && PC_GENS.is_some() {
            1
        } else {
            0
        }
    }
}

// ============================================================================
// Range Proof Generation
// ============================================================================

/// Generate a Bulletproof range proof for a value
///
/// # Arguments
/// * `value` - The value to prove (0 to 2^64-1)
/// * `blinding` - 32-byte blinding factor for the commitment
/// * `proof_out` - Output buffer for the proof (must be at least 674 bytes)
/// * `proof_len_out` - Output length of the proof
///
/// # Returns
/// 0 on success, -1 on error
///
/// # Safety
/// - `blinding` must point to 32 valid bytes
/// - `proof_out` must be a valid buffer of at least 674 bytes
/// - `proof_len_out` must be a valid pointer
#[no_mangle]
pub unsafe extern "C" fn bulletproofs_rangeproof_generate(
    value: u64,
    blinding: *const u8,
    proof_out: *mut u8,
    proof_len_out: *mut usize,
) -> i32 {
    // Check initialization
    let bp_gens = match BP_GENS.as_ref() {
        Some(g) => g,
        None => return -1,
    };
    let pc_gens = match PC_GENS.as_ref() {
        Some(g) => g,
        None => return -1,
    };

    // Validate inputs
    if blinding.is_null() || proof_out.is_null() || proof_len_out.is_null() {
        return -1;
    }

    // Convert blinding factor to Scalar
    let blinding_bytes = slice::from_raw_parts(blinding, 32);
    let blinding_scalar = match Scalar::from_canonical_bytes(*array_ref![blinding_bytes, 0, 32]) {
        Some(s) => s,
        None => return -1,
    };

    // Create transcript
    let mut transcript = Transcript::new(b"DineroCoin Bulletproof");

    // Generate proof
    let (proof, _committed) = match RangeProof::prove_single(
        bp_gens,
        pc_gens,
        &mut transcript,
        value,
        &blinding_scalar,
        MAX_RANGE_BITS,
    ) {
        Ok(result) => result,
        Err(_) => return -1,
    };

    // Serialize proof
    let proof_bytes = proof.to_bytes();

    // Check output buffer size
    if proof_bytes.len() > 2048 {
        // Proof too large
        return -1;
    }

    // Copy proof to output buffer
    ptr::copy_nonoverlapping(proof_bytes.as_ptr(), proof_out, proof_bytes.len());
    *proof_len_out = proof_bytes.len();

    0 // Success
}

// ============================================================================
// Range Proof Verification
// ============================================================================

/// Verify a Bulletproof range proof
///
/// # Arguments
/// * `commitment` - 32-byte Pedersen commitment to verify against
/// * `proof` - Serialized Bulletproof
/// * `proof_len` - Length of the proof in bytes
///
/// # Returns
/// 1 if proof is valid, 0 if invalid, -1 on error
///
/// # Safety
/// - `commitment` must point to 32 valid bytes
/// - `proof` must point to `proof_len` valid bytes
#[no_mangle]
pub unsafe extern "C" fn bulletproofs_rangeproof_verify(
    commitment: *const u8,
    proof: *const u8,
    proof_len: usize,
) -> i32 {
    // Check initialization
    let bp_gens = match BP_GENS.as_ref() {
        Some(g) => g,
        None => return -1,
    };
    let pc_gens = match PC_GENS.as_ref() {
        Some(g) => g,
        None => return -1,
    };

    // Validate inputs
    if commitment.is_null() || proof.is_null() || proof_len == 0 || proof_len > 2048 {
        return -1;
    }

    // Parse commitment (CompressedRistretto point)
    let commitment_bytes = slice::from_raw_parts(commitment, 32);
    let commitment_point =
        match curve25519_dalek::ristretto::CompressedRistretto::from_slice(commitment_bytes)
            .decompress()
        {
            Some(p) => p,
            None => return -1,
        };

    // Parse proof
    let proof_bytes = slice::from_raw_parts(proof, proof_len);
    let proof = match RangeProof::from_bytes(proof_bytes) {
        Ok(p) => p,
        Err(_) => return -1,
    };

    // Create transcript (must match generation)
    let mut transcript = Transcript::new(b"DineroCoin Bulletproof");

    // Verify proof
    match proof.verify_single(bp_gens, pc_gens, &mut transcript, &commitment_point, MAX_RANGE_BITS)
    {
        Ok(_) => 1,  // Valid
        Err(_) => 0, // Invalid
    }
}

// ============================================================================
// Batch Verification (Optimization)
// ============================================================================

/// Verify multiple Bulletproofs in a batch (faster than individual verification)
///
/// # Arguments
/// * `commitments` - Array of 32-byte commitments
/// * `proofs` - Array of proof pointers
/// * `proof_lens` - Array of proof lengths
/// * `count` - Number of proofs to verify
///
/// # Returns
/// 1 if all proofs are valid, 0 if any invalid, -1 on error
///
/// # Safety
/// - All pointers must be valid
/// - Arrays must have `count` elements
#[no_mangle]
pub unsafe extern "C" fn bulletproofs_rangeproof_verify_batch(
    commitments: *const *const u8,
    proofs: *const *const u8,
    proof_lens: *const usize,
    count: usize,
) -> i32 {
    // Check initialization
    let bp_gens = match BP_GENS.as_ref() {
        Some(g) => g,
        None => return -1,
    };
    let pc_gens = match PC_GENS.as_ref() {
        Some(g) => g,
        None => return -1,
    };

    // Validate inputs
    if commitments.is_null() || proofs.is_null() || proof_lens.is_null() || count == 0 {
        return -1;
    }

    let commitments_slice = slice::from_raw_parts(commitments, count);
    let proofs_slice = slice::from_raw_parts(proofs, count);
    let proof_lens_slice = slice::from_raw_parts(proof_lens, count);

    // Parse all commitments and proofs
    let mut commitment_points = Vec::with_capacity(count);
    let mut range_proofs = Vec::with_capacity(count);
    let mut transcripts = Vec::with_capacity(count);

    for i in 0..count {
        // Parse commitment
        let commitment_bytes = slice::from_raw_parts(commitments_slice[i], 32);
        let commitment_point =
            match curve25519_dalek::ristretto::CompressedRistretto::from_slice(commitment_bytes)
                .decompress()
            {
                Some(p) => p,
                None => return -1,
            };
        commitment_points.push(commitment_point);

        // Parse proof
        let proof_bytes = slice::from_raw_parts(proofs_slice[i], proof_lens_slice[i]);
        let proof = match RangeProof::from_bytes(proof_bytes) {
            Ok(p) => p,
            Err(_) => return -1,
        };
        range_proofs.push(proof);

        // Create transcript
        transcripts.push(Transcript::new(b"DineroCoin Bulletproof"));
    }

    // Batch verify
    let transcript_refs: Vec<&mut Transcript> = transcripts.iter_mut().collect();
    let proof_refs: Vec<&RangeProof> = range_proofs.iter().collect();
    let commitment_refs: Vec<&_> = commitment_points.iter().collect();

    match RangeProof::verify_multiple(
        bp_gens,
        pc_gens,
        &mut transcript_refs[..],
        &proof_refs[..],
        &commitment_refs[..],
        MAX_RANGE_BITS,
    ) {
        Ok(_) => 1,  // All valid
        Err(_) => 0, // At least one invalid
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

/// Get the maximum size of a Bulletproof for the given bit length
///
/// # Arguments
/// * `n_bits` - Number of bits in the range proof (typically 64)
///
/// # Returns
/// Maximum proof size in bytes, or 0 on error
#[no_mangle]
pub extern "C" fn bulletproofs_get_max_proof_size(n_bits: usize) -> usize {
    if n_bits > MAX_RANGE_BITS || n_bits == 0 {
        return 0;
    }

    // Bulletproof size formula: 2*floor(log2(n)) + 9 group elements
    // Each group element is 32 bytes
    let log2_n = (n_bits as f64).log2().floor() as usize;
    (2 * log2_n + 9) * 32
}

/// Get library version string
///
/// # Returns
/// Pointer to static version string (do not free)
#[no_mangle]
pub extern "C" fn bulletproofs_version() -> *const i8 {
    b"DineroCoin Bulletproofs FFI 1.0.0 (Dalek)\0".as_ptr() as *const i8
}

// ============================================================================
// Helper Macros
// ============================================================================

// array_ref macro for safe array indexing
macro_rules! array_ref {
    ($arr:expr, $offset:expr, $len:expr) => {{
        {
            #[inline]
            fn as_array<T>(slice: &[T]) -> &[T; $len] {
                unsafe { &*(slice.as_ptr() as *const [T; $len]) }
            }
            as_array(&$arr[$offset..$offset + $len])
        }
    }};
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_rangeproof_generation_and_verification() {
        unsafe {
            // Initialize
            assert_eq!(bulletproofs_init(), 0);

            // Generate proof for value 12345
            let value: u64 = 12345;
            let blinding = [0u8; 32]; // Simple blinding factor
            let mut proof = vec![0u8; 2048];
            let mut proof_len = 0;

            let result = bulletproofs_rangeproof_generate(
                value,
                blinding.as_ptr(),
                proof.as_mut_ptr(),
                &mut proof_len,
            );

            assert_eq!(result, 0);
            assert!(proof_len > 0);
            assert!(proof_len < 2048);

            // TODO: Verify proof (needs commitment calculation)
        }
    }
}
