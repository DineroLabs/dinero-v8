# Prepared full-v6 image lane

The root `Dockerfile`, `docker-entrypoint.sh`, and `docker-publish.yml` now
prepare the next server image; the copies in this directory are review fixtures.
No image is published by these source changes. The build refuses the new
primary while `desktop-inputs-candidate.json` still names the v5 base.

The new image takes the exact H150521 full-v6 payload and original publisher
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
URLs as inputs. Signature verification and the catalog pin are mandatory.
Manual runs with URL overrides cannot publish an image. The final release must carry
the `.dat`, normalized `.dat.manifest.json`, original
`.dat.publisher.manifest.json`, `.dat.publisher.manifest.sig`, and
qualification receipt. Its exact H150521 payload SHA-256 and length must come
from a successful native export, import, and restart at the frozen source SHA.
Before v8.2.3 binaries are public, a manual dry-run can use v8.2.2 binaries to
check the image transport and entrypoint, but that is not the final-source
image smoke.

After native source and payload qualification, update the catalog with the
exact H150521 payload hash, bytes, block hash, and profile, then run a hosted
image smoke with all four fallback paths. The release event builds and smokes
the exact published v8.2.3 binaries and signed snapshot assets before pushing
the image. Until those assets exist, the normal PR image check is expected to
fail closed. `v8.2.2` remains only the recovery snapshot release tag.
