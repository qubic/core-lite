# Qubic Core Lite

[![Build](https://github.com/qubic/core-lite/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/qubic/core-lite/actions/workflows/ci.yml)

The lite version of Qubic Core that can run directly on the OS without a UEFI environment.

[Compare core-lite (develop) vs. core (develop)](https://github.com/qubic/core-lite/compare/develop...qubic:core:develop)

## Menu

- [Qubic Core Lite](#qubic-core-lite)
  - [Supporting Networks](#supporting-networks)
  - [Parameters](#parameters)
  - [Prerequisites](#prerequisites)
    - [Local Testnet](#local-testnet)
    - [Mainnet](#mainnet)
  - [Build Config](#build-config)
    - [Local Testnet](#local-testnet-1)
    - [Mainnet](#mainnet-1)
  - [Build](#build)
    - [Windows](#windows)
    - [Linux](#linux)
  - [Node State](#node-state)
  - [Ticking](#ticking)
  - [RPC](#rpc)
  - [Explorer](#explorer)
  - [Tips](#tips)
  - [FAQs](#faqs)
  - [Command Line Argument](#command-line-argument)
  - [Supporting Platform](#supporting-platform)
  - [Donate The Project](#donate-the-project)

## Supporting Networks

- [x] Mainnet (Beta)
- [x] Local Testnet

## Parameters

- **Security tick** : `./Qubic --security-tick 32`
> The security tick temporarily skips verifying your **node’s contract state (computer digest)** against the quorum. Verification is performed only every `--security-tick` interval.

- **Ticking delay (local testnet)**: `./Qubic --ticking-delay 1000`
> If your local testnet ticking too fast, you can slow it down by `--ticking-delay` ms.

- **Peers**: `./Qubic --peers 1.2.3.4,8.8.8.8`
> You can add more peers using command line

## Prerequisites

### Local Testnet

To run a qubic **local testnet** node, you need the following spec:

- **16GB** RAM.

> **No initial files are needed** in this version (eg. spectrum, universe, contract,...)

### Mainnet

To run a qubic **mainnet** node, you need the following spec:

- High frequency CPU with AVX2/AVX512 support (recommend VCPU AMD 7950x @ 8theads)
- 1Gb/s synchronous internet connection
- **64GB** RAM.
- **500GB** fast SSD disk.

> **Initial files are needed** in this version (eg. spectrum, universe, contract,...)

## Build Config

### Local Testnet

**Local Testnet Single Node**

In `qubic.cpp`

**1.** Uncomment `// #define TESTNET`

```cpp
// #define TESTNET // UNCOMMENT this line if you want to compile for testnet

// this option enables using disk as RAM to reduce hardware requirement for qubic core node
// it is highly recommended to enable this option if you want to run a full mainnet node on SSD
// UNCOMMENT this line to enable it
#define USE_SWAP
```

**2.** Build

**Long-Run Local Testnet (unattended, weeks to months)**

A single node running all 676 computors continuously, ticking at a fixed wall-clock rate.

**1.** Uncomment `// #define LONG_RUN_LOCAL_TESTNET` together with `// #define TESTNET` in `qubic.cpp`, or build with CMake:

```bash
cmake .. -D LONG_RUN_LOCAL_TESTNET=ON   # implies TESTNET
```

**2.** Run. No `--node-mode` or F12 needed — the node starts in MAIN&MAIN mode and ticks unattended.

What the mode changes compared to plain TESTNET:

- **Tick pacing**: each tick takes a fixed wall-clock duration, default **1 second**. Change at startup with `--tick-duration <ms>` (0 = unpaced, max 30000; higher values would trip the next-tick-data timeout of 5 × `TARGET_TICK_DURATION`).
- **Epoch switches only on tick-buffer exhaustion or F7**: no time-based epoch rollover. Tick buffers hold `LONG_RUN_EPOCH_TICK_CAPACITY` ticks (default 5,184,000 = 60 days at 1 s ticks; override with `-D LONG_RUN_EPOCH_TICK_CAPACITY=<ticks>`); when they are about to run out the node performs one seamless epoch transition, which resets the tick storage. Pressing **F7** (or the force-switch special command) triggers the same transition manually at any time. Note: without the F10 pause, external log readers have no drain window around a transition.
- **Unattended**: the node starts as MAIN&MAIN and never waits for F10.
- **Smaller transaction buffers** (`TRANSACTION_SPARSENESS` 10, like mainnet): a local testnet carries little traffic.
- **Disk**: with `USE_SWAP` (default) the tick storage pages to disk as the single epoch grows — roughly **50 GB per day** of 1 s ticks raw (vote storage 21 GB/day is incompressible; the rest is mostly zeros, so `--swap-compression` brings the total to ~25 GB/day). Budget disk for your intended run length. Build with `-D NO_ENABLE_QUBIC_LOGGING_EVENT=ON` if you don't need logging events for a months-long run.
- **No check-in calls** to api.qubic.global.

Keep `USE_SWAP` enabled (default): RAM stays flat across the epoch while tick storage pages to disk.

> **Note**
> The default (non-LITE) long-run build needs **~32 GB RAM** (startup prints `Total RAM required 27 GB`; observed steady RSS is ~32 GB). On smaller machines combine it with `-D TESTNET_LITE_RAM=ON` (~7 GB, but wire/snapshot-incompatible with non-LITE nodes).

**Local Testnet Multiple Nodes**

Afer single node steps please do:

In `private_settings.h`, split the 676 seeds in `broadcastedComputorSeeds` into `computorSeeds` across your nodes (e.g., 300 seeds in node 1, the remaining 376 seeds in node 2):

```c++
static unsigned char computorSeeds[][55 + 1] = {
};
```

> **Warning**
> Do not change the `broadcastedComputorSeeds`.

### Mainnet

Make sure you have commented `#define TESTNET`

**1.** Add public peers from https://app.qubic.li/network/live via command line `--peers` (eg. `--peers 15.235.225.233,115.79.212.169`)

**2.** Prepare the epoch files (blockchain state).

They should be named and structured as follows:

```
./contract0000.XXX
./contract0001.XXX
./contract0002.XXX
./contract0003.XXX
./contract0004.XXX
./contract0005.XXX
./contract0006.XXX
./contract0007.XXX
./contract0008.XXX
./contract0009.XXX
./contract0010.XXX
./contract0011.XXX
./contract00xx.XXX
./spectrum.XXX
./universe.XXX
```

Place all of these files in the same directory where you plan to launch the `Qubic` binary.

**3.** Build

## Build

### Windows

- Open .sln file in project root folder in Visual Studio
- Change build config to Release -> Right click at Qubic project -> Build

### Linux

Detailed instruction can be found here: [Linux Build Tutorial](./README_CLANG.md)

## Node State

### Local Testnet

- 676 seeds in `broadcastedComputorSeeds` and `customSeeds` each has 10B Qubic.

### Mainnet

Current mainnet state

## Ticking

Press **F12** to switch to **MAIN** mode to make the network start ticking (processing transactions).

## RPC

> This feature only available in Linux!

Qubic Core Lite provides a built-in RPC API that enables developers to interact directly with a Lite node with official RPC style, removing the need for an original complex RPC layer.

### Status

- **RPC Live (OK):** `http://localhost:41841/live/v1`
- **RPC Stats (OK):** `http://localhost:41841/`
- **RPC Query V2 (OK):** `http://localhost:41841/query/v1`
- **RPC Archiver V2:** *Deprecated (not implemented)*

### Documentation

https://qubic.github.io/integration/Partners/swagger/qubic-rpc-doc.html?urls.primaryName=Qubic%20RPC%20Live%20Tree  
> Remember to select the appropriate API definition for each endpoint.

## Explorer

> This feature only available in Linux!

A built-in block explorer is served directly by the node — no separate frontend to deploy. Open it in a browser:

**`http://localhost:41841/explorer`**

Views:

- **Overview** — live tick, epoch, quorum, mempool, peers, top miners, supply
- **Ticks** — transactions per tick, contract-name tags, per-tx event-log popup, vote-alignment popup
- **Transactions / Identities** — full tx detail and transfer history
- **Contracts** — registry and recent contract calls
- **Logs** — live event-log feed (newest first), filterable by tick
- **Computors** — 676-node quorum list with current leader

Terminal-style UI with 5 switchable color themes (saved in your browser).

## Tips

- **For Local Testnet:** Default `PORT` is **31841**, you can change it in `qubic.cpp`
- **For Local Testnet:** If you want to fund your custom wallet (seed), you can add these into `customSeeds` in `private_settings.h`
- **For Local Testnet:** An epoch will have `TESTNET_EPOCH_DURATION` (**3000**) ticks by default, you can change it in `public_settings.h`
- You can deploy your own RPC server to core lite - [how to](https://qubic-sc-docs.pages.dev/rpc/setup-rpc)
- Change `TICK_STORAGE_AUTOSAVE_MODE` in `private_settings.h` to `1` to enable **Snapshot** mode (your node will start from latest saved snapshot state when crash/restart instead of from scratch)

## FAQs

- **My node stop ticking after restart, why?**
Delete the **system** file at your current working folder, it may make your node start with wrong state.

## Command Line Argument

| Feature | Syntax | Example Usage | Description |
| :--- | :--- | :--- | :--- |
| **Peers** | `--peers` | `--peers 127.0.0.1` | Specifies peer nodes for network connection. |
| **Security Tick** | `--security-tick` | `--security-tick 32` | Verifies state after every X ticks to reduce the node's computational load. |
| **Lite Node Operator Alias** | `--operator-alias` | `--operator-alias "MyNode"` | A human-readable name for the lite node operator. |
| **Lite Node Operator ID (Seed)** | `--operator-seed` | `--operator-seed aaa...aaa` | Used to identify lite node operators in the network (utilized by the **Network Guardian** project). |
| **Logging Reader Passcode** | `--reader-passcode` | `--reader-passcode 1-2-3-4` | The passcode required to access or read node logs. |
| **HTTP Admin Passcode** | `--http-passcode` | `--http-passcode 1-2-3-4` | The passcode required for admin HTTP routes (shutdown, snapshot, file download). Admin routes are disabled when unset. |

## Supporting Platform

- [x] Windows

- [x] Linux

