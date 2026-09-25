//! Valid synthetic funding and cross-address spend, through unmodified Orchard.
//! All keys/seeds are PUBLIC TEST DATA. No wallet, RPC, network or live chain.
use incrementalmerkletree::Hashable;
use orchard::{
    builder::{Builder, BundleType},
    bundle::{Authorized, BundleVersion, Flags, TxVersion},
    circuit::{ProvingKey, VerifyingKey},
    keys::{FullViewingKey, PreparedIncomingViewingKey, Scope, SpendAuthorizingKey, SpendingKey},
    note::ExtractedNoteCommitment,
    note_encryption::OrchardDomain,
    tree::{MerkleHashOrchard, MerklePath},
    value::NoteValue,
    Bundle,
};
use rand::SeedableRng;
use rand_chacha::ChaCha20Rng;
use sha2::{Digest, Sha256};
use std::path::Path;
use zcash_note_encryption::try_note_decryption;

fn hex<const N: usize>(text: &str) -> [u8; N] {
    assert_eq!(text.len(), 2 * N);
    std::array::from_fn(|i| u8::from_str_radix(&text[2 * i..2 * i + 2], 16).unwrap())
}
fn blob(out: &mut Vec<u8>, bytes: &[u8]) {
    out.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(bytes);
}
// Independent Rust construction of the complete fixed synthetic transaction
// context. Never signs the effects alone. C++ and Python must reproduce this D.
fn intent(effect: [u8; 32], count: usize, balance: i64, input_start: u8) -> [u8; 32] {
    assert!((-5000..=500).contains(&balance));
    let mut out = b"DIN/orchard-v2/tx-sighash/v1\0".to_vec();
    out.push(2);
    out.extend_from_slice(&hex::<32>(
        "6fb72815ae47a082ff3b0f45246c928888c0d00ef43f232c7ef2ab361c000000",
    ));
    for n in [0xa1b2c3d4_u32, 7, 12345, 2] {
        out.extend_from_slice(&n.to_le_bytes());
    }
    let mut taproot = vec![0x51, 32];
    taproot.extend_from_slice(&hex::<32>(
        "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
    ));
    let mut p2wpkh = vec![0, 20];
    p2wpkh.extend_from_slice(&hex::<20>("06afd46bcdfd22ef94ac122aa11f241244a37ecc"));
    for (start, index, sequence, value, script) in [
        (input_start, 3_u32, 0xfffffffd_u32, 12345_u64, &taproot),
        (input_start + 32, 9_u32, 0xfffffffe_u32, 54321_u64, &p2wpkh),
    ] {
        out.extend((0..32).map(|i| start + i));
        out.extend_from_slice(&index.to_le_bytes());
        out.extend_from_slice(&sequence.to_le_bytes());
        out.extend_from_slice(&value.to_le_bytes());
        blob(&mut out, script);
    }
    out.extend_from_slice(&2_u32.to_le_bytes());
    out.extend_from_slice(&10000_u64.to_le_bytes());
    blob(&mut out, &taproot);
    out.extend_from_slice(&((56000 + balance) as u64).to_le_bytes());
    blob(&mut out, &p2wpkh);
    out.push(1);
    out.extend_from_slice(&666_u64.to_le_bytes());
    out.extend_from_slice(&[1, 1, 1]);
    out.extend_from_slice(&(count as u32).to_le_bytes());
    out.extend_from_slice(&effect);
    Sha256::digest(out).into()
}
fn encode(bundle: &Bundle<Authorized, i64>) -> Vec<u8> {
    let mut out = b"DNORCH01".to_vec();
    out.extend_from_slice(&[1, bundle.flag_byte()]);
    out.extend_from_slice(&(bundle.actions().len() as u32).to_le_bytes());
    out.extend_from_slice(&bundle.value_balance().to_le_bytes());
    out.extend_from_slice(&bundle.anchor().to_bytes());
    for action in bundle.actions() {
        out.extend_from_slice(&action.cv_net().to_bytes());
        out.extend_from_slice(&action.nullifier().to_bytes());
        let key: [u8; 32] = action.rk().into();
        out.extend_from_slice(&key);
        out.extend_from_slice(&action.cmx().to_bytes());
        out.extend_from_slice(&action.encrypted_note().epk_bytes);
        out.extend_from_slice(&action.encrypted_note().enc_ciphertext);
        out.extend_from_slice(&action.encrypted_note().out_ciphertext);
        let signature: [u8; 64] = action.authorization().into();
        out.extend_from_slice(&signature);
    }
    let proof = bundle.authorization().proof().as_ref();
    out.extend_from_slice(&(proof.len() as u32).to_le_bytes());
    out.extend_from_slice(proof);
    let binding: [u8; 64] = bundle.authorization().binding_signature().into();
    out.extend_from_slice(&binding);
    out
}
fn save(dir: &Path, name: &str, bundle: &Bundle<Authorized, i64>, digest: [u8; 32]) {
    let vk = VerifyingKey::build(BundleVersion::orchard_v2().circuit_version());
    bundle.verify_proof(&vk).unwrap();
    for action in bundle.actions() {
        action.rk().verify(&digest, action.authorization()).unwrap();
    }
    bundle
        .binding_validating_key()
        .verify(&digest, bundle.authorization().binding_signature())
        .unwrap();
    let effect: [u8; 32] = bundle.commitment(TxVersion::V5).unwrap().into();
    std::fs::write(dir.join(format!("{name}.bundle")), encode(bundle)).unwrap();
    std::fs::write(dir.join(format!("{name}.digest")), digest).unwrap();
    std::fs::write(dir.join(format!("{name}.effect")), effect).unwrap();
    println!(
        "{name}: valid {}-action bundle, balance {}",
        bundle.actions().len(),
        bundle.value_balance()
    );
}
fn main() {
    let dir = std::env::args()
        .nth(1)
        .expect("explicit fixture output directory required");
    let dir = Path::new(&dir);
    std::fs::create_dir_all(dir).unwrap();
    let version = BundleVersion::orchard_v2();
    let pk = ProvingKey::build(version.circuit_version());
    let mut rng = ChaCha20Rng::from_seed([41; 32]);
    let sk = SpendingKey::from_bytes([7; 32]).unwrap();
    let fvk = FullViewingKey::from(&sk);
    let recipient = fvk.address_at(0_u32, Scope::External);
    let mut builder = Builder::new(
        BundleType::DEFAULT,
        version,
        Flags::SPENDS_DISABLED,
        MerkleHashOrchard::empty_root(32.into()).into(),
    )
    .unwrap();
    builder
        .add_output(None, recipient, NoteValue::from_raw(5000), [0; 512])
        .unwrap();
    let (unproven, _) = builder.build::<i64>(&mut rng).unwrap().unwrap();
    assert_eq!(*unproven.value_balance(), -5000);
    let effect: [u8; 32] = unproven.commitment(TxVersion::V5).unwrap().into();
    let d = intent(effect, unproven.actions().len(), -5000, 32);
    let shield = unproven
        .create_proof(&pk, &mut rng)
        .unwrap()
        .apply_signatures(&mut rng, d, &[])
        .unwrap();
    save(dir, "combined-shield", &shield, d);

    let ivk = PreparedIncomingViewingKey::new(&fvk.to_ivk(Scope::External));
    let (index, note) = shield
        .actions()
        .iter()
        .enumerate()
        .find_map(|(i, action)| {
            try_note_decryption(&OrchardDomain::for_action(action), &ivk, action)
                .map(|(note, _, _)| (i, note))
        })
        .unwrap();
    // Include BOTH emitted commitments, including padding, in their real order.
    assert_eq!(shield.actions().len(), 2);
    let cmx: ExtractedNoteCommitment = note.commitment().into();
    let path = MerklePath::from_parts(
        index as u32,
        std::array::from_fn(|level| {
            if level == 0 {
                MerkleHashOrchard::from_cmx(shield.actions()[1 - index].cmx())
            } else {
                MerkleHashOrchard::empty_root((level as u8).into())
            }
        }),
    );
    let anchor = path.root(cmx);
    std::fs::write(dir.join("combined-spend.anchor"), anchor.to_bytes()).unwrap();
    std::fs::write(
        dir.join("combined-spend.funding-index"),
        (index as u32).to_le_bytes(),
    )
    .unwrap();
    std::fs::write(
        dir.join("combined-spend.note-nullifier"),
        note.nullifier(&fvk).to_bytes(),
    )
    .unwrap();
    let destination_key = SpendingKey::from_bytes([9; 32]).unwrap();
    let destination_fvk = FullViewingKey::from(&destination_key);
    let destination = destination_fvk.address_at(0_u32, Scope::External);
    let mut builder = Builder::new(
        BundleType::DEFAULT,
        version,
        version.default_flags(),
        anchor,
    )
    .unwrap();
    builder.add_spend(fvk, note, path).unwrap();
    builder
        .add_output(None, destination, NoteValue::from_raw(4500), [0; 512])
        .unwrap();
    let (unproven, _) = builder.build::<i64>(&mut rng).unwrap().unwrap();
    assert_eq!(*unproven.value_balance(), 500);
    let effect: [u8; 32] = unproven.commitment(TxVersion::V5).unwrap().into();
    let d = intent(effect, unproven.actions().len(), 500, 96);
    let spend = unproven
        .create_proof(&pk, &mut rng)
        .unwrap()
        .apply_signatures(&mut rng, d, &[SpendAuthorizingKey::from(&sk)])
        .unwrap();
    let destination_ivk = PreparedIncomingViewingKey::new(&destination_fvk.to_ivk(Scope::External));
    let received = spend
        .actions()
        .iter()
        .find_map(|action| {
            try_note_decryption(&OrchardDomain::for_action(action), &destination_ivk, action)
        })
        .unwrap();
    assert_eq!(received.0.value().inner(), 4500);
    save(dir, "combined-spend", &spend, d);
}
