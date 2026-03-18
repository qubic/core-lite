# =============================================================================
# Stage 1: Build the Qubic binary from source (testnet flags already set in
# src/qubic.cpp: TESTNET + TESTNET_PREFILL_QUS, USE_SWAP commented out)
# =============================================================================
FROM --platform=linux/amd64 ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    clang \
    cmake \
    nasm \
    libc++-dev \
    libc++abi-dev \
    libjsoncpp-dev \
    uuid-dev \
    zlib1g-dev \
    libssl-dev \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build/qubic-core-lite
# Copy full core-lite source (build context is core-lite/)
COPY . .

RUN mkdir -p build && cd build \
    && cmake .. \
        -D CMAKE_C_COMPILER=clang \
        -D CMAKE_CXX_COMPILER=clang++ \
        -D BUILD_TESTS=OFF \
        -D BUILD_BINARY=ON \
        -D CMAKE_BUILD_TYPE=Release \
        -D ENABLE_AVX512=OFF \
    && cmake --build . --target Qubic -- -j$(nproc)

# =============================================================================
# Stage 2: Minimal runtime image
# =============================================================================
FROM --platform=linux/amd64 ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libc++1 \
    libc++abi1 \
    libjsoncpp25 \
    libssl3 \
    curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/qubic-core-lite/build/src/Qubic /usr/local/bin/Qubic
RUN chmod +x /usr/local/bin/Qubic

COPY docker/testnet/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# P2P testnet (31841), HTTP RPC API (41841)
EXPOSE 31841 41841

WORKDIR /qubic

ENTRYPOINT ["/entrypoint.sh"]
