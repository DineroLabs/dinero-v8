//! Private, versioned recovery of the original randomized construction.
//! Not an authorization, encrypted envelope, chain certificate, or RPC input.
//! No spending key is serialized. Import requires the wallet's live key owner.
use super::*;
use rand::{CryptoRng, RngCore};
use std::panic::{catch_unwind, AssertUnwindSafe};

// Both profiles use the current Orchard engine and unchanged circuit/wire rules.
// Profile 1 retains the original rand0.8 construction shuffle for saved plans;
// profile 2 uses the upstream current shuffle for newly recorded plans.
const MAGIC: &[u8; 8] = b"DNOPLN02";
const LEGACY_MAGIC: &[u8; 8] = b"DNOPLN01";
pub(super) const MAX_PLAN_BYTES: usize = 1024 * 1024;
const MAX_RANDOM_BYTES: usize = 64 * 1024;

pub(super) struct BuildRng {
    bytes: Zeroizing<Vec<u8>>,
    cursor: Option<usize>,
    legacy_profile1: bool,
}
impl BuildRng {
    pub(super) fn is_profile1(&self) -> bool { self.legacy_profile1 }
    pub(super) fn record() -> Self {
        Self {
            bytes: Zeroizing::new(Vec::new()),
            cursor: None,
            legacy_profile1: false,
        }
    }
    fn replay(bytes: &[u8]) -> Result<Self, Status> {
        if bytes.len() > MAX_RANDOM_BYTES {
            return Err(Status::Limit);
        }
        Ok(Self {
            bytes: Zeroizing::new(bytes.to_vec()),
            cursor: Some(0),
            legacy_profile1: false,
        })
    }
    fn draw(&mut self, tag: u8, out: &mut [u8]) -> Result<(), rand::Error> {
        let error = || rand::Error::new("Orchard construction randomness unavailable");
        if let Some(cursor) = self.cursor {
            let end = cursor
                .checked_add(5)
                .and_then(|n| n.checked_add(out.len()))
                .ok_or_else(error)?;
            if end > self.bytes.len()
                || self.bytes[cursor] != tag
                || u32::from_le_bytes(self.bytes[cursor + 1..cursor + 5].try_into().unwrap())
                    as usize
                    != out.len()
            {
                return Err(error());
            }
            out.copy_from_slice(&self.bytes[cursor + 5..end]);
            self.cursor = Some(end);
        } else {
            let size = self
                .bytes
                .len()
                .checked_add(5)
                .and_then(|n| n.checked_add(out.len()))
                .ok_or_else(error)?;
            if size > MAX_RANDOM_BYTES {
                return Err(error());
            }
            OsRng.try_fill_bytes(out)?;
            self.bytes.push(tag);
            self.bytes
                .extend_from_slice(&(out.len() as u32).to_le_bytes());
            self.bytes.extend_from_slice(out);
        }
        Ok(())
    }
    pub(super) fn finish(self) -> Result<Zeroizing<Vec<u8>>, Status> {
        if self.cursor.is_some_and(|n| n != self.bytes.len()) {
            return Err(Status::TrailingBytes);
        }
        Ok(self.bytes)
    }
}
impl RngCore for BuildRng {
    fn next_u32(&mut self) -> u32 {
        let mut b = [0; 4];
        self.draw(1, &mut b)
            .expect("Orchard construction RNG failed");
        u32::from_le_bytes(b)
    }
    fn next_u64(&mut self) -> u64 {
        let mut b = [0; 8];
        self.draw(2, &mut b)
            .expect("Orchard construction RNG failed");
        u64::from_le_bytes(b)
    }
    fn fill_bytes(&mut self, out: &mut [u8]) {
        self.draw(3, out).expect("Orchard construction RNG failed");
    }
    fn try_fill_bytes(&mut self, out: &mut [u8]) -> Result<(), rand::Error> {
        self.draw(4, out)
    }
}
impl CryptoRng for BuildRng {}
// Private migration candidate: retain the original transcript tags and refusal
// checks while adapting only the new rand trait surface. Exact old replay is
// still required; no production compatibility claim is made by this adapter.
impl rand_next::TryRng for BuildRng {
    type Error = core::convert::Infallible;
    fn try_next_u32(&mut self) -> Result<u32, Self::Error> {
        Ok(rand::RngCore::next_u32(self))
    }
    fn try_next_u64(&mut self) -> Result<u64, Self::Error> {
        Ok(rand::RngCore::next_u64(self))
    }
    fn try_fill_bytes(&mut self, out: &mut [u8]) -> Result<(), Self::Error> {
        rand::RngCore::fill_bytes(self, out);
        Ok(())
    }
}
impl rand_next::TryCryptoRng for BuildRng {}


fn number(bytes: &mut Vec<u8>, n: usize) -> Result<(), Status> {
    bytes.extend_from_slice(&u32::try_from(n).map_err(|_| Status::Limit)?.to_le_bytes());
    Ok(())
}
fn sized(bytes: &mut Vec<u8>, value: &[u8]) -> Result<(), Status> {
    number(bytes, value.len())?;
    bytes.extend_from_slice(value);
    Ok(())
}
pub(super) fn prefix(
    keys: &WalletKeys,
    inputs: &[WitnessedNote<'_>],
    anchor: [u8; 32],
    payments: &[Payment],
    legacy_profile1: bool,
) -> Result<Zeroizing<Vec<u8>>, Status> {
    if inputs.len() > MAX_ACTIONS || payments.len() > MAX_ACTIONS {
        return Err(Status::Limit);
    }
    let mut bytes = Zeroizing::new(if legacy_profile1 { LEGACY_MAGIC } else { MAGIC }.to_vec());
    bytes.extend_from_slice(&keys.viewing()?.to_bytes());
    number(&mut bytes, inputs.len())?;
    bytes.extend_from_slice(&anchor);
    for input in inputs {
        // Replay authenticated received-note provenance through actual decryption;
        // do not create a raw Note from unverified component fields.
        let source = &input.note.recovery;
        sized(&mut bytes, &source.encoded)?;
        bytes.push(source.scope);
        bytes.extend_from_slice(&source.index.to_le_bytes());
        bytes.extend_from_slice(&input.position.to_le_bytes());
        for node in input.path {
            bytes.extend_from_slice(&node);
        }
    }
    number(&mut bytes, payments.len())?;
    for p in payments {
        bytes.extend_from_slice(&p.amount.to_le_bytes());
        bytes.extend_from_slice(&p.recipient);
        bytes.extend_from_slice(&p.memo);
    }
    if bytes.len() > MAX_PLAN_BYTES {
        return Err(Status::Limit);
    }
    Ok(bytes)
}
fn fact_bytes(f: &BundleFacts) -> Vec<u8> {
    let mut b = Vec::new();
    b.extend_from_slice(&f.effect);
    b.extend_from_slice(&f.authorization);
    b.extend_from_slice(&f.anchor);
    b.extend_from_slice(&f.value_balance.to_le_bytes());
    b.extend_from_slice(&f.action_count.to_le_bytes());
    b.push(f.flags);
    b.extend_from_slice(&f.reserved);
    for n in f.nullifiers {
        b.extend_from_slice(&n);
    }
    for c in f.commitments {
        b.extend_from_slice(&c);
    }
    b
}
pub(super) fn finish(
    mut bytes: Zeroizing<Vec<u8>>,
    facts: &BundleFacts,
    random: Zeroizing<Vec<u8>>,
) -> Result<Zeroizing<Vec<u8>>, Status> {
    sized(&mut bytes, &random)?;
    bytes.extend_from_slice(&fact_bytes(facts));
    if bytes.len() > MAX_PLAN_BYTES {
        return Err(Status::Limit);
    }
    Ok(bytes)
}
struct Reader<'a> {
    bytes: &'a [u8],
    offset: usize,
}
impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], Status> {
        let end = self.offset.checked_add(n).ok_or(Status::Limit)?;
        let out = self.bytes.get(self.offset..end).ok_or(Status::Truncated)?;
        self.offset = end;
        Ok(out)
    }
    fn array<const N: usize>(&mut self) -> Result<[u8; N], Status> {
        Ok(self.take(N)?.try_into().unwrap())
    }
    fn u32(&mut self) -> Result<u32, Status> {
        Ok(u32::from_le_bytes(self.array()?))
    }
    fn sized(&mut self, max: usize) -> Result<&'a [u8], Status> {
        let n = self.u32()? as usize;
        if n > max {
            return Err(Status::Limit);
        }
        self.take(n)
    }
}
// Caller must first authenticate the whole capsule inside its original durable
// reservation and revalidate intent/domain/inputs before publishing a proof job.
// Exact reconstructed bytes bind every fact, field, draw, order and boundary.
// A short/incompatible random transcript cannot fall back to fresh entropy.
pub(super) fn restore(keys: &WalletKeys, bytes: &[u8]) -> Result<WalletPlan, Status> {
    if bytes.len() > MAX_PLAN_BYTES {
        return Err(Status::Limit);
    }
    catch_unwind(AssertUnwindSafe(|| restore_inner(keys, bytes))).map_err(|_| Status::Format)?
}
fn restore_inner(keys: &WalletKeys, bytes: &[u8]) -> Result<WalletPlan, Status> {
    let mut r = Reader { bytes, offset: 0 };
    let profile = r.take(8)?;
    let legacy_profile1 = match profile {
        p if p == LEGACY_MAGIC => true,
        p if p == MAGIC => false,
        _ => return Err(Status::Encoding),
    };
    if r.array::<96>()? != keys.viewing()?.to_bytes() {
        return Err(Status::Encoding);
    }
    let count = r.u32()? as usize;
    if count > MAX_ACTIONS {
        return Err(Status::Limit);
    }
    let anchor = r.array::<32>()?;
    let mut owned = Vec::with_capacity(count);
    for _ in 0..count {
        let bundle = ParsedBundle::decode(r.sized(MAX_BUNDLE_BYTES)?)?;
        let scope = r.array::<1>()?[0];
        let index = r.u32()?;
        let note = crate::wallet_note::receive(
            &bundle,
            &keys.viewing()?.to_bytes(),
            scope,
            index as usize,
        )?
        .ok_or(Status::Encoding)?;
        let position = r.u32()?;
        let mut path = [[0; 32]; 32];
        for node in &mut path {
            *node = r.array()?;
        }
        owned.push((note, position, path));
    }
    let count = r.u32()? as usize;
    if count > MAX_ACTIONS {
        return Err(Status::Limit);
    }
    let mut payments = Zeroizing::new(Vec::with_capacity(count));
    for _ in 0..count {
        payments.push(Payment {
            amount: u64::from_le_bytes(r.array()?),
            recipient: r.array()?,
            memo: r.array()?,
        });
    }
    let mut random = BuildRng::replay(r.sized(MAX_RANDOM_BYTES)?)?;
    random.legacy_profile1 = legacy_profile1;
    // The remainder is checked against facts generated from the replayed bundle.
    let inputs: Vec<_> = owned
        .iter()
        .map(|(note, position, path)| WitnessedNote {
            note,
            position: *position,
            path: *path,
        })
        .collect();
    let plan = if inputs.is_empty() {
        if anchor != [0; 32] {
            return Err(Status::Encoding);
        }
        prepare_with_rng(keys, &payments, random)?
    } else {
        prepare_spend_with_rng(keys, &inputs, anchor, &payments, random)?
    };
    if plan.recovery.as_deref().map(|v| v.as_slice()) != Some(bytes) {
        return Err(Status::Encoding);
    }
    Ok(plan)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn payment(keys: &WalletKeys, amount: u64) -> Payment {
        Payment {
            amount,
            recipient: keys
                .viewing()
                .unwrap()
                .address_at(0u32, Scope::External)
                .to_raw_address_bytes(),
            memo: [19; 512],
        }
    }
    #[test]
    fn shield_recovery_keeps_original_effects_without_new_construction_entropy() {
        let keys = WalletKeys::derive(&[7; 64], 0).unwrap();
        let recipient = WalletKeys::derive(&[9; 64], 0).unwrap();
        let original = prepare(&keys, &[payment(&recipient, 5000)]).unwrap();
        let facts = original.facts;
        let saved = original.recovery.as_ref().unwrap().clone();
        drop(original);
        let restored = restore(&keys, &saved).unwrap();
        assert!(restored.facts == facts);
        assert!(restored.recovery.as_ref().unwrap().as_slice() == saved.as_slice());
        assert!(
            prepare(&keys, &[payment(&recipient, 5000)])
                .unwrap()
                .facts
                .effect
                != facts.effect
        );
        assert!(restore(&WalletKeys::derive(&[8; 64], 0).unwrap(), &saved).is_err());
    }
    #[test]
    fn malformed_or_incompatible_capsules_refuse() {
        let keys = WalletKeys::derive(&[7; 64], 0).unwrap();
        let plan = prepare(&keys, &[payment(&keys, 5000)]).unwrap();
        let saved = plan.recovery.as_ref().unwrap();
        for n in [0, 7, 103, saved.len() - 1] {
            assert!(restore(&keys, &saved[..n]).is_err());
        }
        for offset in [0, 8, 104, 108, saved.len() - 1] {
            let mut wrong = saved.clone();
            wrong[offset] ^= 1;
            assert!(restore(&keys, &wrong).is_err());
        }
        let mut extra = saved.clone();
        extra.push(0);
        assert!(restore(&keys, &extra).is_err());
        assert!(restore(&keys, &vec![0; MAX_PLAN_BYTES + 1]).is_err());
        assert!(restore(&keys, saved).is_ok());
    }
    #[test]
    fn construction_randomness_requires_exact_draws_and_complete_consumption() {
        let mut recorder = BuildRng::record();
        let expected = recorder.next_u32();
        let tape = recorder.finish().unwrap();
        let mut replay = BuildRng::replay(&tape).unwrap();
        assert!(replay.next_u32() == expected);
        assert!(replay.finish().is_ok());
        assert!(BuildRng::replay(&tape).unwrap().finish().is_err());
        let mut wrong = BuildRng::replay(&tape).unwrap();
        assert!(wrong.try_fill_bytes(&mut [0; 4]).is_err());
        let mut empty = BuildRng::replay(&[]).unwrap();
        assert!(empty.try_fill_bytes(&mut [0; 4]).is_err());
        assert!(BuildRng::replay(&vec![0; MAX_RANDOM_BYTES + 1]).is_err());
    }
    #[test]
    fn spend_recovery_redecrypts_original_note_and_proves_same_intent() {
        let keys = WalletKeys::derive(&[7; 64], 0).unwrap();
        let mut shield = prepare(&keys, &[payment(&keys, 5000)]).unwrap();
        let effect = shield.facts.effect;
        let bytes = prove(&mut shield, &[3; 32], &effect, -5000).unwrap();
        assert!(shield.recovery.is_none());
        let parsed = ParsedBundle::decode(&bytes).unwrap();
        parsed.verify(&[3; 32], -5000).unwrap();
        let note = (0..parsed.bundle.actions().len())
            .find_map(|i| {
                crate::wallet_note::receive(&parsed, &keys.viewing().unwrap().to_bytes(), 0, i)
                    .unwrap()
            })
            .unwrap();
        let nodes: [MerkleHashOrchard; 32] =
            std::array::from_fn(|i| MerkleHashOrchard::empty_root((i as u8).into()));
        let anchor = MerklePath::from_parts(0, nodes)
            .root(note.note.commitment().into())
            .to_bytes();
        let path = nodes.map(|n| n.to_bytes());
        let plan = prepare_spend(
            &keys,
            &[WitnessedNote {
                note: &note,
                position: 0,
                path,
            }],
            anchor,
            &[payment(&keys, 4000)],
        )
        .unwrap();
        let facts = plan.facts;
        let saved = plan.recovery.as_ref().unwrap().clone();
        drop(plan);
        drop(note);
        drop(parsed);
        let mut restored = restore(&keys, &saved).unwrap();
        assert!(restored.facts == facts);
        let signed = prove(&mut restored, &[4; 32], &facts.effect, 1000).unwrap();
        ParsedBundle::decode(&signed)
            .unwrap()
            .verify(&[4; 32], 1000)
            .unwrap();
        assert!(restored.recovery.is_none() && restored.bundle.is_none());
        let mut corrupt = saved.clone();
        // Exact original note ciphertext/proof provenance is required.
        let source_start = 8 + 96 + 4 + 32 + 4;
        corrupt[source_start] ^= 1;
        assert!(restore(&keys, &corrupt).is_err());
    }
}
