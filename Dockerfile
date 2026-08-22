# syntax=docker/dockerfile:1
#
# Official Dinero full-node image.
#
# Installs the official release artifacts, verified against the published SHA256SUMS,
# rather than compiling — so the container ships byte-identical binaries to a manual
# install and the build takes seconds. Bundles the AssumeUTXO snapshot so a fresh node
# reaches the tip in minutes with no multi-gigabyte UTXO database — the project's
# headline claim, true on first run.
#
# Verification asymmetry, stated honestly:
#   * the SNAPSHOT is content-pinned by a trust anchor compiled into dinerod
#     (src/consensus/assume_utxo.cpp) and by the bundled manifest, so it is verified
#     independently of the channel it was downloaded over;
#   * the BINARIES are covered by SHA256SUMS fetched over the same TLS connection from
#     the same host as the artifacts — a same-channel checksum, not a signature.
#     There are no detached signatures or attestations published today.
#
#   docker run -d --name dinero --stop-timeout 60 \
#     --log-opt max-size=50m --log-opt max-file=3 \
#     -v dinero-data:/data -p 20999:20999 ghcr.io/dinerolabs/dinero-v8:8.1.8

ARG DINERO_VERSION=8.1.8
# The snapshot release is explicit rather than inferred from the daemon version.
# Bump it only when that release actually publishes the named snapshot artifacts.
# v8.1.3 retains backward-compatible snapshot upgrades: fresh volumes choose 84131,
# while a persisted lifecycle created by v8.1.1 can select the exact 73035 artifact.
# Never remove a fallback until no supported image can have an active lifecycle at
# that base. A newer primary alone would recreate the interrupted-first-sync brick.
ARG SNAPSHOT_RELEASE=v8.1.8
ARG SNAPSHOT_NAME=dinero-assumeutxo-84131-v4
ARG SNAPSHOT_FALLBACK_NAME=dinero-assumeutxo-73035-v4
# Filename the snapshot is installed under inside the image. It MUST equal the
# manifest's "snapshot_file" field: the daemon's manifest trust gate compares the
# on-disk filename to that field and refuses the snapshot when they differ
# (chainstate_service.cpp ValidateSnapshotManifestPreflight). Asserted at build time.
ARG SNAPSHOT_INSTALL_NAME=dinero-assumeutxo-84131-v4.dat
ARG SNAPSHOT_FALLBACK_INSTALL_NAME=dinero-assumeutxo-73035-v4.dat

# ---------- stage 1: fetch + verify ----------
FROM debian:13-slim AS fetch
ARG DINERO_VERSION
ARG SNAPSHOT_RELEASE
ARG SNAPSHOT_NAME
ARG SNAPSHOT_FALLBACK_NAME
ARG SNAPSHOT_INSTALL_NAME
ARG SNAPSHOT_FALLBACK_INSTALL_NAME

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/dl
RUN set -eux; \
    BASE="https://github.com/DineroLabs/dinero-v8/releases/download/v${DINERO_VERSION}"; \
    SNAP_BASE="https://github.com/DineroLabs/dinero-v8/releases/download/${SNAPSHOT_RELEASE}"; \
    # the checksum file name carries the snapshot height — derive it from SNAPSHOT_NAME
    # so the two cannot drift apart when the snapshot is bumped
    SNAPSHOT_HEIGHT="$(printf '%s' "${SNAPSHOT_NAME}" | sed -n 's/.*-\([0-9][0-9]*\)-v[0-9][0-9]*$/\1/p')"; \
    test -n "${SNAPSHOT_HEIGHT}" || { echo "cannot derive snapshot height from SNAPSHOT_NAME=${SNAPSHOT_NAME}"; exit 1; }; \
    curl -fsSL -O "${BASE}/dinero-linux-x86_64-${DINERO_VERSION}.tar.gz"; \
    curl -fsSL -O "${BASE}/SHA256SUMS-linux-x86_64-${DINERO_VERSION}"; \
    curl -fsSL -O "${SNAP_BASE}/${SNAPSHOT_NAME}.dat"; \
    curl -fsSL -O "${SNAP_BASE}/${SNAPSHOT_NAME}.manifest.json"; \
    curl -fsSL -O "${SNAP_BASE}/${SNAPSHOT_FALLBACK_NAME}.dat"; \
    curl -fsSL -O "${SNAP_BASE}/${SNAPSHOT_FALLBACK_NAME}.manifest.json"; \
    curl -fsSL -O "${SNAP_BASE}/${SNAPSHOT_NAME}.publisher.manifest.json"; \
    curl -fsSL -O "${SNAP_BASE}/${SNAPSHOT_NAME}.publisher.manifest.sig"; \
    curl -fsSL -O "${SNAP_BASE}/SHA256SUMS-assumeutxo-${SNAPSHOT_HEIGHT}"; \
    tar xzf "dinero-linux-x86_64-${DINERO_VERSION}.tar.gz"; \
    # verified AFTER extraction: the checksum file names the two binaries by their
    # in-tarball paths, so all three entries only resolve once unpacked. Both checksum
    # files are verified against the artifacts under their PUBLISHED names, before any
    # rename below.
    sha256sum -c "SHA256SUMS-linux-x86_64-${DINERO_VERSION}"; \
    sha256sum -c "SHA256SUMS-assumeutxo-${SNAPSHOT_HEIGHT}"; \
    # the daemon's manifest trust gate rejects a snapshot whose on-disk filename does
    # not match the manifest's snapshot_file, so a mismatch must fail the BUILD rather
    # than every fresh container at runtime
    MANIFEST_FILE="$(sed -n 's/.*"snapshot_file"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${SNAPSHOT_NAME}.manifest.json")"; \
    test "${MANIFEST_FILE}" = "${SNAPSHOT_INSTALL_NAME}" || { \
        echo "manifest declares snapshot_file=\"${MANIFEST_FILE}\" but the image installs the snapshot as \"${SNAPSHOT_INSTALL_NAME}\"; the daemon's manifest trust gate would reject it at runtime"; \
        exit 1; }; \
    FALLBACK_MANIFEST_FILE="$(sed -n 's/.*"snapshot_file"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "${SNAPSHOT_FALLBACK_NAME}.manifest.json")"; \
    test "${FALLBACK_MANIFEST_FILE}" = "${SNAPSHOT_FALLBACK_INSTALL_NAME}" || { \
        echo "fallback manifest declares snapshot_file=\"${FALLBACK_MANIFEST_FILE}\" but the image installs \"${SNAPSHOT_FALLBACK_INSTALL_NAME}\""; \
        exit 1; }; \
    mkdir -p /out; \
    cp "dinero-linux-x86_64-${DINERO_VERSION}/dinerod"     /out/dinerod; \
    cp "dinero-linux-x86_64-${DINERO_VERSION}/dinero-cli"  /out/dinero-cli; \
    cp "${SNAPSHOT_NAME}.dat"           /out/snapshot.dat; \
    cp "${SNAPSHOT_NAME}.manifest.json" /out/snapshot.manifest.json; \
    cp "${SNAPSHOT_FALLBACK_NAME}.dat"           /out/snapshot-fallback.dat; \
    cp "${SNAPSHOT_FALLBACK_NAME}.manifest.json" /out/snapshot-fallback.manifest.json; \
    chmod +x /out/dinerod /out/dinero-cli

# ---------- stage 2: runtime ----------
FROM debian:13-slim
ARG DINERO_VERSION
ARG SNAPSHOT_INSTALL_NAME
ARG SNAPSHOT_FALLBACK_INSTALL_NAME

LABEL org.opencontainers.image.title="Dinero Full Node"
LABEL org.opencontainers.image.description="Post-quantum, Utreexo-native proof-of-work full node"
LABEL org.opencontainers.image.source="https://github.com/DineroLabs/dinero-v8"
LABEL org.opencontainers.image.documentation="https://github.com/DineroLabs/dinero-v8/blob/dinero-main/README.md"
LABEL org.opencontainers.image.licenses="MIT"
LABEL org.opencontainers.image.version="${DINERO_VERSION}"

# libudev.so.1 is dinerod's only DT_NEEDED library beyond libc/libstdc++ — distroless
# lacks it. debian:13 (not 12) is required because the binary needs GLIBC_2.38 /
# GLIBCXX_3.4.32, which debian:12's glibc 2.36 does not provide.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libudev1 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --create-home --home-dir /data dinero

COPY --from=fetch /out/dinerod                   /usr/local/bin/dinerod
COPY --from=fetch /out/dinero-cli                /usr/local/bin/dinero-cli
# installed under the manifest's declared name, with the manifest as its
# "<snapshot>.manifest.json" sibling — that is the exact path the daemon probes, and
# the only layout in which the manifest trust gate engages instead of warning.
COPY --from=fetch /out/snapshot.dat              /opt/dinero/${SNAPSHOT_INSTALL_NAME}
COPY --from=fetch /out/snapshot.manifest.json    /opt/dinero/${SNAPSHOT_INSTALL_NAME}.manifest.json
COPY --from=fetch /out/snapshot-fallback.dat     /opt/dinero/${SNAPSHOT_FALLBACK_INSTALL_NAME}
COPY --from=fetch /out/snapshot-fallback.manifest.json /opt/dinero/${SNAPSHOT_FALLBACK_INSTALL_NAME}.manifest.json
COPY docker-entrypoint.sh                        /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

ENV DINERO_SNAPSHOT=/opt/dinero/${SNAPSHOT_INSTALL_NAME}
ENV DINERO_SNAPSHOT_FALLBACKS=/opt/dinero/${SNAPSHOT_FALLBACK_INSTALL_NAME}

VOLUME /data
# 20999 = P2P only. RPC (20998) is deliberately NOT exposed: `docker run -P` publishes
# every EXPOSEd port on 0.0.0.0 of the host, and dinerod has no rpcallowip gate, so an
# EXPOSEd 20998 would hand out an ACL-less RPC endpoint. The entrypoint additionally
# binds RPC to 127.0.0.1 inside the container (DINERO_RPCBIND overrides), so it is not
# reachable from other containers on the same network either. `docker exec` runs in the
# container's own network namespace and reaches it regardless.
EXPOSE 20999/tcp

USER dinero
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
