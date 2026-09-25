//! Immutable copies of upstream incrementally updatable note witnesses.
use crate::{boundary, free_owned, frontier, Status, MAX_ACTIONS};
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
                *witness = next;
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
}
