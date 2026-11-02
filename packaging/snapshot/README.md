# Snapshot publication: legacy and Orchard v6

Mainnet format policy is v4 below H111000, v5 from H111000 through H149999,
and v6 at/above H150000. A v6 candidate first qualifies after 288 descendants.
Changing a version byte never constructs a valid state proof.

## Required activation-boundary snapshots

Owner rollout requirement: capture and retain the final pre-Orchard **v5
snapshot at exactly H149999**. Keep its payload, manifest, signature and
checksums separately from the rotating `latest` endpoint and seven-set archive.
Do not let normal retention remove this recovery/bootstrap artifact. No such
artifact is claimed captured or qualified by this source change.

Arrange an explicit boundary capture: a periodic/daily current-tip export may
skip H149999, and an exporter already past that height cannot obtain it merely
by asking dumptxoutset for an older height. Before the boundary, qualify the
publisher and stop its daily timer after its prior candidate has published.
Then run `capture_boundary_v5.py --watch` as a supervised one-shot service,
starting while the selected tip is still below H149999. It polls every second,
holds the same publisher lock during export, and stages `pending.dat/json` only
when the selected tip is exactly H149999. It verifies the RPC result, v5 file
header and selected block hash before and after export. A missed height,
existing unrelated pending candidate, changed selected hash, or postactivation
v6 export refuses without replacing the pending artifact. Keep the watcher log
and its exit status; it is not a publication receipt. The capture height is
fixed at H149999 and the script refuses a non-mainnet RPC endpoint.
After H149999, rerunning the watcher can verify a completed pending capture or
recover metadata from a data-only pending payload, after checking the v5 header
and selected-chain hash. It never starts a new export after that height. The
publisher refuses an unpaired pending payload instead of deleting it. If no
payload survived an interruption, the height was missed: use an independently
captured copy from another selected mainnet host, or fail the boundary gate.
Keep a second supervised capture host ready for that once-only failure case.
If the pending metadata is damaged or the H149999 base leaves the selected
chain, the publisher keeps the native v5 boundary payload for operator review
and leaves the previous `latest` in place. It does not publish a stale base.

After capture, run the publisher only once H150287 is selected and buried.
The publisher's existing native fresh-node gate decides whether to serve it;
for the exact H149999 v5 base, it also installs an immutable copy in
`$PUBROOT/pinned/v5-h149999` before updating `latest`. Archive rotation never
touches this pinned directory. Retain its payload, manifest and signature
even when `latest` later points at a v6 artifact. An existing pin is rechecked
for payload identity, mainnet/legacy profile and signature against the
configured publisher key; key rotation needs an explicit pin migration, not an
automatic overwrite. The v5 base at H149999 must still wait for 288
descendants: earliest publication is H150287. Orchard
activation itself remains H150000; this snapshot does not delay activation or
permit a v5 snapshot with an Orchard-era base. The first possible v6 base is
H150000, earliest publication H150288. Qualify both boundary artifacts against
the selected chain and the matching native loader before using them.

Deploy the publisher/fetcher scripts together with their sibling
`snapshot_transport.py` and a matching Orchard-capable daemon and CLI.
`SNAPSHOT_PROFILE=compact` is the default phone endpoint. The publisher requests
`dumptxoutset <path> compact`: on an Orchard full node this exports a compact
stump/catalog artifact without converting its live owner. Before Orchard, the
shared legacy format is unchanged. `SNAPSHOT_PROFILE=full` exports the ordinary
full artifact. Use distinct PUBROOT endpoints for the two profiles; do not point
phone mirrors at a full-v6 endpoint. The compact endpoint retains the existing
`DATADIR/snapshots/pending.*` path, including the H149999 capture. The full
endpoint uses `DATADIR/snapshots/full/pending.*`; both still share the root
publisher lock so exports and the boundary capture do not race. A profile
mismatch refuses publication and cannot remove the other profile's candidate.

The manifest includes `format_version` and `snapshot_profile` (`legacy` before
v6). Phone publication requires an Ed25519 key, preserving both mobile apps'
existing signature policy. Full chain-bound publication may omit that extra
publisher signature; native selected-chain authentication is always mandatory.

The publisher retains its candidate while waiting for burial, verifies its
RPC/header base identity, signs the exact manifest bytes, and starts a fresh
native node with the matching profile and exact manifest. This disposable gate
explicitly disables legacy Lightning oracles and proof-bridge advertisement.
Its wallet socket uses a separate port (`GATE_WALLET`, default 27000), alongside
the separate `GATE_PORT` and `GATE_RPC` ports, so it can run beside the live node.
The gate checks RPC
AssumeUTXO state/base identity, not legacy-only log text. It rechecks source
ancestry after the gate and only then swaps `latest`. Gate/reorg failures preserve
the previous publication. Native import verifies full state, profile, selected
headers, burial and consensus commitments; transport inspection supplies none
of that authority. Mainnet v6 cannot be live-qualified before activation/burial.

The job remains an external scheduled/systemd operation. Updating repository
source does not deploy it, change a fleet timer, rotate keys, publish a snapshot,
or replace a phone's existing chain database. A fresh snapshot is for fresh
bootstrap, not permission to reset a wallet or an enrolled chain owner.

Tests: `python3 -m unittest discover -s packaging/snapshot -p 'test_*.py'`.
Transition/whole-publisher tests use isolated RPC/native-gate fixtures. They run
real shell, files, hashing and Ed25519 signing. They do not prove native import;
run OrchardSnapshotRuntime, format/section tests and NodeCoreSnapshotConsumers
as well. The latter legacy consumer and the v6 service lifecycle are distinct
claims. Real mobile consumption and activated-chain qualification remain
separate evidence; never call a transfer check a consensus pass.

## Future discovery constraints

Advertisements are untrusted availability hints. Never vote on provider counts
or popularity to establish snapshot validity: Sybil identities supply no trust.
Validate advertised base/format/size against independently verified selected
headers before large downloads; cap attempts, bytes, concurrency and time.

Group state candidates by base hash/format, but group interchangeable bytes only
by matching verified full-file SHA256 and size. A matching advertised digest is
not evidence that a returned range is correct. Cross-provider range resume needs
chunk hashes or a file Merkle root bound to the exact expected file; otherwise
only the final whole-file digest can detect bad ranges, with an explicit bounded
single-provider retry policy. This feature is not implemented here.

Publisher preference may reorder eligible providers; an explicit publisher pin
restricts them. Neither should weaken the mandatory daemon chain-binding checks.
