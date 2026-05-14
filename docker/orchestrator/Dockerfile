# =============================================================================
# Stage 1: Build the Qubic binary from source
# If a pre-built binary exists at build/src/Qubic in the build context,
# the compile step is skipped (used by CI to reuse an earlier build artifact).
# =============================================================================
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    clang \
    cmake \
    nasm \
    libboost-stacktrace-dev \
    libc++-dev \
    libc++abi-dev \
    libjsoncpp-dev \
    uuid-dev \
    zlib1g-dev \
    libzstd-dev \
    liblzo2-dev \
    liblz4-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build/qubic-core-lite
COPY . .

# Always build from source inside the container so the binary's compile
# flags (notably ENABLE_AVX512=OFF) are guaranteed to match this Dockerfile.
# Any local build/ tree from the host is discarded — it may have been
# configured with different flags (e.g. AVX-512 on, the cmake default).
RUN rm -rf build \
    && mkdir -p build && cd build \
    && cmake .. \
        -D CMAKE_C_COMPILER=clang \
        -D CMAKE_CXX_COMPILER=clang++ \
        -D BUILD_TESTS:BOOL=OFF \
        -D BUILD_BINARY:BOOL=ON \
        -D CMAKE_BUILD_TYPE=Release \
        -D ENABLE_AVX512=OFF \
        -D ONLY_LOGGING=ON \
    && cmake --build . --target Qubic -- -j$(nproc)

# Collect build outputs into a common location
RUN mkdir -p /output \
    && cp build/src/Qubic /output/Qubic \
    && grep -oP '#define EPOCH \K[0-9]+' src/public_settings.h > /output/epoch.txt \
    && printf "%s.%s" \
        "$(grep -oP '#define VERSION_A \K[0-9]+' src/public_settings.h)" \
        "$(grep -oP '#define VERSION_B \K[0-9]+' src/public_settings.h)" \
        > /output/version.txt

# =============================================================================
# Stage 2: Runtime image with Python orchestrator
# =============================================================================
FROM ubuntu:24.04

# Runtime C++ dependencies for Qubic binary
RUN apt-get update && apt-get install -y \
    libzstd1 \
    openssh-client \
    rsync \
    curl \
    unzip \
    zip \
    zstd \
    python3 \
    python3-pip \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

# Create Python venv and install dependencies
RUN python3 -m venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Install Python package
COPY docker/orchestrator/pyproject.toml /opt/orchestrator/pyproject.toml
COPY docker/orchestrator/app/ /opt/orchestrator/app/
COPY docker/orchestrator/config/ /opt/orchestrator/config/
WORKDIR /opt/orchestrator
RUN pip install --no-cache-dir .

# Stage binary and metadata in a non-volume location so that container
# image updates always bring the new binary, even when /qubic is a
# persistent Docker volume (Docker only populates volumes on first use).
COPY --from=builder /output/Qubic /opt/qubic-bin/Qubic
COPY --from=builder /output/epoch.txt /opt/qubic-bin/epoch.txt
COPY --from=builder /output/version.txt /opt/qubic-bin/version.txt
RUN chmod +x /opt/qubic-bin/Qubic

WORKDIR /qubic

# Ports: P2P mainnet (21841), P2P testnet (31841), HTTP API (41841), Management (8080)
EXPOSE 21841 31841 41841 8080

# Environment defaults
ENV QUBIC_MODE="normal"
ENV QUBIC_DATA_DIR="/qubic"
ENV QUBIC_BINARY_PATH="/qubic/Qubic"
ENV QUBIC_BINARY_STAGING_DIR="/opt/qubic-bin"
ENV QUBIC_LOG_LEVEL="INFO"
ENV QUBIC_LOG_FORMAT="json"

# No Docker HEALTHCHECK: the Python orchestrator's watchdog already polls the
# node and reacts on failure. A HEALTHCHECK CMD would just spawn a child of
# PID 1 every interval, and PID 1 (Python asyncio) doesn't reap arbitrary
# orphans — that produced zombie curl processes. Remove the duplication.

# PID 1: Python orchestrator
ENTRYPOINT ["python", "-m", "app"]
