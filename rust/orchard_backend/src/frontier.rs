//! Bounded, canonical storage codec around the pinned upstream Orchard tree.
//! A decoded frontier is not proof that its history belongs to the active chain.
use crate::{boundary, Reader, Status, MAX_ACTIONS};
use incrementalmerkletree::{frontier::Frontier, Position};
use orchard::tree::MerkleHashOrchard;

type Tree = Frontier<MerkleHashOrchard, 32>;
const MAGIC: [u8; 8] = *b"DNORFR01";
const CAPACITY: u64 = 1u64 << 32;
const MAX_BYTES: usize = 8 + 8 + 32 + 1 + 32 * 32;

#[repr(C)]
pub struct FrontierResult {
    root: [u8; 32],
    leaf_count: u64,
    encoded_length: u32,
    encoded: [u8; MAX_BYTES],
}

fn decode(bytes: &[u8]) -> Result<Tree, Status> {
    if bytes.len() > MAX_BYTES {
        return Err(Status::Limit);
    }
    let mut r = Reader::new(bytes);
    if r.array::<8>()? != MAGIC {
        return Err(Status::Format);
    }
    let size = u64::from_le_bytes(r.array()?);
    if size > CAPACITY {
        return Err(Status::Limit);
    }
    let tree = if size == 0 {
        Tree::empty()
    } else {
        let leaf =
            Option::from(MerkleHashOrchard::from_bytes(&r.array()?)).ok_or(Status::Encoding)?;
        let count = r.byte()? as usize;
        if count > 32 || count != (size - 1).count_ones() as usize {
            return Err(Status::Format);
        }
        let mut ommers = Vec::with_capacity(count);
        for _ in 0..count {
            ommers.push(
                Option::from(MerkleHashOrchard::from_bytes(&r.array()?)).ok_or(Status::Encoding)?,
            );
        }
        Tree::from_parts(Position::from(size - 1), leaf, ommers).map_err(|_| Status::Format)?
    };
    r.finish()?;
    Ok(tree)
}

fn encode(tree: &Tree) -> FrontierResult {
    let mut bytes = Vec::with_capacity(MAX_BYTES);
    bytes.extend_from_slice(&MAGIC);
    bytes.extend_from_slice(&tree.tree_size().to_le_bytes());
    if let Some(f) = tree.value() {
        bytes.extend_from_slice(&f.leaf().to_bytes());
        bytes.push(f.ommers().len() as u8);
        for node in f.ommers() {
            bytes.extend_from_slice(&node.to_bytes());
        }
    }
    let mut result = FrontierResult {
        root: tree.root().to_bytes(),
        leaf_count: tree.tree_size(),
        encoded_length: bytes.len() as u32,
        encoded: [0; MAX_BYTES],
    };
    result.encoded[..bytes.len()].copy_from_slice(&bytes);
    result
}

/// # Safety
/// Output is aligned writable storage, unchanged on failure.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_frontier_empty_v1(output: *mut FrontierResult) -> i32 {
    if output.is_null() {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let result = encode(&Tree::empty());
        unsafe {
            output.write(result);
        }
        Ok(())
    })
}

/// # Safety
/// Bytes and commitments are immutable, valid, and non-aliasing with aligned
/// writable output. Commitments point to count consecutive 32-byte encodings;
/// null is allowed only for count zero. Count zero validates/re-exports storage.
/// No input is mutated; output is unchanged on every failure, including panic.
/// This does not verify proofs, chain provenance, anchors or nullifiers.
#[no_mangle]
pub unsafe extern "C" fn dinero_orchard_frontier_append_v1(
    bytes: *const u8,
    length: usize,
    commitments: *const u8,
    count: usize,
    output: *mut FrontierResult,
) -> i32 {
    if length > MAX_BYTES || count > MAX_ACTIONS {
        return Status::Limit as i32;
    }
    if bytes.is_null() || output.is_null() || (count != 0 && commitments.is_null()) {
        return Status::NullArgument as i32;
    }
    boundary(|| {
        let mut tree = decode(unsafe { std::slice::from_raw_parts(bytes, length) })?;
        for i in 0..count {
            let raw: [u8; 32] = unsafe { std::slice::from_raw_parts(commitments.add(i * 32), 32) }
                .try_into()
                .map_err(|_| Status::Encoding)?;
            let node = Option::from(MerkleHashOrchard::from_bytes(&raw)).ok_or(Status::Encoding)?;
            if !tree.append(node) {
                return Err(Status::Limit);
            }
        }
        let result = encode(&tree);
        unsafe {
            output.write(result);
        }
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use incrementalmerkletree::{Hashable, Level};
    fn node(n: u8) -> MerkleHashOrchard {
        let mut b = [0; 32];
        b[0] = n;
        MerkleHashOrchard::from_bytes(&b).unwrap()
    }
    // A full level-by-level tree, independent of frontier carry/ommer logic.
    fn reference_root(leaves: &[MerkleHashOrchard]) -> [u8; 32] {
        let mut level = leaves.to_vec();
        for depth in 0..32 {
            if !level.len().is_multiple_of(2) {
                level.push(MerkleHashOrchard::empty_root(Level::from(depth)));
            }
            level = level
                .chunks_exact(2)
                .map(|p| MerkleHashOrchard::combine(Level::from(depth), &p[0], &p[1]))
                .collect();
        }
        level[0].to_bytes()
    }
    #[test]
    fn frontier_roundtrip_matches_independent_tree_walk() {
        let mut t = Tree::empty();
        let mut leaves = Vec::new();
        assert_eq!(encode(&t).root, orchard::Anchor::empty_tree().to_bytes());
        // Upstream orchard0.15.5 test_vectors/commitment_tree.rs depth32 root.
        assert_eq!(
            encode(&t).root,
            [
                0xae, 0x29, 0x35, 0xf1, 0xdf, 0xd8, 0xa2, 0x4a, 0xed, 0x7c, 0x70, 0xdf, 0x7d, 0xe3,
                0xa6, 0x68, 0xeb, 0x7a, 0x49, 0xb1, 0x31, 0x98, 0x80, 0xdd, 0xe2, 0xbb, 0xd9, 0x03,
                0x1a, 0xe5, 0xd8, 0x2f
            ]
        );
        for n in 1..=70 {
            leaves.push(node(n));
            assert!(t.append(node(n)));
            let r = encode(&t);
            assert_eq!(r.root, reference_root(&leaves));
            t = decode(&r.encoded[..r.encoded_length as usize]).unwrap();
            assert_eq!(t.tree_size(), n as u64);
        }
    }
    #[test]
    fn frontier_codec_rejects_noncanonical_and_capacity_errors() {
        let mut t = Tree::empty();
        assert!(t.append(node(1)));
        assert!(t.append(node(2)));
        let r = encode(&t);
        let bytes = &r.encoded[..r.encoded_length as usize];
        for n in 0..bytes.len() {
            assert!(decode(&bytes[..n]).is_err());
        }
        let mut bad = bytes.to_vec();
        bad.push(0);
        assert!(decode(&bad).is_err());
        bad = bytes.to_vec();
        bad[0] ^= 1;
        assert!(decode(&bad).is_err());
        bad = bytes.to_vec();
        bad[48] = 0;
        assert!(decode(&bad).is_err());
        bad = bytes.to_vec();
        bad[16..48].fill(255);
        assert!(decode(&bad).is_err());
        bad = bytes.to_vec();
        bad[49..81].fill(255);
        assert!(decode(&bad).is_err());
        bad = bytes.to_vec();
        bad[8..16].copy_from_slice(&(CAPACITY + 1).to_le_bytes());
        assert!(decode(&bad).is_err());
        let mut full =
            Tree::from_parts(Position::from(CAPACITY - 1), node(1), vec![node(2); 32]).unwrap();
        let saved = encode(&full);
        assert_eq!(saved.encoded_length as usize, MAX_BYTES);
        assert!(decode(&saved.encoded).is_ok());
        assert!(!full.append(node(3)));
        assert_eq!(encode(&full).encoded, saved.encoded);
    }
    #[test]
    fn frontier_ffi_failure_is_atomic_and_layout_is_fixed() {
        assert_eq!(std::mem::offset_of!(FrontierResult, leaf_count), 32);
        assert_eq!(std::mem::offset_of!(FrontierResult, encoded_length), 40);
        assert_eq!(std::mem::offset_of!(FrontierResult, encoded), 44);
        assert_eq!(std::mem::size_of::<FrontierResult>(), 1120);
        let input = encode(&Tree::empty());
        let bytes = &input.encoded[..16];
        let mut output = encode(&Tree::empty());
        output.root.fill(0x55);
        let mut commitments = [0u8; 64];
        commitments[32..].fill(255);
        assert_eq!(
            unsafe {
                dinero_orchard_frontier_append_v1(
                    bytes.as_ptr(),
                    bytes.len(),
                    commitments.as_ptr(),
                    2,
                    &mut output,
                )
            },
            Status::Encoding as i32
        );
        assert_eq!(output.root, [0x55; 32]);
        assert_eq!(output.leaf_count, 0);
        assert_eq!(
            unsafe {
                dinero_orchard_frontier_append_v1(
                    bytes.as_ptr(),
                    bytes.len(),
                    std::ptr::null(),
                    9,
                    &mut output,
                )
            },
            Status::Limit as i32
        );
        assert_eq!(
            unsafe {
                dinero_orchard_frontier_append_v1(
                    bytes.as_ptr(),
                    bytes.len(),
                    std::ptr::null(),
                    1,
                    &mut output,
                )
            },
            Status::NullArgument as i32
        );
    }
}
