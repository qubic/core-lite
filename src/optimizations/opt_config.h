#pragma once

// Master configuration for lite node optimization features.
// Each flag controls a specific optimization that can be toggled independently.
// See individual opt_*.h headers for implementation details.
//
// All optimizations default to OFF (0). Enable by changing 1 here, or by
// passing -DUSE_<NAME>=1 at compile time.

//
// Catch-up / sync optimizations
//

// Eager TX fetch: when tickData arrives via processBroadcastFutureTickData(),
// immediately scan its transaction digests and request missing transactions
// from peers — instead of waiting for the ticker to discover missing txs and
// stall on every tick during catch-up. Pipelines tx requests across many
// future ticks while the ticker is still busy with earlier ticks.
// (opt_eager_tx_fetch.h)
#ifndef USE_EAGER_TX_FETCH
#define USE_EAGER_TX_FETCH 1
#endif

// Future-tick prefetch: when behind the network, requests tickData / votes /
// transactions for ticks ahead of system.tick+1 in parallel (depth scales with
// how far behind, capped at 20). Upstream only requests +1 and +2; this lets
// catch-up pipeline many ticks worth of data instead of stalling per tick.
// (opt_future_tick_prefetch.h)
#ifndef USE_FUTURE_TICK_PREFETCH
#define USE_FUTURE_TICK_PREFETCH 1
#endif
