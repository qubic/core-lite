# Wasm smart-contract extension

The extension is enabled with one feature switch and two explicit prerequisites:

```sh
cmake -S . -B build-node \
  -DTESTNET=ON \
  -DTESTNET_LITE_RAM=ON \
  -DLITE_WASM_SC=ON
```

CMake rejects `LITE_WASM_SC` without either prerequisite. WAMR always uses the classic interpreter with call-stack capture, and a build without the switch does not link WAMR or libffi.

The source is split by dependency direction:

- `shared/` contains ABI metadata and binary types used by both sides.
- `sdk/` contains contract-side intrinsics, QPI forwarding, registration, storage, and dispatch.
- `runtime/` contains node-side state, deployment, WAMR loading, host services, tracing, and registration.

`runtime/extension.h` is the node's single Wasm include and fixes runtime dependency order.
`sdk/module_runtime.h` is Qinit's final post-contract include and aggregates the contract-side
runtime pieces. Runtime and SDK headers may include `shared/` and headers within their own side, but
must not include each other. `runtime/reserved_slot_contract.h` is intentionally a repeatedly
included fragment and therefore has no include guard.

`LITE_WASM_TU_BUILD` remains the contract-side compilation guard. `LITE_SC_PAGER`, `LITE_SC_CONTRACT_LEVEL`, and `LITE_SC_NO_PAGER` are internal state-backend controls, not user-facing Wasm feature switches.

The `"lhost"` names and signatures, module exports, deployment transaction layout, state layout, libffi call shapes, migration order, and registration order are compatibility boundaries. Changes to them require an explicit ABI or wire-format migration.

A contract failure falls into one of three classes. An abort or trap inside a user function fails that query with a contract error and the node keeps ticking. An abort or trap inside a procedure, system procedure, or migration commits its trace entry, records the node fault served at `/live/v1/dev/fault`, and halts the tick loop. A nested Wasm trap is recovered by the caller as `NoCallError` with zeroed output; a nested abort unwinds every frame and halts like a top-level one. `/live/v1/dev/state-read` serves any length up to the contract's state size; the client chooses its chunk size. `/live/v1/dev/state-bytes` serves the same slice as raw bytes with the state size in `X-State-Size`, for dumps. On a testnet build `/live/v1/dev/state-stage` takes an initial state in ordered chunks: a Wasm slot starts from it at its next deploy (INITIALIZE is skipped, or MIGRATE runs when the size is its `OldStateData`), a native slot has it written at the next tick.
