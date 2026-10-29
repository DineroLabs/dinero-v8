# Present PQ owners during encrypted unlock

Encrypted unlock now opens an existing separate PQ database read-only and captures all present records for the current wallet in one checked statement. The reader returns encrypted seed fields together with typed metadata, checks terminal completion and refuses borrowed transactions. It never returns a partial inventory.

While holding the actual main-wallet lifecycle lease, unlock uses the staged authenticated master to open every captured seed, derive its ML-DSA public key, verify the single-leaf commitment and recorded address commitment, and compare the exact recorded watch script and path. Failure precedes main-wallet commit and live publication, including failed re-unlock. Temporary master, seed and secret-key buffers are cleansed. No key, script, path, label, account or cursor is generated or rewritten.

Existing APIs also accept caller-selected address prefixes. Unlock preserves those recorded addresses and verifies the decoded commitment; it does not reinterpret their network or relabel them.

Imported seeds may have arbitrary caller-provided paths. A recorded HD-looking path is recognition metadata, not proof that the seed derives from that path. Validation preserves these imports. A present nonempty inventory with no authenticated master refuses; missing storage never grants permission to generate a historical master.

The PQ query and main-wallet transaction are separate snapshots. This does not establish atomic cross-database issuance, durable wallet identity binding for old PQ files, deletion/backup completeness, all watch-only owners, authenticated derivation metadata, account archives or lasting readiness. An absent PQ file remains unknown rather than a completeness certificate. The captured read can describe the state before a concurrent writer; a later unlock must validate the later state.

Component cases exercise actual encrypted unlock and reopen, independently imported seed material with unrelated paths, late-row corruption and authenticated wrong-seed substitution, key/address/watch bindings, missing schema/master, interrupted and denied reads, borrowed-transaction refusal, and a separate WAL writer during the active capture. No whole-process crash, network RPC, production lifecycle or release qualification is claimed.
