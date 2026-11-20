//! Zcash v5 transparent transactions for DIN <-> ZEC swaps, over librustzcash.
//!
//! The C ABI is in `include/dinero_zcash_swap.h`. This crate serializes
//! transactions, computes their ZIP 244 txid and SIGHASH_ALL, and encodes
//! t-addresses. It holds no keys: C++ builds the HTLC scripts and signs.

use std::panic::{catch_unwind, AssertUnwindSafe};

use transparent::address::Script;
use transparent::bundle::{self as tbundle, OutPoint, TxIn, TxOut};
use transparent::sighash::{SighashType, TransparentAuthorizingContext};
use zcash_address::{ToAddress, TryFromAddress, ZcashAddress};
use zcash_primitives::transaction::sighash::{PrecomputedSighashData, SignableInput};
use zcash_primitives::transaction::{Authorization, Transaction, TransactionData, TxVersion};
use zcash_protocol::consensus::{BlockHeight, BranchId, NetworkType};
use zcash_protocol::value::Zatoshis;
use zcash_script::script;

use zcash_transparent as transparent;

pub const DZS_OK: i32 = 0;
pub const DZS_ERR_ARG: i32 = 1;
pub const DZS_ERR_BRANCH: i32 = 2;
pub const DZS_ERR_PARSE: i32 = 3;
pub const DZS_ERR_BUFFER: i32 = 4;
pub const DZS_ERR_INDEX: i32 = 5;
pub const DZS_ERR_ADDRESS: i32 = 6;
pub const DZS_ERR_PANIC: i32 = 7;
pub const DZS_ERR_SCRIPT_INVALID: i32 = 8;

pub const DZS_NET_MAIN: u8 = 0;
pub const DZS_NET_TEST: u8 = 1;
pub const DZS_NET_REGTEST: u8 = 2;
pub const DZS_ADDR_P2PKH: u8 = 0;
pub const DZS_ADDR_P2SH: u8 = 1;

// Zcash consensus: a nonzero nExpiryHeight must be below this.
const TX_EXPIRY_HEIGHT_THRESHOLD: u32 = 500_000_000;

#[repr(C)]
pub struct DzsTxIn {
    pub prev_txid: [u8; 32],
    pub prev_index: u32,
    pub sequence: u32,
    pub script_sig: *const u8,
    pub script_sig_len: usize,
}

#[repr(C)]
pub struct DzsTxOut {
    pub value_zat: u64,
    pub script_pubkey: *const u8,
    pub script_pubkey_len: usize,
}

type Res<T> = Result<T, i32>;

/// Runs `f`, turning a panic into DZS_ERR_PANIC so it never unwinds into C++.
fn guard(f: impl FnOnce() -> Res<()>) -> i32 {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => DZS_OK,
        Ok(Err(code)) => code,
        Err(_) => DZS_ERR_PANIC,
    }
}

/// # Safety
/// `ptr` must point to `len` readable items, or `len` must be 0.
unsafe fn slice<'a, T>(ptr: *const T, len: usize) -> Res<&'a [T]> {
    if len == 0 {
        return Ok(&[]);
    }
    if ptr.is_null() {
        return Err(DZS_ERR_ARG);
    }
    Ok(std::slice::from_raw_parts(ptr, len))
}

fn script_of(bytes: &[u8]) -> Script {
    Script(script::Code(bytes.to_vec()))
}

fn v5_branch(id: u32) -> Res<BranchId> {
    let branch = BranchId::try_from(id).map_err(|_| DZS_ERR_BRANCH)?;
    if !TxVersion::V5.valid_in_branch(branch) {
        return Err(DZS_ERR_BRANCH);
    }
    Ok(branch)
}

/// Reads exactly one v5 transaction from `bytes`.
fn read_v5(bytes: &[u8]) -> Res<Transaction> {
    let mut cursor = bytes;
    // The branch id argument is used only for v4 and earlier; a v5
    // transaction carries its own in the header.
    let tx = Transaction::read(&mut cursor, BranchId::Nu5).map_err(|_| DZS_ERR_PARSE)?;
    if !cursor.is_empty() || tx.version() != TxVersion::V5 {
        return Err(DZS_ERR_PARSE);
    }
    Ok(tx)
}

fn write_out(bytes: &[u8], out: *mut u8, out_cap: usize, out_len: *mut usize) -> Res<()> {
    if out_len.is_null() {
        return Err(DZS_ERR_ARG);
    }
    unsafe { *out_len = bytes.len() };
    if out_cap < bytes.len() || (out.is_null() && !bytes.is_empty()) {
        return Err(DZS_ERR_BUFFER);
    }
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len()) };
    Ok(())
}

fn write_32(bytes: &[u8; 32], out: *mut u8) -> Res<()> {
    if out.is_null() {
        return Err(DZS_ERR_ARG);
    }
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, 32) };
    Ok(())
}

/// # Safety
/// Pointers must be valid for the given lengths; see the C header.
#[no_mangle]
pub unsafe extern "C" fn dzs_tx_build(
    consensus_branch_id: u32,
    lock_time: u32,
    expiry_height: u32,
    vin: *const DzsTxIn,
    vin_len: usize,
    vout: *const DzsTxOut,
    vout_len: usize,
    out: *mut u8,
    out_cap: usize,
    out_len: *mut usize,
) -> i32 {
    guard(|| {
        let branch = v5_branch(consensus_branch_id)?;
        if expiry_height >= TX_EXPIRY_HEIGHT_THRESHOLD {
            return Err(DZS_ERR_ARG);
        }
        let vin = slice(vin, vin_len)?;
        let vout = slice(vout, vout_len)?;
        if vin.is_empty() || vout.is_empty() {
            return Err(DZS_ERR_ARG);
        }
        let mut inputs = Vec::with_capacity(vin.len());
        for i in vin {
            let sig = slice(i.script_sig, i.script_sig_len)?;
            inputs.push(TxIn::<tbundle::Authorized>::from_parts(
                OutPoint::new(i.prev_txid, i.prev_index),
                script_of(sig),
                i.sequence,
            ));
        }
        let mut outputs = Vec::with_capacity(vout.len());
        for o in vout {
            let value = Zatoshis::from_u64(o.value_zat).map_err(|_| DZS_ERR_ARG)?;
            outputs.push(TxOut::new(value, script_of(slice(o.script_pubkey, o.script_pubkey_len)?)));
        }
        let data = TransactionData::<zcash_primitives::transaction::Authorized>::from_parts(
            TxVersion::V5,
            branch,
            lock_time,
            BlockHeight::from_u32(expiry_height),
            Some(tbundle::Bundle { vin: inputs, vout: outputs, authorization: tbundle::Authorized }),
            None,
            None,
            None,
        );
        let tx = data.freeze().map_err(|_| DZS_ERR_ARG)?;
        let mut bytes = Vec::new();
        tx.write(&mut bytes).map_err(|_| DZS_ERR_ARG)?;
        write_out(&bytes, out, out_cap, out_len)
    })
}

/// # Safety
/// `tx` must point to `tx_len` bytes and `out` to 32 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn dzs_txid(tx: *const u8, tx_len: usize, out: *mut u8) -> i32 {
    guard(|| {
        let tx = read_v5(slice(tx, tx_len)?)?;
        write_32(tx.txid().as_ref(), out)
    })
}

/// The spent outputs, which ZIP 244 needs for every transparent input.
#[derive(Debug)]
struct Prevouts {
    amounts: Vec<Zatoshis>,
    scripts: Vec<Script>,
}

impl tbundle::Authorization for Prevouts {
    type ScriptSig = Script;
}

impl TransparentAuthorizingContext for Prevouts {
    fn input_amounts(&self) -> Vec<Zatoshis> {
        self.amounts.clone()
    }
    fn input_scriptpubkeys(&self) -> Vec<Script> {
        self.scripts.clone()
    }
}

struct WithPrevouts;

impl Authorization for WithPrevouts {
    type TransparentAuth = Prevouts;
    type SaplingAuth = sapling::bundle::Authorized;
    type OrchardAuth = orchard::bundle::Authorized;
}

/// The ZIP 244 SIGHASH_ALL of transparent input `index` of `tx`.
fn sighash_all(tx: Transaction, index: usize, prevouts: &[DzsTxOut]) -> Res<[u8; 32]> {
    let mut amounts = Vec::with_capacity(prevouts.len());
    let mut scripts = Vec::with_capacity(prevouts.len());
    for p in prevouts {
        amounts.push(Zatoshis::from_u64(p.value_zat).map_err(|_| DZS_ERR_ARG)?);
        scripts.push(script_of(unsafe { slice(p.script_pubkey, p.script_pubkey_len)? }));
    }
    let n_in = tx.transparent_bundle().map_or(0, |b| b.vin.len());
    if prevouts.len() != n_in || index >= n_in {
        return Err(DZS_ERR_INDEX);
    }
    let data = tx.into_data().map_bundles::<WithPrevouts>(
        |t| {
            t.map(|b| tbundle::Bundle {
                vin: b
                    .vin
                    .iter()
                    .map(|i| TxIn::from_parts(i.prevout().clone(), i.script_sig().clone(), i.sequence()))
                    .collect(),
                vout: b.vout,
                authorization: Prevouts { amounts, scripts },
            })
        },
        |s| s,
        |o| o,
    );
    let precomputed = PrecomputedSighashData::new_zip244(data).map_err(|_| DZS_ERR_PARSE)?;
    let bundle = precomputed.tx().transparent_bundle().ok_or(DZS_ERR_INDEX)?;
    let auth = &bundle.authorization;
    // ZIP 244 hashes the spent output's scriptPubKey; v5 has no separate script code.
    let input = transparent::sighash::SignableInput::from_parts(
        bundle,
        SighashType::ALL,
        index,
        &auth.scripts[index],
        &auth.scripts[index],
        auth.amounts[index],
    )
    .map_err(|_| DZS_ERR_INDEX)?;
    Ok(*precomputed.signature_hash(&SignableInput::Transparent(input)).as_ref())
}

/// # Safety
/// Pointers must be valid for the given lengths; `out` must hold 32 bytes.
#[no_mangle]
pub unsafe extern "C" fn dzs_sighash_all(
    tx: *const u8,
    tx_len: usize,
    input_index: usize,
    prevouts: *const DzsTxOut,
    prevouts_len: usize,
    out: *mut u8,
) -> i32 {
    guard(|| {
        let tx = read_v5(slice(tx, tx_len)?)?;
        let hash = sighash_all(tx, input_index, slice(prevouts, prevouts_len)?)?;
        write_32(&hash, out)
    })
}

/// # Safety
/// `hash` must point to 20 bytes; `out` to `out_cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn dzs_taddr_encode(
    network: u8,
    kind: u8,
    hash: *const u8,
    out: *mut c_char_t,
    out_cap: usize,
    out_len: *mut usize,
) -> i32 {
    guard(|| {
        let net = match network {
            DZS_NET_MAIN => NetworkType::Main,
            DZS_NET_TEST => NetworkType::Test,
            DZS_NET_REGTEST => NetworkType::Regtest,
            _ => return Err(DZS_ERR_ARG),
        };
        let data: [u8; 20] = slice(hash, 20)?.try_into().map_err(|_| DZS_ERR_ARG)?;
        let addr = match kind {
            DZS_ADDR_P2PKH => ZcashAddress::from_transparent_p2pkh(net, data),
            DZS_ADDR_P2SH => ZcashAddress::from_transparent_p2sh(net, data),
            _ => return Err(DZS_ERR_ARG),
        };
        write_out(addr.encode().as_bytes(), out as *mut u8, out_cap, out_len)
    })
}

#[allow(non_camel_case_types)]
pub type c_char_t = std::os::raw::c_char;

/// A decoded transparent address: network, kind, hash.
struct TAddr(u8, u8, [u8; 20]);

fn net_code(net: NetworkType) -> u8 {
    match net {
        NetworkType::Main => DZS_NET_MAIN,
        NetworkType::Test => DZS_NET_TEST,
        NetworkType::Regtest => DZS_NET_REGTEST,
    }
}

impl TryFromAddress for TAddr {
    type Error = ();

    fn try_from_transparent_p2pkh(
        net: NetworkType,
        data: [u8; 20],
    ) -> Result<Self, zcash_address::ConversionError<()>> {
        Ok(TAddr(net_code(net), DZS_ADDR_P2PKH, data))
    }

    fn try_from_transparent_p2sh(
        net: NetworkType,
        data: [u8; 20],
    ) -> Result<Self, zcash_address::ConversionError<()>> {
        Ok(TAddr(net_code(net), DZS_ADDR_P2SH, data))
    }
}

/// # Safety
/// `s` must point to `s_len` bytes; the out pointers must be writable
/// (`hash` for 20 bytes).
#[no_mangle]
pub unsafe extern "C" fn dzs_taddr_decode(
    s: *const c_char_t,
    s_len: usize,
    network: *mut u8,
    kind: *mut u8,
    hash: *mut u8,
) -> i32 {
    guard(|| {
        if network.is_null() || kind.is_null() || hash.is_null() {
            return Err(DZS_ERR_ARG);
        }
        let text = std::str::from_utf8(slice(s as *const u8, s_len)?).map_err(|_| DZS_ERR_ADDRESS)?;
        let addr = ZcashAddress::try_from_encoded(text).map_err(|_| DZS_ERR_ADDRESS)?;
        let TAddr(n, k, data) = addr.convert::<TAddr>().map_err(|_| DZS_ERR_ADDRESS)?;
        *network = n;
        *kind = k;
        std::ptr::copy_nonoverlapping(data.as_ptr(), hash, 20);
        Ok(())
    })
}

#[cfg(feature = "script-verify")]
mod verify {
    use super::*;
    use zcash_script::interpreter::{CallbackTransactionSignatureChecker, Flags};
    use zcash_script::signature::HashType;

    // zcashd STANDARD_SCRIPT_VERIFY_FLAGS: what a relaying node checks.
    fn standard_flags() -> Flags {
        Flags::P2SH
            | Flags::StrictEnc
            | Flags::MinimalData
            | Flags::NullDummy
            | Flags::DiscourageUpgradableNOPs
            | Flags::CleanStack
            | Flags::CHECKLOCKTIMEVERIFY
            | Flags::LowS
    }

    /// # Safety
    /// Pointers must be valid for the given lengths.
    #[no_mangle]
    pub unsafe extern "C" fn dzs_verify_input(
        tx: *const u8,
        tx_len: usize,
        input_index: usize,
        prevouts: *const DzsTxOut,
        prevouts_len: usize,
    ) -> i32 {
        guard(|| {
            let bytes = slice(tx, tx_len)?;
            let prevouts = slice(prevouts, prevouts_len)?;
            let parsed = read_v5(bytes)?;
            let (script_sig, sequence) = {
                let b = parsed.transparent_bundle().ok_or(DZS_ERR_INDEX)?;
                let i = b.vin.get(input_index).ok_or(DZS_ERR_INDEX)?;
                (i.script_sig().0 .0.clone(), i.sequence())
            };
            let lock_time = parsed.lock_time();
            let sighash = sighash_all(parsed, input_index, prevouts)?;
            let spk = slice(prevouts[input_index].script_pubkey, prevouts[input_index].script_pubkey_len)?;
            let calc = |_: &script::Code, hash_type: &HashType| {
                (hash_type.raw_bits() == i32::from(SighashType::ALL.encode())).then_some(sighash)
            };
            let checker = CallbackTransactionSignatureChecker {
                sighash: &calc,
                lock_time: i64::from(lock_time),
                is_final: sequence == u32::MAX,
            };
            match script::Raw::from_raw_parts(script_sig, spk.to_vec()).eval(standard_flags(), &checker) {
                Ok(true) => Ok(()),
                _ => Err(DZS_ERR_SCRIPT_INVALID),
            }
        })
    }
}
