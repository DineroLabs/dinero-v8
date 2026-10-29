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
by asking dumptxoutset for an older height. Validate the returned/header base
height and hash, not just the requested filename. The v5 base at H149999 must
still wait for 288 descendants: earliest publication is H150287. Orchard
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
phone mirrors at a full-v6 endpoint. A profile mismatch refuses publication.

The manifest includes `format_version` and `snapshot_profile` (`legacy` before
v6). Phone publication requires an Ed25519 key, preserving both mobile apps'
existing signature policy. Full chain-bound publication may omit that extra
publisher signature; native selected-chain authentication is always mandatory.

The publisher retains its candidate while waiting for burial, verifies its
RPC/header base identity, signs the exact manifest bytes, and starts a fresh
native node with the matching profile and exact manifest. The gate checks RPC
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
