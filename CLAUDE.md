# Qubic Core Lite — Claude Code Guide

## Project Overview
Lightweight Qubic blockchain node that runs on Linux/Windows (no UEFI required). Implements consensus, smart contracts (QPI), solution scoring, networking, and an RPC API (Linux only).

## Build

**Requirements:** Clang 18+ (AppleClang 15+ on macOS), CMake 3.15+, NASM 2.16+

```bash
# Configure
cmake -B build \
  -D CMAKE_C_COMPILER=clang \
  -D CMAKE_CXX_COMPILER=clang++ \
  -D BUILD_TESTS=ON \
  -D BUILD_BINARY=OFF \
  -D CMAKE_BUILD_TYPE=Release \
  -D ENABLE_AVX512=OFF

# Build
cmake --build build -- -j$(nproc)
```

Key CMake options:
- `BUILD_TESTS` — Google Test suite
- `BUILD_BINARY` — EFI application
- `BUILD_BENCHMARK` — UEFI benchmark
- `USE_SANITIZER` — ASAN/UBSAN (test builds)

## Run Tests

```bash
cd build && ctest --output-on-failure
# or directly:
./build/bin/qubic_core_tests
```

Test sources live in `test/`. Most test files are disabled by default in `test/CMakeLists.txt` — enable them individually.

## Key Directories

| Path | Purpose |
|------|---------|
| `src/` | Main source (entry: `src/qubic.cpp`) |
| `src/contracts/` | Smart contracts (Qbay, QBond, Qearn, QubicSolanaBridge, …) |
| `src/contract_core/` | Contract execution engine & QPI definitions |
| `src/network_core/` | Networking |
| `src/network_messages/` | Wire protocol message types |
| `lib/platform_os/` | Linux/Windows platform layer |
| `lib/platform_common/` | Cross-platform utilities |
| `test/` | Google Tests + test data CSVs |
| `tools/` | Utilities (score test generator, Python scripts) |
| `docker/` | Docker environments for local testnets |

## Configuration

- `src/private_settings.h` — runtime config: seeds, ports, feature flags
- `src/public_settings.h` — compile-time constants

Important feature flags (in `private_settings.h`):
- `#define TESTNET` — enable testnet mode
- `#define USE_SWAP` — disk-as-RAM for mainnet
- `#define INCLUDE_CONTRACT_TEST_EXAMPLES` — enable test contracts
- `#define TICK_STORAGE_AUTOSAVE_MODE` — tick snapshots

## Coding Conventions

- **C++20**, Clang only (no MSVC-isms in core logic)
- No STL in hot paths — platform primitives preferred
- Smart contracts implement QPI interface defined in `src/contract_core/`
- New contracts go in `src/contracts/` with a corresponding test in `test/`
- Test data (ground-truth CSVs) lives in `test/data/`

