//! Immutable copies of upstream incrementally updatable note witnesses.
use crate::{boundary, free_owned, frontier, Reader, Status, MAX_ACTIONS};
use incrementalmerkletree::{frontier::CommitmentTree, witness::IncrementalWitness};
use orchard::tree::MerkleHashOrchard;
type Witness = IncrementalWitness<MerkleHashOrchard, 32>;
pub struct WalletWitness {
    witness: Witness,
    commitment: [u8; 32],
}
#[repr(C)]
pub struct WitnessFacts {
    leaf_count: u64,
    position: u32,
    reserved: u32,
    root: [u8; 32],
    commitment: [u8; 32],
    path: [[u8; 32]; 32],
}
fn node(raw: &[u8; 32]) -> Result<MerkleHashOrchard, Status> {
    Option::from(MerkleHashOrchard::from_bytes(raw)).ok_or(Status::Encoding)
}
fn create(frontier: &[u8], leaves: &[[u8; 32]], index: usize) -> Result<WalletWitness, Status> {
    if leaves.is_empty() || leaves.len() > MAX_ACTIONS || index >= leaves.len() {
        return Err(Status::Limit);
    }
    let mut tree = CommitmentTree::from_frontier(&frontier::decode(frontier)?);
    for raw in &leaves[..=index] {
        tree.append(node(raw)?).map_err(|_| Status::Limit)?;
    }
    let mut witness = Witness::from_tree(tree).ok_or(Status::Format)?;
    for raw in &leaves[index + 1..] {
        witness.append(node(raw)?).map_err(|_| Status::Limit)?;
    }
    Ok(WalletWitness {
        witness,
        commitment: leaves[index],
    })
}
fn append(
    previous: &WalletWitness,
    leaves: &[[u8; 32]],
    parent: &[u8; 32],
    next: &[u8; 32],
) -> Result<WalletWitness, Status> {
    if leaves.len() > MAX_ACTIONS {
        return Err(Status::Limit);
    }
    if previous.witness.root().to_bytes() != *parent {
        return Err(Status::Format);
    }
    let mut witness = previous.witness.clone();
    for raw in leaves {
        witness.append(node(raw)?).map_err(|_| Status::Limit)?;
    }
    if witness.root().to_bytes() != *next {
        return Err(Status::Format);
    }
    Ok(WalletWitness {
        witness,
        commitment: previous.commitment,
    })
}
fn facts(witness: &WalletWitness) -> Result<WitnessFacts, Status> {
    let path = witness.witness.path().ok_or(Status::Format)?;
    let position: u64 = path.position().into();
    let tip: u64 = witness.witness.tip_position().into();
    if position > u32::MAX as u64 || tip > u32::MAX as u64 {
        return Err(Status::Limit);
    }
    let path = path
        .path_elems()
        .iter()
        .map(MerkleHashOrchard::to_bytes)
        .collect::<Vec<_>>()
        .try_into()
        .map_err(|_| Status::Format)?;
    Ok(WitnessFacts {
        leaf_count: tip + 1,
        position: position as u32,
        reserved: 0,
        root: witness.witness.root().to_bytes(),
        commitment: witness.commitment,
        path,
    })
}
const WITNESS_MAGIC: [u8; 8] = *b"DNORWI01";
const MAX_WITNESS_BYTES: usize = 4096;
#[repr(C)]
pub struct StoredWitness {
    length: u32,
    bytes: [u8; MAX_WITNESS_BYTES],
}
fn encode(witness: &WalletWitness) -> Result<Vec<u8>, Status> {
    let state = facts(witness)?;
    let mut bytes = WITNESS_MAGIC.to_vec();
    bytes.extend_from_slice(&witness.commitment);
    bytes.extend_from_slice(&state.root);
    bytes.extend_from_slice(&state.leaf_count.to_le_bytes());
    let tree = frontier::encoded_bytes(&witness.witness.tree().to_frontier());
    bytes.extend_from_slice(&(tree.len() as u32).to_le_bytes());
    bytes.extend_from_slice(&tree);
    if witness.witness.filled().len() > 32 {
        return Err(Status::Limit);
    }
    bytes.push(witness.witness.filled().len() as u8);
    for node in witness.witness.filled() {
        bytes.extend_from_slice(&node.to_bytes());
    }
    let cursor = witness
        .witness
        .cursor()
        .as_ref()
        .map(|tree| frontier::encoded_bytes(&tree.to_frontier()))
        .unwrap_or_default();
    bytes.extend_from_slice(&(cursor.len() as u32).to_le_bytes());
    bytes.extend_from_slice(&cursor);
    if bytes.len() > MAX_WITNESS_BYTES {
        return Err(Status::Limit);
    }
    Ok(bytes)
}
fn decode(bytes: &[u8]) -> Result<WalletWitness, Status> {
    if bytes.len() > MAX_WITNESS_BYTES {
        return Err(Status::Limit);
    }
    let mut reader = Reader::new(bytes);
    if reader.array::<8>()? != WITNESS_MAGIC {
        return Err(Status::Format);
    }
    let commitment = reader.array()?;
    let root: [u8; 32] = reader.array()?;
    let leaf_count = u64::from_le_bytes(reader.array()?);
    let length = reader.u32()? as usize;
    if length > 1073 {
        return Err(Status::Limit);
    }
    let tree = CommitmentTree::from_frontier(&frontier::decode(reader.take(length)?)?);
    if tree.is_empty() || tree.leaf().map(MerkleHashOrchard::to_bytes) != Some(commitment) {
        return Err(Status::Format);
    }
    // A filled hash represents a COMPLETE future sibling subtree. A cursor
    // must be a nonempty, INCOMPLETE next subtree. Enforce this before calling
    // upstream from_parts, whose legacy decoder is intentionally permissive.
    let position = incrementalmerkletree::Position::from(tree.size() as u64 - 1);
    let future: Vec<_> = position
        .witness_addrs(32.into())
        .filter_map(|(a, source)| {
            (source == incrementalmerkletree::Source::Future).then_some(u8::from(a.level()))
        })
        .collect();
    let count = reader.byte()? as usize;
    if count > future.len() {
        return Err(Status::Format);
    }
    let mut filled = Vec::with_capacity(count);
    for _ in 0..count {
        filled.push(node(&reader.array()?)?);
    }
    let length = reader.u32()? as usize;
    if length > 1073 {
        return Err(Status::Limit);
    }
    let cursor = if length == 0 {
        None
    } else {
        let cursor = CommitmentTree::from_frontier(&frontier::decode(reader.take(length)?)?);
        let depth = *future.get(count).ok_or(Status::Format)?;
        if cursor.is_empty() || cursor.size() as u64 >= (1u64 << depth) {
            return Err(Status::Format);
        }
        Some(cursor)
    };
    reader.finish()?;
    let witness = Witness::from_parts(tree, filled, cursor).ok_or(Status::Format)?;
    let result = WalletWitness {
        witness,
        commitment,
    };
    let state = facts(&result)?;
    if state.root != root || state.leaf_count != leaf_count || encode(&result)? != bytes {
        return Err(Status::Format);
    }
    Ok(result)
}
/// # Safety
/// Live immutable witness, aligned writable non-aliasing output. Output is
/// unchanged on failure. Encoded witness links a wallet note to chain state;
/// persist under wallet privacy protections, never as a consensus certificate.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_witness_encode_v1(
    witness: *const WalletWitness,
    output: *mut StoredWitness,
) -> i32 {
    if witness.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let bytes = encode(unsafe { &*witness })?;
        let mut result = StoredWitness {
            length: bytes.len() as u32,
            bytes: [0; MAX_WITNESS_BYTES],
        };
        result.bytes[..bytes.len()].copy_from_slice(&bytes);
        unsafe {
            output.write(result);
        }
        Ok(())
    })
}
/// # Safety
/// Immutable bounded bytes, 32-byte commitment/root and selected tree count.
/// Aligned writable non-aliasing output, unchanged on failure. Host must bind
/// the supplied expected values to its authenticated wallet/chain checkpoint.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_witness_decode_v1(
    bytes: *const u8,
    length: usize,
    commitment: *const u8,
    root: *const u8,
    leaf_count: u64,
    output: *mut *mut WalletWitness,
) -> i32 {
    if length > MAX_WITNESS_BYTES {
        return Status::Limit as i32;
    }
    if bytes.is_null() || commitment.is_null() || root.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let witness = decode(unsafe { std::slice::from_raw_parts(bytes, length) })?;
        let state = facts(&witness)?;
        let expected_commitment: &[u8; 32] = unsafe { &*commitment.cast() };
        let expected_root: &[u8; 32] = unsafe { &*root.cast() };
        if witness.commitment != *expected_commitment
            || state.root != *expected_root
            || state.leaf_count != leaf_count
        {
            return Err(Status::Format);
        }
        unsafe {
            output.write(Box::into_raw(Box::new(witness)));
        }
        Ok(())
    })
}

/// # Safety
/// Immutable bounded frontier encoding and count consecutive 32-byte leaves.
/// Aligned writable non-aliasing output, unchanged on failure. Host supplies
/// authenticated ordered chain data; this establishes only tree membership.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_witness_create_v1(
    frontier: *const u8,
    length: usize,
    leaves: *const u8,
    count: usize,
    index: usize,
    output: *mut *mut WalletWitness,
) -> i32 {
    if count == 0 || count > MAX_ACTIONS || index >= count || length > 1073 {
        return Status::Limit as i32;
    }
    if frontier.is_null() || leaves.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let result = Box::new(create(
            unsafe { std::slice::from_raw_parts(frontier, length) },
            unsafe { std::slice::from_raw_parts(leaves.cast(), count) },
            index,
        )?);
        unsafe {
            output.write(Box::into_raw(result));
        }
        Ok(())
    })
}
/// # Safety
/// Live immutable witness and count consecutive 32-byte leaves, null only for
/// zero count. Parent/next are 32 bytes. Output non-aliasing, unchanged on error.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_witness_append_v1(
    witness: *const WalletWitness,
    leaves: *const u8,
    count: usize,
    parent: *const u8,
    next: *const u8,
    output: *mut *mut WalletWitness,
) -> i32 {
    if count > MAX_ACTIONS {
        return Status::Limit as i32;
    }
    if witness.is_null()
        || (count != 0 && leaves.is_null())
        || parent.is_null()
        || next.is_null()
        || output.is_null()
    {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let leaves = if count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(leaves.cast(), count) }
        };
        let result = Box::new(append(
            unsafe { &*witness },
            leaves,
            unsafe { &*parent.cast() },
            unsafe { &*next.cast() },
        )?);
        unsafe {
            output.write(Box::into_raw(result));
        }
        Ok(())
    })
}
/// # Safety
/// Live immutable witness and writable aligned non-aliasing output. Unchanged
/// on failure. These facts establish neither chain choice nor spendability.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_witness_facts_v1(
    witness: *const WalletWitness,
    output: *mut WitnessFacts,
) -> i32 {
    if witness.is_null() || output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let result = facts(unsafe { &*witness })?;
        unsafe {
            output.write(result);
        }
        Ok(())
    })
}
/// # Safety
/// Null or owned live witness, consumed on any status; no concurrent use.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_witness_free_v1(witness: *mut WalletWitness) -> i32 {
    unsafe { free_owned(witness) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use incrementalmerkletree::frontier::Frontier;
    fn leaf(n: u32) -> [u8; 32] {
        let mut b = [0; 32];
        b[..4].copy_from_slice(&n.to_le_bytes());
        b
    }
    #[test]
    fn incremental_paths_follow_committed_frontiers_without_mutating_parents() {
        let mut empty = b"DNORFR01".to_vec();
        empty.extend_from_slice(&0u64.to_le_bytes());
        let leaves = [leaf(1), leaf(2), leaf(3), leaf(4)];
        let mut tree = Frontier::<MerkleHashOrchard, 32>::empty();
        for raw in leaves {
            assert!(tree.append(node(&raw).unwrap()));
        }
        let mut witnesses: Vec<_> = (0..4)
            .map(|i| create(&empty, &leaves, i).unwrap())
            .collect();
        for start in (5..=253).step_by(8) {
            let parent = tree.root().to_bytes();
            let leaves: Vec<_> = (start..start + 8).map(leaf).collect();
            for raw in &leaves {
                assert!(tree.append(node(raw).unwrap()));
            }
            for witness in &mut witnesses {
                let before = facts(witness).unwrap();
                let next = append(witness, &leaves, &parent, &tree.root().to_bytes()).unwrap();
                assert_eq!(facts(witness).unwrap().root, before.root);
                assert_eq!(next.witness.root(), tree.root());
                let path = next.witness.path().unwrap();
                assert_eq!(path.root(node(&witness.commitment).unwrap()), tree.root());
                assert_eq!(facts(&next).unwrap().leaf_count, tree.tree_size());
                let bytes = encode(&next).unwrap();
                let restored = decode(&bytes).unwrap();
                assert_eq!(encode(&restored).unwrap(), bytes);
                assert_eq!(facts(&restored).unwrap().path, facts(&next).unwrap().path);
                *witness = restored;
            }
        }
        let w = &witnesses[0];
        let root = w.witness.root().to_bytes();
        assert!(matches!(
            append(w, &[leaf(500)], &[0; 32], &root),
            Err(Status::Format)
        ));
        assert!(matches!(
            append(w, &[[255; 32]], &root, &root),
            Err(Status::Encoding)
        ));
        assert_eq!(w.witness.root().to_bytes(), root);
        assert_eq!(std::mem::size_of::<WitnessFacts>(), 1104);
        assert_eq!(std::mem::offset_of!(WitnessFacts, path), 80);
    }
    #[test]
    fn stored_witness_is_bounded_canonical_and_checkpoint_bound() {
        let mut empty = b"DNORFR01".to_vec();
        empty.extend_from_slice(&0u64.to_le_bytes());
        let witness = create(&empty, &[leaf(1), leaf(2), leaf(3)], 0).unwrap();
        let bytes = encode(&witness).unwrap();
        for n in 0..bytes.len() {
            assert!(decode(&bytes[..n]).is_err());
        }
        let mut trailing = bytes.clone();
        trailing.push(0);
        assert!(matches!(decode(&trailing), Err(Status::TrailingBytes)));
        for at in [0, 8, 40, 72] {
            let mut changed = bytes.clone();
            changed[at] ^= 1;
            assert!(decode(&changed).is_err());
        }
        let mut too_many = bytes.clone();
        too_many[80..84].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(matches!(decode(&too_many), Err(Status::Limit)));
        let state = facts(&witness).unwrap();
        let mut handle = std::ptr::null_mut();
        assert_eq!(
            unsafe {
                dinero_orchard_witness_decode_v1(
                    bytes.as_ptr(),
                    bytes.len(),
                    state.commitment.as_ptr(),
                    state.root.as_ptr(),
                    state.leaf_count + 1,
                    &mut handle,
                )
            },
            Status::Format as i32
        );
        assert!(handle.is_null());
        assert_eq!(std::mem::size_of::<StoredWitness>(), 4100);
        // Last possible leaf: no future sibling or cursor remains legal.
        let almost = Frontier::<MerkleHashOrchard, 32>::from_parts(
            incrementalmerkletree::Position::from((1u64 << 32) - 2),
            node(&leaf(9)).unwrap(),
            vec![node(&leaf(7)).unwrap(); 31],
        )
        .unwrap();
        let full = create(&frontier::encoded_bytes(&almost), &[leaf(10)], 0).unwrap();
        assert_eq!(facts(&full).unwrap().leaf_count, 1u64 << 32);
        let restored = decode(&encode(&full).unwrap()).unwrap();
        assert_eq!(facts(&restored).unwrap().position, u32::MAX);
        let root = full.witness.root().to_bytes();
        assert!(append(&full, &[leaf(11)], &root, &root).is_err());
    }
}
