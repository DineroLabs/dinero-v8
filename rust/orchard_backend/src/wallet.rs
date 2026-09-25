//! Wallet key/receiver profile. No RPC, database, signing or consensus admission.
//! Secret bytes remain in an opaque non-cloneable handle and are wiped on drop.
//! Upstream temporary key objects are not zeroizing; this is not a claim that
//! every compiler/stack copy or the caller's seed has been erased.
use crate::{boundary, free_owned, Status};
use bech32::{primitives::decode::CheckedHrpstring, Bech32m, Hrp};
use orchard::{
    keys::{FullViewingKey, SpendingKey},
    Address,
};
use zeroize::Zeroizing;
use zip32::{AccountId, DiversifierIndex, Scope};

const COIN_TYPE: u32 = 1448;
const ADDRESS_PROFILE: u8 = 1;
const MAX_ADDRESS_CHARS: usize = 90;

pub struct WalletKeys {
    spending_key: Zeroizing<[u8; 32]>,
}
impl WalletKeys {
    pub(super) fn derive(seed: &[u8], account: u32) -> Result<Self, Status> {
        if !(32..=252).contains(&seed.len()) {
            return Err(Status::Limit);
        }
        let account = AccountId::try_from(account).map_err(|_| Status::Limit)?;
        let key =
            SpendingKey::from_zip32_seed(seed, COIN_TYPE, account).map_err(|_| Status::Encoding)?;
        Ok(Self {
            spending_key: Zeroizing::new(*key.to_bytes()),
        })
    }
    pub(super) fn viewing(&self) -> Result<FullViewingKey, Status> {
        let sk = Option::<SpendingKey>::from(SpendingKey::from_bytes(*self.spending_key))
            .ok_or(Status::Encoding)?;
        Ok(FullViewingKey::from(&sk))
    }
}
fn scope(value: u8) -> Result<Scope, Status> {
    match value {
        0 => Ok(Scope::External),
        1 => Ok(Scope::Internal),
        _ => Err(Status::Format),
    }
}
fn hrp(network: u8) -> Result<Hrp, Status> {
    Hrp::parse(match network {
        0 => "dinorch",
        1 => "tdinorch",
        2 => "rdinorch",
        _ => return Err(Status::Format),
    })
    .map_err(|_| Status::Format)
}
fn receiver(bytes: &[u8; 43]) -> Result<Address, Status> {
    Option::from(Address::from_raw_address_bytes(bytes)).ok_or(Status::Encoding)
}
fn encode_address(raw: &[u8; 43], network: u8) -> Result<String, Status> {
    let _ = receiver(raw)?;
    let mut payload = [0u8; 44];
    payload[0] = ADDRESS_PROFILE;
    payload[1..].copy_from_slice(raw);
    bech32::encode::<Bech32m>(hrp(network)?, &payload).map_err(|_| Status::Encoding)
}
fn decode_address(text: &str, network: u8) -> Result<[u8; 43], Status> {
    if text.len() > MAX_ADDRESS_CHARS {
        return Err(Status::Limit);
    }
    let decoded = CheckedHrpstring::new::<Bech32m>(text).map_err(|_| Status::Encoding)?;
    if decoded.hrp() != hrp(network)? {
        return Err(Status::Format);
    }
    let payload: Vec<_> = decoded.byte_iter().collect();
    if payload.len() != 44 || payload[0] != ADDRESS_PROFILE {
        return Err(Status::Format);
    }
    let raw: [u8; 43] = payload[1..].try_into().map_err(|_| Status::Format)?;
    // Exact re-encoding also rejects nonzero padding and additional symbols.
    // All-uppercase Bech32 text is a display variant; mixed case is rejected.
    if encode_address(&raw, network)? != text.to_ascii_lowercase() {
        return Err(Status::Encoding);
    }
    Ok(raw)
}

#[repr(C)]
pub struct AddressText {
    length: u32,
    text: [u8; 96],
}

/// # Safety
/// Seed points to length immutable bytes; output is aligned, writable and
/// non-aliasing. Caller owns seed hygiene/entropy. Output unchanged on failure.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_wallet_keys_v1(
    seed: *const u8,
    length: usize,
    account: u32,
    output: *mut *mut WalletKeys,
) -> i32 {
    if !(32..=252).contains(&length) {
        return Status::Limit as i32;
    }
    if seed.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let seed = unsafe { std::slice::from_raw_parts(seed, length) };
        let keys = Box::new(WalletKeys::derive(seed, account)?);
        unsafe {
            output.write(Box::into_raw(keys));
        }
        Ok(())
    })
}
/// # Safety
/// Handle is live and immutable. Output is 96 writable, non-aliasing bytes.
/// Full viewing keys reveal wallet activity and must be stored as private data.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_wallet_fvk_v1(
    handle: *const WalletKeys,
    output: *mut u8,
) -> i32 {
    if handle.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let bytes = unsafe { &*handle }.viewing()?.to_bytes();
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), output, bytes.len());
        }
        Ok(())
    })
}
/// # Safety
/// FVK is 96 readable bytes, index is 11 readable little-endian bytes, output
/// is 43 writable non-aliasing bytes. Invalid input leaves output unchanged.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_wallet_receiver_v1(
    fvk: *const u8,
    scope_code: u8,
    index: *const u8,
    output: *mut u8,
) -> i32 {
    if fvk.is_null() || index.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let bytes: &[u8; 96] = unsafe { &*fvk.cast() };
        let fvk = FullViewingKey::from_bytes(bytes).ok_or(Status::Encoding)?;
        let index: [u8; 11] = unsafe { *index.cast() };
        let address = fvk
            .address_at(DiversifierIndex::from(index), scope(scope_code)?)
            .to_raw_address_bytes();
        unsafe {
            std::ptr::copy_nonoverlapping(address.as_ptr(), output, address.len());
        }
        Ok(())
    })
}
/// # Safety
/// Raw is 43 readable bytes. Output is aligned writable non-aliasing storage.
/// Success validates receiver encoding, not ownership or recipient intent.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_address_encode_v1(
    raw: *const u8,
    network: u8,
    output: *mut AddressText,
) -> i32 {
    if raw.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let raw: &[u8; 43] = unsafe { &*raw.cast() };
        let encoded = encode_address(raw, network)?;
        if encoded.len() > MAX_ADDRESS_CHARS {
            return Err(Status::Limit);
        }
        let mut result = AddressText {
            length: encoded.len() as u32,
            text: [0; 96],
        };
        result.text[..encoded.len()].copy_from_slice(encoded.as_bytes());
        unsafe {
            output.write(result);
        }
        Ok(())
    })
}
/// # Safety
/// Text is length immutable bytes and output is 43 writable non-aliasing bytes.
/// Expected network is explicit; no auto-detection/default mainnet.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_address_decode_v1(
    text: *const u8,
    length: usize,
    network: u8,
    output: *mut u8,
) -> i32 {
    if length > MAX_ADDRESS_CHARS {
        return Status::Limit as i32;
    }
    if text.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let bytes = unsafe { std::slice::from_raw_parts(text, length) };
        let address = decode_address(
            std::str::from_utf8(bytes).map_err(|_| Status::Encoding)?,
            network,
        )?;
        unsafe {
            std::ptr::copy_nonoverlapping(address.as_ptr(), output, address.len());
        }
        Ok(())
    })
}
/// # Safety
/// Null or a live owned handle; consumed even on panic. No concurrent use/free.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_wallet_free_v1(handle: *mut WalletKeys) -> i32 {
    unsafe { free_owned(handle) }
}
#[no_mangle]
pub extern "C" fn dinero_orchard_wallet_coin_type_v1() -> u32 {
    COIN_TYPE
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn derivation_matches_pinned_zip32_and_watch_only_addresses() {
        let seed = [7; 64];
        let keys = WalletKeys::derive(&seed, 3).unwrap();
        let reference =
            SpendingKey::from_zip32_seed(&seed, 1448, AccountId::try_from(3).unwrap()).unwrap();
        assert_eq!(
            keys.viewing().unwrap().to_bytes(),
            FullViewingKey::from(&reference).to_bytes()
        );
        assert_ne!(
            keys.viewing().unwrap().to_bytes(),
            WalletKeys::derive(&seed, 4)
                .unwrap()
                .viewing()
                .unwrap()
                .to_bytes()
        );
        let fvk = keys.viewing().unwrap();
        let bytes = fvk.to_bytes();
        let mut external = [0x55; 43];
        let mut index = [0; 11];
        index[10] = 1; // Exercise the full 88-bit index.
        assert_eq!(
            unsafe {
                dinero_orchard_wallet_receiver_v1(
                    bytes.as_ptr(),
                    0,
                    index.as_ptr(),
                    external.as_mut_ptr(),
                )
            },
            0
        );
        assert_eq!(
            external,
            fvk.address_at(DiversifierIndex::from(index), Scope::External)
                .to_raw_address_bytes()
        );
        assert_ne!(
            external,
            fvk.address_at(DiversifierIndex::from(index), Scope::Internal)
                .to_raw_address_bytes()
        );
        let saved = external;
        assert_eq!(
            unsafe {
                dinero_orchard_wallet_receiver_v1(
                    bytes.as_ptr(),
                    2,
                    index.as_ptr(),
                    external.as_mut_ptr(),
                )
            },
            Status::Format as i32
        );
        assert_eq!(external, saved);
    }
    #[test]
    fn explicit_network_profile_checksum_and_receiver_are_required() {
        let raw = WalletKeys::derive(&[1; 32], 0)
            .unwrap()
            .viewing()
            .unwrap()
            .address_at(0u32, Scope::External)
            .to_raw_address_bytes();
        for network in 0..3 {
            let text = encode_address(&raw, network).unwrap();
            assert!(text.len() <= 90);
            assert_eq!(decode_address(&text, network).unwrap(), raw);
            assert_eq!(decode_address(&text.to_uppercase(), network).unwrap(), raw);
            assert!(decode_address(&text, (network + 1) % 3).is_err());
            let mut changed = text.clone().into_bytes();
            let last = changed.len() - 1;
            changed[last] = if changed[last] == b'q' { b'p' } else { b'q' };
            assert!(decode_address(std::str::from_utf8(&changed).unwrap(), network).is_err());
            let mut payload = vec![ADDRESS_PROFILE];
            payload.extend_from_slice(&raw);
            let old_checksum =
                bech32::encode::<bech32::Bech32>(hrp(network).unwrap(), &payload).unwrap();
            assert!(decode_address(&old_checksum, network).is_err());
            payload[0] = 2;
            let wrong_profile = bech32::encode::<Bech32m>(hrp(network).unwrap(), &payload).unwrap();
            assert!(decode_address(&wrong_profile, network).is_err());
        }
        assert!(encode_address(&raw, 255).is_err());
        assert!(encode_address(&[0; 43], 0).is_err());
        assert!(WalletKeys::derive(&[0; 31], 0).is_err());
        assert!(WalletKeys::derive(&[0; 253], 0).is_err());
        assert!(WalletKeys::derive(&[0; 32], 1 << 31).is_err());
    }
    #[test]
    fn ffi_failures_never_publish_partial_outputs() {
        let mut keys = std::ptr::null_mut();
        assert_eq!(
            unsafe { dinero_orchard_wallet_keys_v1([1; 32].as_ptr(), 32, 1 << 31, &mut keys) },
            Status::Limit as i32
        );
        assert!(keys.is_null());
        let mut output = [0x55; 43];
        assert_ne!(
            unsafe { dinero_orchard_address_decode_v1(b"bad".as_ptr(), 3, 0, output.as_mut_ptr()) },
            0
        );
        assert_eq!(output, [0x55; 43]);
        assert_eq!(
            unsafe { dinero_orchard_wallet_free_v1(std::ptr::null_mut()) },
            0
        );
    }
}
