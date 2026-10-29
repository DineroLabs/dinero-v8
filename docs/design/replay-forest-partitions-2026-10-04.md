# Partitioned storage for retained forest versions

A retained Utreexo forest version can share its state until a mutation. Copying the complete node and lookup containers on the first mutation makes the amount of work depend on the entire inventory. The internal storage now divides those containers so a mutation copies the affected part.

## Representation and ownership

`ForestPages<T>` stores values in pages of 256 entries. Copies share a directory of pages. The first write detaches that directory and the affected page; subsequent writes to an exclusively owned page update it directly. Shrinking clears the retained page tail so regrowth cannot revive removed values. Logical size publishes after the required allocations succeed.

`ForestMap` and `ForestSet` divide keys into 256 hash partitions. Copies share the partitions. Insert, replacement and erase detach the affected partition before mutation. Reads expose const iterators and references, so callers cannot bypass a later version's detach through a mutable reference.

The existing forest state and root-vector ownership remain in place. Forest mutation sites use explicit setters or key erasure. No lock or synchronization contract changes: sharing distinct versions does not make concurrent mutation of one instance safe. Existing callers must retain their ownership and locking requirements.

## Behavior to preserve

A retained version must keep its values, roots, serialization and rollback state when another version changes. A failed copy must leave both versions readable. Tail reuse must contain default values. The accumulator's serialized format and consensus meaning remain unchanged.

`ReplayForestPartitions` covers page copy counts and tail reuse, hash-partition copy counts and reference inventory, failed copies, and real forest rollback across page boundaries. It has one explicit 180-second CTest registration in both backend configurations. Existing replay and forest component registrations remain separate.

## Limits

Directory detachment still copies page pointers. Hash collisions can concentrate keys in one partition. Predicate erasure examines the inventory; subtree hashing and root rebuilding retain their existing costs. This change supplies no worst-case constant-time or resident-memory guarantee and is not qualification for a particular chain height.

The selected-parent replay limits remain unchanged. Full-history capacity, load, platform, restart/reindex and release qualification remain separate gates. Mainnet activation is not set by this change.
