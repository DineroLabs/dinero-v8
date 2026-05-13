# 🐳 Dinero Production Container Image
# Distroless, non-root, read-only filesystem

# Build stage
FROM ubuntu:22.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    libsqlite3-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
WORKDIR /src
COPY . .

# Build release binary
RUN cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SANITIZERS=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build build -j$(nproc) \
    && strip build/bin/dinerod

# Runtime stage - distroless for minimal attack surface
FROM gcr.io/distroless/cc-debian12:nonroot

# Metadata
LABEL org.opencontainers.image.title="Dinero Cryptocurrency Node"
LABEL org.opencontainers.image.description="Production-ready cryptocurrency daemon with enterprise security"
LABEL org.opencontainers.image.version="v1.0.0"
LABEL org.opencontainers.image.vendor="Dinero Project"
LABEL org.opencontainers.image.licenses="MIT"
LABEL org.opencontainers.image.source="https://github.com/dinero/dinero"
LABEL org.opencontainers.image.documentation="https://github.com/dinero/dinero/blob/main/README.md"

# Create app directory
WORKDIR /app

# Copy binary from builder
COPY --from=builder /src/build/bin/dinerod /app/dinerod

# Copy configuration templates
COPY --from=builder /src/ops/systemd/dinerod.service /app/dinerod.service.template
COPY --from=builder /src/ops/nginx/dinero-rpc.conf /app/nginx.conf.template

# Use non-root user (distroless nonroot user)
USER 65532:65532

# Expose ports
EXPOSE 20998/tcp 21001/tcp

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD ["/app/dinerod", "--healthcheck"]

# Default entrypoint
ENTRYPOINT ["/app/dinerod"]

# Default arguments for container deployment
CMD ["-datadir=/data", \
     "-printtoconsole=1", \
     "-rpcbind=0.0.0.0", \
     "-rpcport=20998", \
     "-wsport=21001", \
     "-listen=0", \
     "-dnsseed=0", \
     "-upnp=0", \
     "-gen=0"]
