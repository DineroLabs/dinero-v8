# syntax=docker/dockerfile:1
# Full-v6 image source. H150521 is the primary only after final-source native
# import/restart and public signed-asset qualification; older bases remain fallbacks.
ARG DINERO_VERSION=8.2.3
ARG PRIMARY_SNAPSHOT_RELEASE=v8.2.3
ARG RECOVERY_SNAPSHOT_RELEASE=v8.2.2
ARG PRIMARY_SNAPSHOT_PAYLOAD_URL=
ARG PRIMARY_SNAPSHOT_MANIFEST_URL=
ARG PRIMARY_SNAPSHOT_SIGNATURE_URL=
ARG BINARY_TARBALL_URL=
ARG BINARY_SUMS_URL=
ARG BINARY_TARBALL_SHA256=
ARG BINARY_SUMS_SHA256=
ARG EXPECTED_SOURCE_SHA=

FROM debian:13-slim AS fetch
ARG DINERO_VERSION
ARG PRIMARY_SNAPSHOT_RELEASE
ARG RECOVERY_SNAPSHOT_RELEASE
ARG PRIMARY_SNAPSHOT_PAYLOAD_URL
ARG PRIMARY_SNAPSHOT_MANIFEST_URL
ARG PRIMARY_SNAPSHOT_SIGNATURE_URL
ARG BINARY_TARBALL_URL
ARG BINARY_SUMS_URL
ARG BINARY_TARBALL_SHA256
ARG BINARY_SUMS_SHA256
ARG EXPECTED_SOURCE_SHA
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl openssl python3 \
 && rm -rf /var/lib/apt/lists/*
COPY packaging/snapshot/check_desktop_primary.py /verify/check_desktop_primary.py
COPY packaging/snapshot/fetch_desktop_primary.py /verify/fetch_desktop_primary.py
COPY packaging/snapshot/snapshot_transport.py /verify/snapshot_transport.py
COPY packaging/snapshot/fleet-publisher-ed25519.pub.pem /verify/fleet-publisher-ed25519.pub.pem
COPY packaging/snapshot/desktop-inputs-candidate.json /verify/desktop-inputs-candidate.json
COPY packaging/docker/check_recovery_manifests.py /verify/check_recovery_manifests.py
COPY scripts/ci/check_snapshot_freshness.py /verify/check_snapshot_freshness.py
WORKDIR /tmp/dl
RUN set -eux; \
    BASE="https://github.com/DineroLabs/dinero-v8/releases/download/v${DINERO_VERSION}"; \
    PRIMARY="https://github.com/DineroLabs/dinero-v8/releases/download/${PRIMARY_SNAPSHOT_RELEASE}"; \
    RECOVERY="https://github.com/DineroLabs/dinero-v8/releases/download/${RECOVERY_SNAPSHOT_RELEASE}"; \
    if [ -n "${PRIMARY_SNAPSHOT_PAYLOAD_URL}${PRIMARY_SNAPSHOT_MANIFEST_URL}${PRIMARY_SNAPSHOT_SIGNATURE_URL}" ]; then \
      test -n "${PRIMARY_SNAPSHOT_PAYLOAD_URL}" && test -n "${PRIMARY_SNAPSHOT_MANIFEST_URL}" && test -n "${PRIMARY_SNAPSHOT_SIGNATURE_URL}" || { echo 'all three signed snapshot URLs are required for pre-release bootstrap'; exit 1; }; \
    else \
      PRIMARY_SNAPSHOT_PAYLOAD_URL="${PRIMARY}/dinero-assumeutxo-150521-v6-full.dat"; \
      PRIMARY_SNAPSHOT_MANIFEST_URL="${PRIMARY}/dinero-assumeutxo-150521-v6-full.dat.publisher.manifest.json"; \
      PRIMARY_SNAPSHOT_SIGNATURE_URL="${PRIMARY}/dinero-assumeutxo-150521-v6-full.dat.publisher.manifest.sig"; \
    fi; \
    if [ -n "${BINARY_TARBALL_URL}${BINARY_SUMS_URL}${BINARY_TARBALL_SHA256}${BINARY_SUMS_SHA256}" ]; then \
      test -n "${BINARY_TARBALL_URL}" && test -n "${BINARY_SUMS_URL}" && test -n "${BINARY_TARBALL_SHA256}" && test -n "${BINARY_SUMS_SHA256}" || { echo 'all exact candidate binary URLs and hashes are required'; exit 1; }; \
      case "${BINARY_TARBALL_URL}" in https://*) ;; *) echo 'binary tarball URL must use HTTPS'; exit 1 ;; esac; \
      case "${BINARY_SUMS_URL}" in https://*) ;; *) echo 'binary checksums URL must use HTTPS'; exit 1 ;; esac; \
      curl -fsSL -o "dinero-linux-x86_64-${DINERO_VERSION}.tar.gz" "${BINARY_TARBALL_URL}"; \
      curl -fsSL -o "SHA256SUMS-linux-x86_64-${DINERO_VERSION}" "${BINARY_SUMS_URL}"; \
      printf '%s  %s\n' "${BINARY_TARBALL_SHA256}" "dinero-linux-x86_64-${DINERO_VERSION}.tar.gz" | sha256sum -c -; \
      printf '%s  %s\n' "${BINARY_SUMS_SHA256}" "SHA256SUMS-linux-x86_64-${DINERO_VERSION}" | sha256sum -c -; \
    else \
      curl -fsSL -O "${BASE}/dinero-linux-x86_64-${DINERO_VERSION}.tar.gz"; \
      curl -fsSL -O "${BASE}/SHA256SUMS-linux-x86_64-${DINERO_VERSION}"; \
    fi; \
    python3 /verify/fetch_desktop_primary.py \
      --payload-url "${PRIMARY_SNAPSHOT_PAYLOAD_URL}" \
      --manifest-url "${PRIMARY_SNAPSHOT_MANIFEST_URL}" \
      --signature-url "${PRIMARY_SNAPSHOT_SIGNATURE_URL}" \
      --destination /tmp/dl --catalog /verify/desktop-inputs-candidate.json; \
    curl -fsSL -O "${RECOVERY}/dinero-assumeutxo-148375-v5.dat"; \
    curl -fsSL -O "${RECOVERY}/dinero-assumeutxo-148375-v5.dat.manifest.json"; \
    curl -fsSL -O "${RECOVERY}/dinero-assumeutxo-99677-v4.dat"; \
    curl -fsSL -O "${RECOVERY}/dinero-assumeutxo-99677-v4.manifest.json"; \
    curl -fsSL -O "${RECOVERY}/dinero-assumeutxo-73035-v4.dat"; \
    curl -fsSL -O "${RECOVERY}/dinero-assumeutxo-73035-v4.manifest.json"; \
    curl -fsSL -O "${RECOVERY}/utxo-snapshot-65300.dat"; \
    curl -fsSL -O "${RECOVERY}/utxo-snapshot-65300.dat.manifest.json"; \
    cp dinero-assumeutxo-99677-v4.manifest.json dinero-assumeutxo-99677-v4.dat.manifest.json; \
    cp dinero-assumeutxo-73035-v4.manifest.json dinero-assumeutxo-73035-v4.dat.manifest.json; \
    tar xzf "dinero-linux-x86_64-${DINERO_VERSION}.tar.gz"; \
    sha256sum -c "SHA256SUMS-linux-x86_64-${DINERO_VERSION}"; \
    test -n "${EXPECTED_SOURCE_SHA}" && test -f "dinero-linux-x86_64-${DINERO_VERSION}/SOURCE_SHA" && \
      test "$(cat "dinero-linux-x86_64-${DINERO_VERSION}/SOURCE_SHA")" = "${EXPECTED_SOURCE_SHA}" || { echo 'Linux archive source SHA differs from the qualified checkout'; exit 1; }; \
    python3 /verify/check_recovery_manifests.py \
      --directory /tmp/dl --catalog /verify/desktop-inputs-candidate.json; \
    mkdir -p /out; \
    cp "dinero-linux-x86_64-${DINERO_VERSION}/dinerod" /out/dinerod; \
    cp "dinero-linux-x86_64-${DINERO_VERSION}/dinero-cli" /out/dinero-cli; \
    cp dinero-assumeutxo-150521-v6-full.dat* \
       dinero-assumeutxo-148375-v5.dat* \
       dinero-assumeutxo-99677-v4.dat* \
       dinero-assumeutxo-73035-v4.dat* \
       utxo-snapshot-65300.dat* /out/; \
    chmod +x /out/dinerod /out/dinero-cli

FROM debian:13-slim
ARG DINERO_VERSION
ARG EXPECTED_SOURCE_SHA
LABEL org.opencontainers.image.title="Dinero Full Node"
LABEL org.opencontainers.image.description="Post-quantum, Utreexo-native proof-of-work full node"
LABEL org.opencontainers.image.source="https://github.com/DineroLabs/dinero-v8"
LABEL org.opencontainers.image.version="${DINERO_VERSION}"
RUN apt-get update && apt-get install -y --no-install-recommends libudev1 ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --create-home --home-dir /data dinero
COPY --from=fetch /out/dinerod /usr/local/bin/dinerod
COPY --from=fetch /out/dinero-cli /usr/local/bin/dinero-cli
# A receipt inside the archive can self-label. Check the independently
# compiled daemon identity too before the image can pass a build.
COPY packaging/docker/check_binary_source.sh /verify/check_binary_source.sh
RUN sh /verify/check_binary_source.sh /usr/local/bin/dinerod "${EXPECTED_SOURCE_SHA}" "${DINERO_VERSION}"
COPY --from=fetch /out/dinero-assumeutxo-150521-v6-full.dat* /opt/dinero/
COPY --from=fetch /out/dinero-assumeutxo-148375-v5.dat* /opt/dinero/
COPY --from=fetch /out/dinero-assumeutxo-99677-v4.dat* /opt/dinero/
COPY --from=fetch /out/dinero-assumeutxo-73035-v4.dat* /opt/dinero/
COPY --from=fetch /out/utxo-snapshot-65300.dat* /opt/dinero/
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh
ENV DINERO_SNAPSHOT=/opt/dinero/dinero-assumeutxo-150521-v6-full.dat
ENV DINERO_SNAPSHOT_FALLBACKS="/opt/dinero/dinero-assumeutxo-148375-v5.dat;/opt/dinero/dinero-assumeutxo-99677-v4.dat;/opt/dinero/dinero-assumeutxo-73035-v4.dat;/opt/dinero/utxo-snapshot-65300.dat"
VOLUME /data
EXPOSE 20999/tcp
USER dinero
WORKDIR /data
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
