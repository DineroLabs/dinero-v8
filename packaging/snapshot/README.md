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

The proposed EU1 systemd unit is `dinero-snapshot-boundary-v5.service`. Install
`capture_boundary_v5.py`, `run_boundary_capture_v5.sh`, and the sibling
`snapshot_transport.py` together in `/usr/local/libexec/dinero-snapshot/`;
install the unit in `/etc/systemd/system/` only after checking the live paths,
RPC cookie, and service owner. The template uses root because the existing EU1
publisher owns `snapshots/publisher.lock` as root, while the node's cookie is
readable through that account. It starts after `dinero.service` and retries after
transient RPC/process failures. The wrapper refuses to start if
`dinero-snapshot.timer` is active or its state cannot be read. A missed height
without a saved payload exits 2; systemd does not restart that terminal case.
Start and verify the watcher **below H149999** on EU1 and a second selected-chain
host, with the daily publisher timer held on the capture host. Verify its
journal and `systemctl is-active` state before relying on it. A successful
service exit confirms staging only; inspect `pending.dat/json` and selected
H149999 hash before resuming the publisher after 288 descendants. This unit is
a deployable template, not installed or enabled by repository checkout.

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
The publisher holds a PUBROOT-level `.publisher.lock` through publication, in
addition to the datadir lock, so publishers with distinct datadirs cannot race
on one endpoint. It durably binds the endpoint to its first requested profile
in `.snapshot-profile`, including an initially empty root. The claim persists
if a later native gate fails; reuse a separate empty root for another profile
instead of silently repurposing a claimed endpoint. It checks an existing
`latest` manifest before export and again after the native gate. A v4/v5
manifest without `snapshot_profile` is treated as the existing compact/phone
endpoint, matching the live legacy publication.
A full publisher pointed there, or a compact publisher pointed at a full-v6
endpoint, exits without replacing `latest`. An unreadable or unknown existing
manifest also fails closed. A new full endpoint must start with its own empty
PUBROOT; the native gate and mirror/consumer checks are still required before
advertising it.

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

## Private phone candidate preparation

`prepare_phone_v6.sh` prepares a postactivation compact v6 candidate without
promoting the public phone endpoint. Install it with the publisher and
`snapshot_transport.py` in a separate directory. The accompanying
`dinero-phone-v6-prepare.service` and `.timer` retry every three minutes;
before H150000 the wrapper performs only a tip query. The publisher keeps its
candidate while waiting for 288 descendants and runs its normal native import
gate before writing the private publication.

The compact pending namespace `phone-v6-qualification` is separate from the
root `pending.*` files owned by the H149999 capture. Both use the existing
publisher lock, so exports cannot overlap. The default private root is
`/var/lib/dinero/snapshot-qualification/compact-v6`; the wrapper refuses a
`/var/www` root. No phone endpoint or normal daily timer is changed by this job.

`phone-qualification-required.json` records the exact payload and manifest
digests after the server import gate. It explicitly leaves iOS and Android
qualification false. This receipt is **not publication approval**. Before
promoting those bytes, exercise both updated phone runtimes in disposable
application/datadir namespaces: fleet-signature and payload verification,
native import at the exact base, forward selected-tip/accumulator parity, a
clean stop and cold restart, and refusal of a full-profile or altered artifact.
Record app/Core identities and payload/manifest digests in each receipt. Then
promote the exact qualified generation and verify the signatures and digests
served by all three mirrors. Existing app wallets and chain owners are not
qualification datadirs.

`native_phone_probe.c` supplies an isolated C ABI host for that device work.
On Android compile with `-DPHONE_PROBE_DYNAMIC -ldl` and pass the exact
`libnodecore_ffi.so` extracted from the updated APK as its first argument.
On Apple link the exact NodeCore archive distributed with the updated app and
install the host under a separate development bundle identifier. `version`
records native identity; `home` finds that host's current sandbox (an app
reinstall can relocate it). `run <private-datadir> <config.json> <seconds>`
records bootstrap/base and accumulator RPCs, then calls the orderly native
stop. A second invocation with the same private datadir and no snapshot in
its config exercises cold reopen. Keep the `NCPROBE` records and daemon
output from both runs. Successful START/STOP or a zero launcher exit is not
snapshot qualification: require the exact base and selected-tip accumulator
checks above, with the compact validator role and zero full-forest memory.

## Promote the exact phone-qualified generation

Run `promote_phone_v6.py` on EU1 after both real-device receipts exist. It
recomputes payload and manifest digests against the private staging receipt,
inspects the compact v6 header, cryptographically verifies the raw manifest
signature against the public key pinned in both apps, and rechecks selected
ancestry plus 288 descendants through the local CLI. There is no public-key
override or option to skip phone receipts or mirror verification.

The phone receipt schema is `dinero.phone-snapshot-qualification.v1`, with:

- `state: PASS_NATIVE_COMPACT_V6`, `platform: ios` or `android`, `network:
  mainnet`, `app_version`, and `native_source_sha` equal to the updated apps'
  Core `87ef0f46c5ad90f35b7c83b4a91b3edf7cc679fa`.
- A `native_artifact` object with a receipt-relative `path`, `kind` and
  `sha256`. The binary must be retained alongside the receipt. Promotion
  rehashes it against the fixed platform pin in `CORE_ARTIFACTS`: the exact
  iOS arm64 NodeCore archive used to link the app/probe, or the arm64-v8a
  shared library extracted from the Android release APK. A source-SHA
  attestation or version string alone cannot substitute for those bytes.
- `payload_sha256`, `manifest_sha256`, `base_height`, and `base_hash` matching
  the staging receipt; `forward_height` equal to the last qualified initial
  native accumulator sample and strictly above the base.
- `validation_role: compact_validator`, `retains_full_state: false`,
  `forest_memory_bytes_estimate: 0`, and at least two distinct named
  `independent_observers`.
- A `checks` object with each of `fleet_signature_verified`,
  `payload_hash_verified`, `transport_profile_verified`,
  `native_import_verified`, `forward_selected_tip_and_accumulator_verified`,
  `cold_restart_verified`, `clean_shutdown_verified`,
  `altered_payload_rejected`, and `full_profile_rejected` set to JSON `true`.
- An `evidence` object with `initial_import`, `cold_restart`, `transport`,
  `negative_controls`, and `fleet_agreement` entries. Each names a retained
  receipt-relative file via `path` and its `sha256`. The script rehashes every
  file. Initial/restart logs must contain the actual `NCPROBE` records, clean
  start/stop, exact snapshot base and a forward compact accumulator. Initial
  import must require the manifest; cold restart must omit the configured
  snapshot and must not reimport it. A startup-only log is rejected even if
  every receipt checkbox says true.

The fleet evidence is JSON with `network: mainnet` and a `checkpoints` list.
For each initial/restart sample include `height`, `block_hash`, `commitment`,
and an `observers` list naming at least two of the receipt's independent
observers that returned those same values. These are recorded observations,
not values synthesized from the phone's own response. Keep device identifiers
and raw evidence private; the public generation includes only receipt hashes.

Build identity evidence is recorded in `phone_core_identity_v822.json`.
Although `src/nodecore/CMakeLists.txt` defaults to `DINERO_CLI_GIT_SHA=nodecore`,
the updated app builds force-include recorded version headers overriding that
macro. Both actual physical-device probes returned
`dinerod 8.2.2-87ef0f46c5ad90f35b7c83b4a91b3edf7cc679fa`. The iOS SDK manifest
binds its source and archive digest; the Android native-build receipt binds
its source, and the APK/Google-generated-APK parity record binds the packaged
shared-library bytes. The captured VERSION rows are regression-test inputs,
not strings generated by the version gate. Generic `dinerod nodecore` or mixed
version evidence remains refused for these specific artifacts. No app rebuild
or evidence-log editing is needed. The summary JSON is explicitly mode 0644,
including when promotion runs under umask 0077.

Example invocation (paths below refer to genuine completed qualification):

```sh
python3 /usr/local/libexec/dinero-phone-v6/promote_phone_v6.py \
  --staging-receipt /var/lib/dinero/snapshot-qualification/compact-v6/phone-qualification-required.json \
  --ios-receipt /var/lib/dinero/snapshot-qualification/phones/ios/receipt.json \
  --android-receipt /var/lib/dinero/snapshot-qualification/phones/android/receipt.json \
  --report /var/lib/dinero/snapshot-qualification/compact-v6/promotion.json \
  --dry-run
```

Remove `--dry-run` to promote. The script installs and re-verifies an immutable
archive generation under the shared publication-root lock, rechecks ancestry,
then atomically replaces `latest`. It neither rewrites the v5 pin nor prunes
archives. Dry run writes only the private report. Retrying the same verified
generation is safe; replacing a full-profile endpoint or rolling height back
is refused.

NA/SJ currently pull once daily. After the EU1 swap, trigger their existing
mirror services from the operator host (no new fleet-to-fleet SSH trust):

```sh
ssh dinero-na systemctl start dinero-snapshot-mirror.service
ssh dinero-sj systemctl start dinero-snapshot-mirror.service
```

Promotion waits up to 15 minutes for all three public URLs. Each must serve
the exact manifest bytes, a valid pinned-key signature, and a fully downloaded
payload of the exact size and SHA-256; the manifest is checked again after the
payload transfer. Success is `PROMOTED_ALL_MIRRORS_VERIFIED`. If a mirror stays
old or fails verification after the local swap, the command exits nonzero with
`LOCAL_PROMOTED_MIRROR_VERIFICATION_FAILED`, preserving the qualified EU1
generation and recording each mirror's result. Refresh the lagging mirror and
rerun; an incomplete rollout is never reported as a preflight refusal or as
success.

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
