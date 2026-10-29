# Orchard operation identity and chain observations

`wallet.orchard.listoperations` returns the current authenticated account's retained operations. A `reserved` entry owns its reservation. A `signed` entry additionally exposes `txid`, computed from its retained transaction. These are durable wallet states; a confirmed operation can remain signed until the existing explicit archival transition runs.

Every entry includes `chain_observation`. It is null when that account checkpoint has no recorded outcome. Otherwise it contains `outcome` (`confirmed` or `conflicted`), `height`, `block_hash`, and `transaction_id`. A conflict identifies the transaction that consumed reserved resources; a reserved entry has no signed transaction ID of its own. Confirmation and conflict observations are authenticated against the selected history during the existing full account/catalog restore. The endpoint does not expose signed transaction bytes, key material, or a spendability assertion.

## Interpreting progress

Read `account_sequence`/`account_digest` together with `captured_source_sequence`/`captured_source_digest`. Different values mean that the account checkpoint is behind the captured source; its observation is historical. Matching values describe that captured checkpoint, not a promise that the live source cannot advance after the call. Null means no outcome recorded at that checkpoint; it does not prove that a transaction is absent from a mempool or from newer blocks.

A selected-chain disconnect can remove an observation while preserving the operation's exact signed ID and reservation. Reinclusion records the outcome again. Listing itself does not sign, submit, archive, release reservations, repair storage, or advance recovery. Missing or invalid catalog/account material remains an error with no partial operation list.

## Qualification scope

The component tests exercise real account reservation/signing, selected-block confirmation and conflict, lag, reopen, disconnect and reinclusion, plus read refusal and byte preservation. A separate-process HTTP fixture is prepared to exercise ordinary daemon startup, real proof/admission/mining and provider delivery, process restart, reorg and full reindex. Its preparation is not an end-to-end pass. Broader platform, load and release gates remain separate.


## Ordinary validation-tip ownership

The first actual HTTP diagnostic built the full daemon and passed all 24 selected
component groups, but refused funding block generation before shielding. The
ordinary block commit advanced canonical state and then its pool callback could
not acquire the checked source: `getCanonicalBlockHash` requires the validation
tip, while only the Orchard block writer maintained that record.

The ordinary ConnectTip owner now stages the validation-tip identity in its
existing consensus WriteBatch after successful ConnectBlock. Ordinary
DisconnectTip stages the parent identity in the existing rollback batch after
actual undo. Both stage results are checked. The canonical-source reader and
independent selected-history activation replay keep their existing requirements;
there is no reader fallback or startup fabrication of a missing record. The
record binds the validated transition to canonical state, not global snapshot
provenance or a replacement for independent history verification. CSN writers
and legacy preexisting records are outside this change.

The HTTP fixture additionally generates two ordinary blocks, restarts, invalidates
the second, restarts at its actual parent, and generates the replacement history
before the existing shield/send/unshield sequence. It uses actual RPC calls and
preserves the existing assertions and 900-second deadline. This change requires
fresh qualification; the previous HTTP run did not reach Orchard activity.

## Explicit storage-layout prerequisite found through actual HTTP

The actual ordinary HTTP sequence now passes mining, restart, invalidation, a
second restart and replacement mining to height101. Activation construction then
refused: the newly created ChainDB uses its supported legacy nine-family layout;
Orchard requires the explicitly separated shielded-state layout. Diagnostic
logging preserved every check and identified the selected-state refusal before
independent boundary replay. The actual RocksDB OPTIONS confirms nine families.

The HTTP fixture now stops the isolated original and invokes the existing native
`migrate_shielded_datadir` qualification tool on a separate copied candidate. It
compares original files before/after (excluding native diagnostic LOG and lock
files), requires the real migration result, and reopens the candidate with the
same wallet password, height and tip before activation. It creates no layout
fence or substitute chain state itself. The original remains preserved.

This is an explicit prerequisite in the isolated test, not an implicit daemon
migration or a shipped operator tool. Fresh-node layout initialization, safe
production upgrade/cutover and full reindex remain release gates. This revision
has not yet run; migration and later Orchard cycle outcomes must be verified.
The separately linked migration tool needs its own instrumentation scope; the
HTTP daemon graph alone does not instrument that executable.
