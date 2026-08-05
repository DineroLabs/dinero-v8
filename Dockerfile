# syntax=docker/dockerfile:1
#
# Official Dinero full-node image.
#
# Installs the SIGNED release artifacts rather than compiling, so the container ships
# byte-identical binaries to a manual install and the build takes seconds. Bundles the
# AssumeUTXO snapshot so a fresh node reaches the tip in minutes with no multi-gigabyte
# UTXO database — the project's headline claim, true on first run.
#
#   docker run -d --name dinero -v dinero-data:/data -p 20999:20999 dinerolabs/dinerod

ARG DINERO_VERSION=8.1.1
ARG SNAPSHOT_NAME=dinero-assumeutxo-73035-v4

# ---------- stage 1: fetch + verify ----------
FROM debian:13-slim AS fetch
ARG DINERO_VERSION
ARG SNAPSHOT_NAME

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/dl
RUN set -eux; \
    BASE="https://github.com/DineroLabs/dinero-v8/releases/download/v${DINERO_VERSION}"; \
    curl -fsSL -O "${BASE}/dinero-linux-x86_64-${DINERO_VERSION}.tar.gz"; \
    curl -fsSL -O "${BASE}/SHA256SUMS-linux-x86_64-${DINERO_VERSION}"; \
    curl -fsSL -O "${BASE}/${SNAPSHOT_NAME}.dat"; \
    curl -fsSL -O "${BASE}/${SNAPSHOT_NAME}.manifest.json"; \
    curl -fsSL -O "${BASE}/SHA256SUMS-assumeutxo-73035"; \
    tar xzf "dinero-linux-x86_64-${DINERO_VERSION}.tar.gz"; \
    # verified AFTER extraction: the checksum file names the two binaries by their
    # in-tarball paths, so all three entries only resolve once unpacked
    sha256sum -c "SHA256SUMS-linux-x86_64-${DINERO_VERSION}"; \
    sha256sum -c "SHA256SUMS-assumeutxo-73035"; \
    mkdir -p /out; \
    cp "dinero-linux-x86_64-${DINERO_VERSION}/dinerod"     /out/dinerod; \
    cp "dinero-linux-x86_64-${DINERO_VERSION}/dinero-cli"  /out/dinero-cli; \
    cp "${SNAPSHOT_NAME}.dat"           /out/snapshot.dat; \
    cp "${SNAPSHOT_NAME}.manifest.json" /out/snapshot.manifest.json; \
    chmod +x /out/dinerod /out/dinero-cli

# ---------- stage 2: runtime ----------
FROM debian:13-slim
ARG DINERO_VERSION

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
COPY --from=fetch /out/snapshot.dat              /opt/dinero/snapshot.dat
COPY --from=fetch /out/snapshot.manifest.json    /opt/dinero/snapshot.manifest.json
COPY docker-entrypoint.sh                        /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

VOLUME /data
# 20999 = P2P (publish this to serve the network). 20998 = RPC — deliberately NOT in
# the documented run command so nobody exposes RPC to the internet by copy-paste.
EXPOSE 20999/tcp 20998/tcp

USER dinero
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
