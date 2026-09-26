//! Owned one-use shield construction. Host transaction context is still required.
use crate::{
    boundary, free_owned, wallet::WalletKeys, wallet_note::ReceivedNote, BundleFacts, ParsedBundle,
    Status, BUNDLE_VERSION, EFFECT_TX_VERSION, MAGIC, MAX_ACTIONS, MAX_BUNDLE_BYTES, MAX_MONEY,
    WIRE_VERSION,
};
use incrementalmerkletree::Hashable;
use orchard::{
    builder::{Builder, BundleType, InProgress, Unauthorized, Unproven},
    bundle::{Authorized, Flags},
    circuit::ProvingKey,
    keys::{FullViewingKey, SpendAuthorizingKey, SpendingKey},
    tree::{MerkleHashOrchard, MerklePath},
    value::NoteValue,
    Address, Anchor, Bundle,
};
use rand::rngs::OsRng;
use std::collections::BTreeSet;
use std::sync::{Mutex, OnceLock};
use zeroize::Zeroizing;
use zip32::Scope;

type UnprovedBundle = Bundle<InProgress<Unproven, Unauthorized>, i64>;
static PROVING_KEY: OnceLock<ProvingKey> = OnceLock::new();
static PROVER_POOL: OnceLock<Result<rayon::ThreadPool, Status>> = OnceLock::new();
static PROVER_MUTEX: Mutex<()> = Mutex::new(());

#[repr(C)]
pub struct Payment {
    amount: u64,
    recipient: [u8; 43],
    memo: [u8; 512],
}
pub struct WalletPlan {
    bundle: Option<UnprovedBundle>,
    facts: BundleFacts,
    spending_key: Option<Zeroizing<[u8; 32]>>,
}
#[repr(C)]
pub struct BuiltBundle {
    length: u32,
    bytes: [u8; MAX_BUNDLE_BYTES],
}
#[repr(C)]
pub struct SpendInput {
    position: u32,
    path: [[u8; 32]; 32],
    note: *const ReceivedNote,
}
struct WitnessedNote<'a> {
    note: &'a ReceivedNote,
    position: u32,
    path: [[u8; 32]; 32],
}
fn add_payments(
    builder: &mut Builder,
    fvk: &FullViewingKey,
    payments: &[Payment],
) -> Result<u64, Status> {
    if payments.len() > MAX_ACTIONS {
        return Err(Status::Limit);
    }
    let mut total = 0;
    for payment in payments {
        if payment.amount == 0 || payment.amount > MAX_MONEY || total > MAX_MONEY - payment.amount {
            return Err(Status::Money);
        }
        total += payment.amount;
        let address = Option::<Address>::from(Address::from_raw_address_bytes(&payment.recipient))
            .ok_or(Status::Encoding)?;
        builder
            .add_output(
                Some(fvk.to_ovk(Scope::External)),
                address,
                NoteValue::from_raw(payment.amount),
                payment.memo,
            )
            .map_err(|_| Status::Format)?;
    }
    Ok(total)
}
fn finish_plan(
    builder: Builder,
    balance: i64,
    key: Option<Zeroizing<[u8; 32]>>,
) -> Result<WalletPlan, Status> {
    let (bundle, _) = builder
        .build::<i64>(&mut OsRng)
        .map_err(|_| Status::Format)?
        .ok_or(Status::Format)?;
    if bundle.actions().len() > MAX_ACTIONS || *bundle.value_balance() != balance {
        return Err(Status::Format);
    }
    let mut facts = BundleFacts {
        effect: bundle
            .commitment(EFFECT_TX_VERSION)
            .map_err(|_| Status::Format)?
            .into(),
        authorization: [0; 32],
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
    Ok(WalletPlan {
        bundle: Some(bundle),
        facts,
        spending_key: key,
    })
}
fn prepare(keys: &WalletKeys, payments: &[Payment]) -> Result<WalletPlan, Status> {
    if payments.is_empty() {
        return Err(Status::Limit);
    }
    let fvk = keys.viewing()?;
    let mut builder = Builder::new(
        BundleType::DEFAULT,
        BUNDLE_VERSION,
        Flags::SPENDS_DISABLED,
        MerkleHashOrchard::empty_root(32.into()).into(),
    )
    .map_err(|_| Status::Format)?;
    let total = add_payments(&mut builder, &fvk, payments)?;
    finish_plan(builder, -(total as i64), None)
}
fn prepare_spend(
    keys: &WalletKeys,
    inputs: &[WitnessedNote<'_>],
    anchor: [u8; 32],
    payments: &[Payment],
) -> Result<WalletPlan, Status> {
    if inputs.is_empty() || inputs.len() > MAX_ACTIONS {
        return Err(Status::Limit);
    }
    let anchor = Option::<Anchor>::from(Anchor::from_bytes(anchor)).ok_or(Status::Encoding)?;
    let fvk = keys.viewing()?;
    let mut builder = Builder::new(
        BundleType::DEFAULT,
        BUNDLE_VERSION,
        BUNDLE_VERSION.default_flags(),
        anchor,
    )
    .map_err(|_| Status::Format)?;
    let mut seen = BTreeSet::new();
    let mut total = 0u64;
    for input in inputs {
        if input.note.fvk.to_bytes() != fvk.to_bytes() {
            return Err(Status::Encoding);
        }
        if !seen.insert(input.note.note.nullifier(&fvk).to_bytes()) {
            return Err(Status::DuplicateNullifier);
        }
        let amount = input.note.note.value().inner();
        if amount == 0 || amount > MAX_MONEY || total > MAX_MONEY - amount {
            return Err(Status::Money);
        }
        total += amount;
        let mut path = [MerkleHashOrchard::empty_root(0.into()); 32];
        for (node, raw) in path.iter_mut().zip(&input.path) {
            *node = Option::from(MerkleHashOrchard::from_bytes(raw)).ok_or(Status::Encoding)?;
        }
        let path = MerklePath::from_parts(input.position, path);
        if path.root(input.note.note.commitment().into()) != anchor {
            return Err(Status::Format);
        }
        builder
            .add_spend(fvk.clone(), input.note.note, path)
            .map_err(|_| Status::Format)?;
    }
    let output_total = add_payments(&mut builder, &fvk, payments)?;
    finish_plan(
        builder,
        total as i64 - output_total as i64,
        Some(keys.secret_copy()),
    )
}
fn encode(bundle: &Bundle<Authorized, i64>) -> Result<Vec<u8>, Status> {
    if bundle.actions().len() > MAX_ACTIONS {
        return Err(Status::Limit);
    }
    let mut bytes = MAGIC.to_vec();
    bytes.extend_from_slice(&[WIRE_VERSION, bundle.flag_byte()]);
    bytes.extend_from_slice(&(bundle.actions().len() as u32).to_le_bytes());
    bytes.extend_from_slice(&bundle.value_balance().to_le_bytes());
    bytes.extend_from_slice(&bundle.anchor().to_bytes());
    for action in bundle.actions() {
        bytes.extend_from_slice(&action.cv_net().to_bytes());
        bytes.extend_from_slice(&action.nullifier().to_bytes());
        let rk: [u8; 32] = action.rk().into();
        bytes.extend_from_slice(&rk);
        bytes.extend_from_slice(&action.cmx().to_bytes());
        let note = action.encrypted_note();
        bytes.extend_from_slice(&note.epk_bytes);
        bytes.extend_from_slice(&note.enc_ciphertext);
        bytes.extend_from_slice(&note.out_ciphertext);
        let sig: [u8; 64] = action.authorization().into();
        bytes.extend_from_slice(&sig);
    }
    let proof = bundle.authorization().proof().as_ref();
    if proof.len() > MAX_BUNDLE_BYTES {
        return Err(Status::Limit);
    }
    bytes.extend_from_slice(&(proof.len() as u32).to_le_bytes());
    bytes.extend_from_slice(proof);
    let signature: [u8; 64] = bundle.authorization().binding_signature().into();
    bytes.extend_from_slice(&signature);
    if bytes.len() > MAX_BUNDLE_BYTES {
        return Err(Status::Limit);
    }
    Ok(bytes)
}
fn prove(
    plan: &mut WalletPlan,
    digest: &[u8; 32],
    expected_effect: &[u8; 32],
    required_balance: i64,
) -> Result<Vec<u8>, Status> {
    if plan.facts.effect != *expected_effect {
        return Err(Status::Format);
    }
    if plan.facts.value_balance != required_balance {
        return Err(Status::BalanceMismatch);
    }
    // One proof at a time, one dedicated two-worker budget, not nested pools.
    // The wallet service still needs a bounded/cancellable request queue.
    let _guard = PROVER_MUTEX.lock().map_err(|_| Status::Panic)?;
    let pool = PROVER_POOL
        .get_or_init(|| {
            rayon::ThreadPoolBuilder::new()
                .num_threads(2)
                .thread_name(|i| format!("dinero-orchard-prover-{i}"))
                .build()
                .map_err(|_| Status::Panic)
        })
        .as_ref()
        .map_err(|e| *e)?;
    let bundle = plan.bundle.take().ok_or(Status::Format)?;
    let secret = plan.spending_key.take();
    let authorities = match &secret {
        Some(raw) => {
            let sk = Option::<SpendingKey>::from(SpendingKey::from_bytes(**raw))
                .ok_or(Status::Encoding)?;
            vec![SpendAuthorizingKey::from(&sk)]
        }
        None => Vec::new(),
    };
    let complete = pool.install(|| {
        let key = PROVING_KEY.get_or_init(|| ProvingKey::build(BUNDLE_VERSION.circuit_version()));
        bundle
            .create_proof(key, &mut OsRng)
            .map_err(|_| Status::Proof)?
            .apply_signatures(OsRng, *digest, &authorities)
            .map_err(|_| Status::SpendSignature)
    })?;
    let bytes = encode(&complete)?;
    let parsed = ParsedBundle::decode(&bytes)?;
    let mut expected = plan.facts;
    expected.authorization = parsed.facts.authorization;
    if parsed.facts != expected {
        return Err(Status::Format);
    }
    parsed.verify(digest, required_balance)?;
    Ok(bytes)
}
/// # Safety
/// Keys is live and immutable; payments points to count aligned immutable
/// entries. Output is aligned writable/non-aliasing and unchanged on failure.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_prepare_shield_v1(
    keys: *const WalletKeys,
    payments: *const Payment,
    count: usize,
    output: *mut *mut WalletPlan,
) -> i32 {
    if count == 0 || count > MAX_ACTIONS {
        return Status::Limit as i32;
    }
    if keys.is_null() || payments.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let plan = Box::new(prepare(unsafe { &*keys }, unsafe {
            std::slice::from_raw_parts(payments, count)
        })?);
        unsafe {
            output.write(Box::into_raw(plan));
        }
        Ok(())
    })
}
/// # Safety
/// Keys and each input note are live and immutable for the call. All arrays
/// are aligned, immutable and non-aliasing with writable output. Payments may
/// be null only at zero count. Anchor is 32 bytes from the selected chain.
/// Failure leaves output unchanged. Paths prove membership, not chain choice.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_prepare_spend_v1(
    keys: *const WalletKeys,
    inputs: *const SpendInput,
    count: usize,
    anchor: *const u8,
    payments: *const Payment,
    payment_count: usize,
    output: *mut *mut WalletPlan,
) -> i32 {
    if count == 0 || count > MAX_ACTIONS || payment_count > MAX_ACTIONS {
        return Status::Limit as i32;
    }
    if keys.is_null()
        || inputs.is_null()
        || anchor.is_null()
        || output.is_null()
        || (payment_count != 0 && payments.is_null())
    {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let inputs = unsafe { std::slice::from_raw_parts(inputs, count) };
        let mut checked = Vec::with_capacity(count);
        for input in inputs {
            if input.note.is_null() {
                return Err(Status::NullArgument);
            }
            checked.push(WitnessedNote {
                note: unsafe { &*input.note },
                position: input.position,
                path: input.path,
            });
        }
        let payments = if payment_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(payments, payment_count) }
        };
        let plan = Box::new(prepare_spend(
            unsafe { &*keys },
            &checked,
            unsafe { *anchor.cast() },
            payments,
        )?);
        unsafe {
            output.write(Box::into_raw(plan));
        }
        Ok(())
    })
}
/// # Safety
/// Plan is live, immutable during this call; output is aligned writable and
/// non-aliasing. Facts describe this plan only, never a verified transaction.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_wallet_plan_facts_v1(
    plan: *const WalletPlan,
    output: *mut BundleFacts,
) -> i32 {
    if plan.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        unsafe {
            output.write((*plan).facts);
        }
        Ok(())
    })
}
/// # Safety
/// Plan is exclusively borrowed, one-use; no concurrent calls. Digest/effect
/// are 32 immutable bytes; output is aligned writable and non-aliasing.
/// Output unchanged on any failure. A proof attempt consumes the inner plan;
/// free the handle once even after a failure. Host derives digest from this
/// plan's facts plus authenticated transaction context, never a raw RPC value.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_prove_wallet_bundle_v1(
    plan: *mut WalletPlan,
    digest: *const u8,
    effect: *const u8,
    balance: i64,
    output: *mut BuiltBundle,
) -> i32 {
    if plan.is_null() || digest.is_null() || effect.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let bytes = prove(
            unsafe { &mut *plan },
            unsafe { &*digest.cast() },
            unsafe { &*effect.cast() },
            balance,
        )?;
        let mut result = BuiltBundle {
            length: bytes.len() as u32,
            bytes: [0; MAX_BUNDLE_BYTES],
        };
        result.bytes[..bytes.len()].copy_from_slice(&bytes);
        unsafe {
            output.write(result);
        }
        Ok(())
    })
}
/// # Safety
/// Null or live owned plan, consumed regardless of status. No concurrent use.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_wallet_plan_free_v1(plan: *mut WalletPlan) -> i32 {
    unsafe { free_owned(plan) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use orchard::{keys::PreparedIncomingViewingKey, note_encryption::OrchardDomain};
    use zcash_note_encryption::try_note_decryption;
    fn payment(keys: &WalletKeys, amount: u64) -> Payment {
        Payment {
            amount,
            recipient: keys
                .viewing()
                .unwrap()
                .address_at(0u32, Scope::External)
                .to_raw_address_bytes(),
            memo: [42; 512],
        }
    }
    #[test]
    fn prepared_outputs_are_fresh_and_decrypt_to_the_requested_payment() {
        let sender = WalletKeys::derive(&[7; 64], 0).unwrap();
        let receiver = WalletKeys::derive(&[9; 64], 0).unwrap();
        let first = prepare(&sender, &[payment(&receiver, 5000)]).unwrap();
        let second = prepare(&sender, &[payment(&receiver, 5000)]).unwrap();
        assert_ne!(first.facts.effect, second.facts.effect);
        assert_eq!(first.facts.value_balance, -5000);
        assert_eq!(
            first.facts.flags,
            Flags::SPENDS_DISABLED.to_byte(BUNDLE_VERSION).unwrap()
        );
        let fvk = receiver.viewing().unwrap();
        let ivk = PreparedIncomingViewingKey::new(&fvk.to_ivk(Scope::External));
        let bundle = first.bundle.as_ref().unwrap();
        let notes: Vec<_> = bundle
            .actions()
            .iter()
            .filter_map(|action| {
                try_note_decryption(&OrchardDomain::for_action(action), &ivk, action)
            })
            .collect();
        assert_eq!(notes.len(), 1);
        assert_eq!(notes[0].0.value().inner(), 5000);
        assert_eq!(
            notes[0].1.to_raw_address_bytes(),
            payment(&receiver, 5000).recipient
        );
        assert_eq!(notes[0].2, [42; 512]);
    }
    #[test]
    fn payment_bounds_and_abi_outputs_fail_closed() {
        assert_eq!(std::mem::size_of::<Payment>(), 568);
        assert_eq!(std::mem::offset_of!(Payment, recipient), 8);
        assert_eq!(std::mem::offset_of!(Payment, memo), 51);
        assert_eq!(std::mem::size_of::<BuiltBundle>(), 65540);
        let keys = WalletKeys::derive(&[7; 64], 0).unwrap();
        assert!(matches!(prepare(&keys, &[]), Err(Status::Limit)));
        assert!(matches!(
            prepare(&keys, &[payment(&keys, 0)]),
            Err(Status::Money)
        ));
        assert!(matches!(
            prepare(&keys, &[payment(&keys, MAX_MONEY), payment(&keys, 1)]),
            Err(Status::Money)
        ));
        let mut invalid = payment(&keys, 10);
        invalid.recipient = [0; 43];
        assert!(matches!(prepare(&keys, &[invalid]), Err(Status::Encoding)));
        let mut plan = std::ptr::null_mut();
        assert_eq!(
            unsafe { dinero_orchard_prepare_shield_v1(&keys, &payment(&keys, 0), 1, &mut plan) },
            Status::Money as i32
        );
        assert!(plan.is_null());
        let mut plan = prepare(&keys, &[payment(&keys, 10)]).unwrap();
        let effect = plan.facts.effect;
        assert!(matches!(
            prove(&mut plan, &[1; 32], &effect, 0),
            Err(Status::BalanceMismatch)
        ));
        assert!(plan.bundle.is_some()); // failure occurred before any proof work.
    }
}
