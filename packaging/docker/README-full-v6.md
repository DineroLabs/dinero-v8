# Prepared full-v6 image lane

`Dockerfile.full-v6` and `entrypoint-full-v6.sh` prepare the next server image.
The live root `Dockerfile`, `docker-entrypoint.sh`, and `docker-publish.yml`
still publish the old image. This candidate refuses to build while
`desktop-inputs-candidate.json` names the v5 primary.

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

Before a release tag exists, the desktop candidate workflows take the exact
snapshot and publisher manifest URLs as inputs. They use HTTPS, verify the
original signature if published, and check the catalog pin. This avoids asking
an unpublished GitHub release for its own assets. The final release must carry
the `.dat`, normalized `.dat.manifest.json`, original
`.dat.publisher.manifest.json`, `.dat.publisher.manifest.sig`, and
qualification receipt. Its exact H150521 payload SHA-256 and length must come
from a successful native export, import, and restart at the frozen source SHA.

After that source and payload qualify, bump the release version together,
switch the live Docker workflow to this candidate, and run a hosted fresh-volume
image smoke with all four fallback paths. The prepared Dockerfile deliberately
does not make a release or tag.
