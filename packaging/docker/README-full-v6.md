# Prepared full-v6 image lane

The root `Dockerfile`, `docker-entrypoint.sh`, and `docker-publish.yml` now
prepare the next server image; the copies in this directory are review fixtures.
No image is published by these source changes. The build refuses the new
primary while `desktop-inputs-candidate.json` still names the v5 base.

The new image takes the exact H152700 full-v6 payload and original publisher
manifest from the release, plus its detached signature. The fetch
helper requires and verifies that signature and creates the normalized runtime
manifest. It independently checks the payload profile, internal checksum,
catalog SHA-256 and byte count, and manifest age. The normalized manifest is
**unsigned** because its `snapshot_file` field names the installed asset rather
than the publisher's `snapshot.dat`. Its catalog pin and the daemon's native
selected-chain check remain mandatory. The image retains H148375, H99677,
H73035, and H65300 for exact-base restart recovery. H73035 is extra image
lineage beyond the three desktop fallbacks.

Before a release tag exists, the desktop candidate workflows and Docker manual
dry-run can take the exact snapshot, publisher manifest, and signature HTTPS
URLs as inputs. The Docker dry-run also needs the exact Linux 8.2.3 archive
and SHA256SUMS from the qualified `linux-release.yml` candidate run, staged at
HTTPS URLs with reviewed SHA-256 hashes for both files. Supply the full
`candidate_source` SHA matching this workflow's checkout. The archive includes
`SOURCE_SHA`; Docker checks it and the compiled daemon's `--version` commit
against that same SHA. Signature verification and the snapshot catalog pin
remain mandatory. Manual runs with URL overrides cannot publish an image.
The final release must carry
the `.dat`, normalized `.dat.manifest.json`, original
`.dat.publisher.manifest.json`, `.dat.publisher.manifest.sig`, and
qualification receipt. Its exact H152700 payload SHA-256 and length must come
from a successful native export, import, and restart at the frozen source SHA.
Pull-request CI runs source contracts only and explicitly reports that no
image was qualified. It does not pair old 8.2.2 binaries with the new snapshot
or fetch an unpublished v8.2.3 release URL. After the exact-source manual
dry-run passes the real daemon fresh-volume and same-volume restart smoke,
the release event repeats that smoke against the published assets before
pushing an image to GHCR.

After native source and payload qualification, update the catalog with the
exact H152700 payload hash, bytes, block hash, and profile, then run a hosted
image smoke with all four fallback paths. The release event builds and smokes
the exact published v8.2.3 binaries and signed snapshot assets before pushing
the image. Until those assets exist, the normal PR image check is expected to
fail closed. `v8.2.2` remains only the recovery snapshot release tag.
