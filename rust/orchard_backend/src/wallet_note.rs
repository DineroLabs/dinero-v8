//! Decrypted owned notes. These carry no claim of active-chain inclusion.
use crate::{boundary, free_owned, ParsedBundle, Status};
use orchard::{
    keys::{FullViewingKey, PreparedIncomingViewingKey},
    note_encryption::OrchardDomain,
    Note,
};
use zcash_note_encryption::try_note_decryption;
use zip32::Scope;

pub struct ReceivedNote {
    pub(super) note: Note,
    pub(super) fvk: FullViewingKey,
    memo: [u8; 512],
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NoteFacts {
    amount: u64,
    commitment: [u8; 32],
    nullifier: [u8; 32],
    recipient: [u8; 43],
    memo: [u8; 512],
    reserved: [u8; 5],
}
fn receive(
    bundle: &ParsedBundle,
    fvk: &[u8; 96],
    scope: u8,
    index: usize,
) -> Result<Option<ReceivedNote>, Status> {
    let scope = match scope {
        0 => Scope::External,
        1 => Scope::Internal,
        _ => return Err(Status::Format),
    };
    let fvk = FullViewingKey::from_bytes(fvk).ok_or(Status::Encoding)?;
    let action = bundle.bundle.actions().get(index).ok_or(Status::Limit)?;
    let ivk = PreparedIncomingViewingKey::new(&fvk.to_ivk(scope));
    Ok(
        try_note_decryption(&OrchardDomain::for_action(action), &ivk, action)
            .map(|(note, _, memo)| ReceivedNote { note, fvk, memo }),
    )
}
fn facts(note: &ReceivedNote) -> NoteFacts {
    let cmx: orchard::note::ExtractedNoteCommitment = note.note.commitment().into();
    NoteFacts {
        amount: note.note.value().inner(),
        commitment: cmx.to_bytes(),
        nullifier: note.note.nullifier(&note.fvk).to_bytes(),
        recipient: note.note.recipient().to_raw_address_bytes(),
        memo: note.memo,
        reserved: [0; 5],
    }
}
/// # Safety
/// Immutable live bundle and 96-byte FVK, aligned non-aliasing writable output.
/// Success with null means not owned; failure leaves output unchanged. Host
/// must establish authorization/chain provenance separately. No raw note import.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_receive_note_v1(
    bundle: *const ParsedBundle,
    fvk: *const u8,
    scope: u8,
    index: u32,
    output: *mut *mut ReceivedNote,
) -> i32 {
    if bundle.is_null() || fvk.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let note = receive(
            unsafe { &*bundle },
            unsafe { &*fvk.cast() },
            scope,
            index as usize,
        )?;
        let handle = note
            .map(|n| Box::into_raw(Box::new(n)))
            .unwrap_or(std::ptr::null_mut());
        unsafe {
            output.write(handle);
        }
        Ok(())
    })
}
/// # Safety
/// Live immutable note; aligned non-aliasing writable output. Wallet-private
/// metadata; do not log or expose publicly. Output unchanged on failure.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_note_facts_v1(
    note: *const ReceivedNote,
    output: *mut NoteFacts,
) -> i32 {
    if note.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        unsafe {
            output.write(facts(&*note));
        }
        Ok(())
    })
}
/// # Safety
/// Null or owned live note, consumed regardless of status; no concurrent use.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_note_free_v1(note: *mut ReceivedNote) -> i32 {
    unsafe { free_owned(note) }
}
