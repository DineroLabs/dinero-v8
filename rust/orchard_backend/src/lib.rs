//! Pinned Orchard cryptographic backend for the staged v8.1.13 integration.
//! The bundle encoding is a draft inner codec, not a consensus transaction.
//! Transaction identity, transparent scripts and chainstate checks belong to
//! the host. No function in this crate declares a Dinero transaction valid.

use nonempty::NonEmpty;
mod frontier;
mod wallet;
use orchard::{
    bundle::{Authorized, BundleVersion, Flags, TxVersion},
    circuit::VerifyingKey,
    note::{ExtractedNoteCommitment, Nullifier, TransmittedNoteCiphertext},
    primitives::redpallas::{self, Binding, SpendAuth},
    value::ValueCommitment,
    Action, Anchor, Bundle, Proof,
};

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::OnceLock;

const MAGIC: [u8; 8] = *b"DNORCH01";
const WIRE_VERSION: u8 = 1;
// Draft protocol/ABI-v1 limit; do not increase without a versioned codec and ABI review.
pub const MAX_ACTIONS: usize = 8;
const MAX_BUNDLE_BYTES: usize = 64 * 1024;
const MAX_MONEY: u64 = 26_542_800_000_000_000;
const EFFECT_TX_VERSION: TxVersion = TxVersion::V5;
const BUNDLE_VERSION: BundleVersion = BundleVersion::orchard_v2();
static VERIFYING_KEY: OnceLock<VerifyingKey> = OnceLock::new();

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Status {
    NullArgument = 1,
    Limit = 2,
    Truncated = 3,
    Format = 4,
    Encoding = 5,
    Proof = 6,
    SpendSignature = 7,
    BindingSignature = 8,
    Panic = 9,
    TrailingBytes = 10,
    Money = 11,
    DuplicateNullifier = 12,
    BalanceMismatch = 13,
}

/// ABI-v1 selection, derived from the same versions used for commitments/keys.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ProtocolProfile {
    pub transaction_version: u32,
    pub bundle_wire_profile: u32,
    pub pool_profile: u32,
    pub circuit_profile: u32,
    pub effect_commitment_version: u32,
}
fn protocol_profile() -> Result<ProtocolProfile, Status> {
    // A future profile needs an explicit ABI/codec change, never an implicit
    // selection with the existing v1 identifier.
    if BUNDLE_VERSION != BundleVersion::orchard_v2() {
        return Err(Status::Format);
    }
    let circuit_profile = match BUNDLE_VERSION.circuit_version() {
        orchard::circuit::OrchardCircuitVersion::FixedPostNu6_2 => 1,
        _ => return Err(Status::Format),
    };
    let effect_commitment_version = match EFFECT_TX_VERSION {
        TxVersion::V5 => 5,
        TxVersion::V6 => 6,
        _ => return Err(Status::Format),
    };
    Ok(ProtocolProfile {
        transaction_version: 7,
        bundle_wire_profile: WIRE_VERSION as u32,
        pool_profile: 1,
        circuit_profile,
        effect_commitment_version,
    })
}

/// All fields come from the same owned, strictly decoded bundle. These are
/// unverified facts until authorization verification succeeds. The host must
/// bind `effect` into its transaction signing message and use both commitments
/// where the transaction's authenticated-body rules require them.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BundleFacts {
    pub effect: [u8; 32],
    pub authorization: [u8; 32],
    pub anchor: [u8; 32],
    pub value_balance: i64,
    pub action_count: u32,
    pub flags: u8,
    pub reserved: [u8; 3],
    pub nullifiers: [[u8; 32]; MAX_ACTIONS],
    pub commitments: [[u8; 32]; MAX_ACTIONS],
}

/// Opaque immutable object: decoding cannot set any verification state.
pub struct ParsedBundle {
    bundle: Bundle<Authorized, i64>,
    facts: BundleFacts,
}

impl ParsedBundle {
    fn decode(bytes: &[u8]) -> Result<Self, Status> {
        let _ = protocol_profile()?;
        let bundle = parse_bundle(bytes)?;
        let mut facts = BundleFacts {
            effect: bundle
                .commitment(EFFECT_TX_VERSION)
                .map_err(|_| Status::Format)?
                .into(),
            authorization: bundle
                .authorizing_commitment(EFFECT_TX_VERSION)
                .map_err(|_| Status::Format)?
                .0
                .as_bytes()
                .try_into()
                .map_err(|_| Status::Format)?,
            anchor: bundle.anchor().to_bytes(),
            value_balance: *bundle.value_balance(),
            action_count: bundle.actions().len() as u32,
            flags: bundle.flag_byte(),
            reserved: [0; 3],
            nullifiers: [[0; 32]; MAX_ACTIONS],
            commitments: [[0; 32]; MAX_ACTIONS],
        };
        for (i, action) in bundle.actions().iter().enumerate() {
            facts.nullifiers[i] = action.nullifier().to_bytes();
            facts.commitments[i] = action.cmx().to_bytes();
        }
        Ok(Self { bundle, facts })
    }

    fn verify(&self, digest: &[u8; 32], required_value_balance: i64) -> Result<(), Status> {
        // Orchard positive value_balance means value LEAVES its pool.
        // The host derives this from resolved transparent coins/outputs/fee.
        if self.facts.value_balance != required_value_balance {
            return Err(Status::BalanceMismatch);
        }
        // Cheap authorizations first. No verification cache keyed only by
        // effects: proof/signature bytes and the supplied digest both matter.
        for action in self.bundle.actions() {
            action
                .rk()
                .verify(digest, action.authorization())
                .map_err(|_| Status::SpendSignature)?;
        }
        self.bundle
            .binding_validating_key()
            .verify(digest, self.bundle.authorization().binding_signature())
            .map_err(|_| Status::BindingSignature)?;
        let vk =
            VERIFYING_KEY.get_or_init(|| VerifyingKey::build(BUNDLE_VERSION.circuit_version()));
        self.bundle.verify_proof(vk).map_err(|_| Status::Proof)
    }
}

struct Reader<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> Reader<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }

    fn take(&mut self, len: usize) -> Result<&'a [u8], Status> {
        let end = self.offset.checked_add(len).ok_or(Status::Limit)?;
        let result = self.bytes.get(self.offset..end).ok_or(Status::Truncated)?;
        self.offset = end;
        Ok(result)
    }

    fn array<const N: usize>(&mut self) -> Result<[u8; N], Status> {
        self.take(N)?.try_into().map_err(|_| Status::Truncated)
    }

    fn byte(&mut self) -> Result<u8, Status> {
        Ok(self.array::<1>()?[0])
    }

    fn u32(&mut self) -> Result<u32, Status> {
        Ok(u32::from_le_bytes(self.array::<4>()?))
    }

    fn i64(&mut self) -> Result<i64, Status> {
        Ok(i64::from_le_bytes(self.array::<8>()?))
    }

    fn finish(&self) -> Result<(), Status> {
        if self.offset == self.bytes.len() {
            Ok(())
        } else {
            Err(Status::TrailingBytes)
        }
    }
}

fn parse_bundle(bytes: &[u8]) -> Result<Bundle<Authorized, i64>, Status> {
    if bytes.len() > MAX_BUNDLE_BYTES {
        return Err(Status::Limit);
    }
    let mut reader = Reader::new(bytes);
    if reader.array::<8>()? != MAGIC || reader.byte()? != WIRE_VERSION {
        return Err(Status::Format);
    }
    let flags = Flags::from_byte(reader.byte()?, BUNDLE_VERSION).ok_or(Status::Format)?;
    let count = reader.u32()? as usize;
    if !(1..=MAX_ACTIONS).contains(&count) {
        return Err(Status::Limit);
    }
    let value_balance = reader.i64()?;
    if value_balance.unsigned_abs() > MAX_MONEY {
        return Err(Status::Money);
    }
    let anchor = Option::<Anchor>::from(Anchor::from_bytes(reader.array::<32>()?))
        .ok_or(Status::Encoding)?;

    let mut actions = Vec::with_capacity(count);
    let mut nullifiers = std::collections::BTreeSet::new();
    for _ in 0..count {
        let cv =
            Option::<ValueCommitment>::from(ValueCommitment::from_bytes(&reader.array::<32>()?))
                .ok_or(Status::Encoding)?;
        let nf = Option::<Nullifier>::from(Nullifier::from_bytes(&reader.array::<32>()?))
            .ok_or(Status::Encoding)?;
        if !nullifiers.insert(nf.to_bytes()) {
            return Err(Status::DuplicateNullifier);
        }
        let rk = redpallas::VerificationKey::<SpendAuth>::try_from(reader.array::<32>()?)
            .map_err(|_| Status::Encoding)?;
        let cmx = Option::<ExtractedNoteCommitment>::from(ExtractedNoteCommitment::from_bytes(
            &reader.array::<32>()?,
        ))
        .ok_or(Status::Encoding)?;
        let encrypted_note = TransmittedNoteCiphertext {
            epk_bytes: reader.array::<32>()?,
            enc_ciphertext: reader.array::<580>()?,
            out_ciphertext: reader.array::<80>()?,
        };
        let signature = redpallas::Signature::<SpendAuth>::from(reader.array::<64>()?);
        let action = Action::from_parts(nf, rk, cmx, encrypted_note, cv, signature)
            .map_err(|_| Status::Encoding)?;
        actions.push(action);
    }

    let proof_len = reader.u32()? as usize;
    if proof_len != Proof::expected_proof_size(count) {
        return Err(Status::Format);
    }
    let proof = Proof::new(reader.take(proof_len)?.to_vec());
    let binding_signature = redpallas::Signature::<Binding>::from(reader.array::<64>()?);
    reader.finish()?;
    let actions = NonEmpty::from_vec(actions).ok_or(Status::Limit)?;
    Bundle::try_from_parts(
        actions,
        flags,
        value_balance,
        anchor,
        Authorized::from_parts(proof, binding_signature),
        BUNDLE_VERSION,
    )
    .map_err(|_| Status::Format)
}

fn boundary(f: impl FnOnce() -> Result<(), Status>) -> i32 {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => 0,
        Ok(Err(status)) => status as i32,
        Err(payload) => {
            // Dropping an arbitrary panic payload can itself panic. Avoid a
            // second unwind escaping this C boundary; leaking a panic payload
            // is preferable to unwinding through foreign code.
            std::mem::forget(payload);
            Status::Panic as i32
        }
    }
}

/// # Safety
/// Output must be valid writable aligned storage; it is unchanged on failure.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_protocol_v1(output: *mut ProtocolProfile) -> i32 {
    if output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let profile = protocol_profile()?;
        unsafe {
            output.write(profile);
        }
        Ok(())
    })
}

/// # Safety
/// Non-null byte and output pointers must be valid, non-aliasing and aligned;
/// input bytes must stay immutable during this call. A successful handle is
/// owned by the caller and must be freed exactly once. Output is unchanged on
/// any failure; no partial handle escapes.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_decode_v1(
    bytes: *const u8,
    length: usize,
    output: *mut *mut ParsedBundle,
) -> i32 {
    if length > MAX_BUNDLE_BYTES {
        return Status::Limit as i32;
    }
    if bytes.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let owned = unsafe { std::slice::from_raw_parts(bytes, length) }.to_vec();
        let parsed = Box::new(ParsedBundle::decode(&owned)?);
        unsafe {
            output.write(Box::into_raw(parsed));
        }
        Ok(())
    })
}

/// # Safety
/// `handle` is a live immutable handle from decode. `output` is valid aligned
/// writable storage, does not alias the handle, and is unchanged on failure.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_facts_v1(
    handle: *const ParsedBundle,
    output: *mut BundleFacts,
) -> i32 {
    if handle.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        unsafe {
            output.write((*handle).facts);
        }
        Ok(())
    })
}

/// # Safety
/// `handle` remains live throughout the call. `digest` points to 32 immutable
/// bytes derived by the host from the complete signing context, not supplied
/// by the transaction. `required_value_balance` is host-derived. Success is
/// Orchard cryptographic authorization only, not chainstate/script validation.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_verify_v1(
    handle: *const ParsedBundle,
    digest: *const u8,
    required_value_balance: i64,
) -> i32 {
    if handle.is_null() || digest.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let digest: [u8; 32] = unsafe { std::slice::from_raw_parts(digest, 32) }
            .try_into()
            .map_err(|_| Status::Format)?;
        unsafe { &*handle }.verify(&digest, required_value_balance)
    })
}

/// # Safety
/// `handle` is null or a live handle returned by decode; it must not be used
/// concurrently or again after this call. Null is accepted for RAII cleanup.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_free_v1(handle: *mut ParsedBundle) -> i32 {
    // The handle is consumed even if a destructor panics. Never retry free.
    unsafe { free_owned(handle) }
}

unsafe fn free_owned<T>(handle: *mut T) -> i32 {
    boundary(|| {
        if !handle.is_null() {
            unsafe {
                drop(Box::from_raw(handle));
            }
        }
        Ok(())
    })
}

/// Protocol-v1 monetary bound, exposed for the C++/host consistency gate.
#[no_mangle]
pub extern "C" fn dinero_orchard_max_money_v1() -> u64 {
    MAX_MONEY
}

/// Protocol-v1 action bound, exposed for the cross-language consistency gate.
#[no_mangle]
pub extern "C" fn dinero_orchard_max_actions_v1() -> u32 {
    MAX_ACTIONS as u32
}

#[cfg(test)]
mod tests {
    use super::*;
    const BUNDLE: &[u8] = include_bytes!("../tests/fixtures/candidate-spend.bundle");
    const DIGEST: &[u8; 32] = include_bytes!("../tests/fixtures/candidate-spend.digest");
    const EFFECT: &[u8; 32] = include_bytes!("../tests/fixtures/candidate-spend.effect");

    #[test]
    fn honest_fixture_verifies_and_exports_exact_bound_effects() {
        let parsed = ParsedBundle::decode(BUNDLE).unwrap();
        assert_eq!(parsed.facts.effect, *EFFECT);
        assert_eq!(parsed.facts.action_count, 2);
        assert_eq!(parsed.facts.reserved, [0; 3]);
        assert_eq!(parsed.facts.nullifiers[2..], [[0; 32]; 6]);
        assert_eq!(parsed.verify(DIGEST, parsed.facts.value_balance), Ok(()));
    }

    #[test]
    fn host_balance_and_signing_message_are_required() {
        let parsed = ParsedBundle::decode(BUNDLE).unwrap();
        assert_eq!(
            parsed.verify(DIGEST, parsed.facts.value_balance + 1),
            Err(Status::BalanceMismatch)
        );
        let mut different_message = *DIGEST;
        different_message[0] ^= 1;
        assert_eq!(
            parsed.verify(&different_message, parsed.facts.value_balance),
            Err(Status::SpendSignature)
        );
    }

    #[test]
    fn proof_spend_and_binding_authorizations_each_checked() {
        // Corruption rejection checks on a synthetic honest proof. These do
        // not constitute a proof-system soundness review.
        for (offset, expected) in [
            (54 + 820, Status::SpendSignature),
            (BUNDLE.len() - 1, Status::BindingSignature),
            (54 + 884 * 2 + 4 + 30, Status::Proof),
        ] {
            let mut bytes = BUNDLE.to_vec();
            bytes[offset] ^= 1;
            let parsed = ParsedBundle::decode(&bytes).unwrap();
            assert_eq!(
                parsed.verify(DIGEST, parsed.facts.value_balance),
                Err(expected)
            );
        }
    }

    #[test]
    fn noncanonical_signature_components_reject_during_verification() {
        // RedPallas Signature::from is only a byte container. The pinned
        // verifier must reject noncanonical R and S for BOTH signature roles.
        // Bounded rejection checks on the public synthetic honest fixture.
        for (start, expected) in [
            (54 + 820, Status::SpendSignature),
            (BUNDLE.len() - 64, Status::BindingSignature),
        ] {
            for component in [0, 32] {
                let mut bytes = BUNDLE.to_vec();
                bytes[start + component..start + component + 32].fill(0xff);
                let parsed = ParsedBundle::decode(&bytes).unwrap();
                assert_eq!(
                    parsed.verify(DIGEST, parsed.facts.value_balance),
                    Err(expected)
                );
            }
        }
    }

    #[test]
    fn exact_codec_rejects_trailing_and_incomplete_encodings() {
        let mut trailing = BUNDLE.to_vec();
        trailing.push(0);
        assert!(matches!(
            ParsedBundle::decode(&trailing),
            Err(Status::TrailingBytes)
        ));
        for end in [
            0,
            7,
            8,
            9,
            13,
            21,
            53,
            54,
            938,
            BUNDLE.len() - 65,
            BUNDLE.len() - 1,
        ] {
            assert!(
                ParsedBundle::decode(&BUNDLE[..end]).is_err(),
                "prefix {end}"
            );
        }
        let mut wrong_profile = BUNDLE.to_vec();
        wrong_profile[8] = 2;
        assert!(matches!(
            ParsedBundle::decode(&wrong_profile),
            Err(Status::Format)
        ));
    }

    #[test]
    fn counts_and_money_are_bounded_before_expensive_verification() {
        assert!(matches!(
            ParsedBundle::decode(&vec![0; MAX_BUNDLE_BYTES + 1]),
            Err(Status::Limit)
        ));
        for count in [0_u32, 9, u32::MAX] {
            let mut bytes = BUNDLE.to_vec();
            bytes[10..14].copy_from_slice(&count.to_le_bytes());
            assert!(matches!(ParsedBundle::decode(&bytes), Err(Status::Limit)));
        }
        for balance in [
            i64::MIN,
            i64::MAX,
            MAX_MONEY as i64 + 1,
            -(MAX_MONEY as i64) - 1,
        ] {
            let mut bytes = BUNDLE.to_vec();
            bytes[14..22].copy_from_slice(&balance.to_le_bytes());
            assert!(matches!(ParsedBundle::decode(&bytes), Err(Status::Money)));
        }
    }

    #[test]
    fn duplicate_nullifiers_are_rejected_in_the_decoded_bundle() {
        let mut bytes = BUNDLE.to_vec();
        let first = bytes[54 + 32..54 + 64].to_vec();
        bytes[54 + 884 + 32..54 + 884 + 64].copy_from_slice(&first);
        assert!(matches!(
            ParsedBundle::decode(&bytes),
            Err(Status::DuplicateNullifier)
        ));
    }

    #[test]
    fn ffi_owns_input_and_does_not_publish_partial_output() {
        let mut bytes = BUNDLE.to_vec();
        let mut handle = std::ptr::null_mut();
        // SAFETY: all test pointers refer to live buffers with no aliasing.
        unsafe {
            assert_eq!(
                dinero_orchard_decode_v1(bytes.as_ptr(), bytes.len(), &mut handle),
                0
            );
            assert!(!handle.is_null());
            bytes.fill(0);
            // Every field has a valid all-zero representation; deliberately do
            // not seed output from the expected facts.
            let mut output: BundleFacts = std::mem::zeroed();
            assert_eq!(dinero_orchard_facts_v1(handle, &mut output), 0);
            assert_eq!(output.effect, *EFFECT);
            assert_eq!(output, (*handle).facts);
            assert_eq!(
                dinero_orchard_verify_v1(handle, DIGEST.as_ptr(), output.value_balance),
                0
            );
            let mut failed_output = handle;
            assert_ne!(
                dinero_orchard_decode_v1(bytes.as_ptr(), bytes.len(), &mut failed_output),
                0
            );
            assert_eq!(failed_output, handle);
            let before = output;
            assert_eq!(
                dinero_orchard_facts_v1(std::ptr::null(), &mut output),
                Status::NullArgument as i32
            );
            assert_eq!(before, output);
            assert_eq!(dinero_orchard_free_v1(handle), 0);
            assert_eq!(dinero_orchard_free_v1(std::ptr::null_mut()), 0);
        }
    }

    #[test]
    fn destructor_panic_is_contained_and_handle_consumed() {
        struct PanicsOnDrop;
        impl Drop for PanicsOnDrop {
            fn drop(&mut self) {
                panic!("test destructor containment");
            }
        }
        let handle = Box::into_raw(Box::new(PanicsOnDrop));
        // SAFETY: one owned allocation is consumed once, including on panic.
        assert_eq!(unsafe { free_owned(handle) }, Status::Panic as i32);
        assert_eq!(
            BUNDLE_VERSION.circuit_version(),
            orchard::circuit::OrchardCircuitVersion::FixedPostNu6_2
        );
    }

    #[test]
    fn selected_protocol_descriptor_and_null_output_are_checked() {
        let mut output = ProtocolProfile {
            transaction_version: 0,
            bundle_wire_profile: 0,
            pool_profile: 0,
            circuit_profile: 0,
            effect_commitment_version: 0,
        };
        // SAFETY: non-aliasing initialized output storage.
        assert_eq!(unsafe { dinero_orchard_protocol_v1(&mut output) }, 0);
        assert_eq!(
            output,
            ProtocolProfile {
                transaction_version: 7,
                bundle_wire_profile: 1,
                pool_profile: 1,
                circuit_profile: 1,
                effect_commitment_version: 5
            }
        );
        assert_eq!(
            unsafe { dinero_orchard_protocol_v1(std::ptr::null_mut()) },
            Status::NullArgument as i32
        );
        assert_eq!(std::mem::size_of::<ProtocolProfile>(), 20);
    }

    #[test]
    fn abi_layout_and_unwind_boundary_are_fixed() {
        let header = include_str!("../include/orchard_backend_ffi.h");
        for (name, status) in [
            ("NULL_ARGUMENT", Status::NullArgument),
            ("LIMIT", Status::Limit),
            ("TRUNCATED", Status::Truncated),
            ("FORMAT", Status::Format),
            ("ENCODING", Status::Encoding),
            ("PROOF", Status::Proof),
            ("SPEND_SIGNATURE", Status::SpendSignature),
            ("BINDING_SIGNATURE", Status::BindingSignature),
            ("PANIC", Status::Panic),
            ("TRAILING_BYTES", Status::TrailingBytes),
            ("MONEY", Status::Money),
            ("DUPLICATE_NULLIFIER", Status::DuplicateNullifier),
            ("BALANCE_MISMATCH", Status::BalanceMismatch),
        ] {
            assert!(header.contains(&format!("DINERO_ORCHARD_{} = {},", name, status as i32)));
        }
        assert_eq!(std::mem::size_of::<BundleFacts>(), 624);
        assert_eq!(std::mem::offset_of!(BundleFacts, value_balance), 96);
        assert_eq!(std::mem::offset_of!(BundleFacts, nullifiers), 112);
        assert_eq!(
            boundary(|| panic!("test panic containment")),
            Status::Panic as i32
        );
        struct PanickingPayload;
        impl Drop for PanickingPayload {
            fn drop(&mut self) {
                panic!("payload drop must not cross boundary");
            }
        }
        assert_eq!(
            boundary(|| std::panic::panic_any(PanickingPayload)),
            Status::Panic as i32
        );
    }
}
