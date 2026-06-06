#pragma once

// Future-tick prefetch: when the node is behind the network, request tickData,
// votes (quorum tick), and transactions for ticks ahead of system.tick+1
// in parallel — instead of only requesting +1 and +2 (upstream behavior).
//
// During catch-up the node processes ticks in fast succession; without this
// optimization each tick stalls waiting for the next tickData/votes/txs to
// arrive (one peer RTT per tick). Requesting many ticks ahead at once lets
// the data pipeline in while the ticker is busy with earlier ticks.
//
// Prefetch depth is dynamic: scales with how far behind we are (capped at
// CATCHUP_MAX_PREFETCH). When in sync (ticksBehind < 2), behaves like
// upstream — only requests +1 and +2.
//
// Called from the main loop in place of the upstream "request +1 / +2" block.
// Requires Peer::peerReportedTick to be maintained at message-receive sites.

#if USE_FUTURE_TICK_PREFETCH

namespace opt_future_tick_prefetch {

constexpr unsigned int CATCHUP_MAX_PREFETCH = 20;

// Returns the highest tick number any connected peer has reported via
// votes/tickData (i.e. an estimate of the network tip).
static unsigned int estimateNetworkTip()
{
    unsigned int networkTipTick = 0;
    for (unsigned int i = 0; i < NUMBER_OF_OUTGOING_CONNECTIONS + NUMBER_OF_INCOMING_CONNECTIONS; i++)
    {
        if (peers[i].tcp4Protocol && peers[i].isConnectedAccepted && !peers[i].isClosing)
        {
            if (peers[i].peerReportedTick > networkTipTick)
            {
                networkTipTick = peers[i].peerReportedTick;
            }
        }
    }
    return networkTipTick;
}

// Compute how many ticks ahead to prefetch.
// When in sync, returns 2 (matches upstream behavior).
static unsigned int computePrefetchDepth()
{
    const unsigned int networkTipTick = estimateNetworkTip();
    const unsigned int ticksBehind = networkTipTick > system.tick
        ? (networkTipTick - system.tick) : 0;
    if (ticksBehind < 2) return 2;
    if (ticksBehind < CATCHUP_MAX_PREFETCH) return ticksBehind;
    return CATCHUP_MAX_PREFETCH;
}

// Request tickData for ticks +2..+depth that we don't have yet.
// Caller must NOT hold ts.tickData lock; this function acquires/releases it internally.
static void requestFutureTickData(unsigned int prefetchDepth)
{
    ts.tickData.acquireLock();
    for (unsigned int d = 2; d <= prefetchDepth; d++)
    {
        const unsigned int futureTick = system.tick + d;
        if (!ts.tickInCurrentEpochStorage(futureTick)) break;
        if (ts.tickData[futureTick - system.initialTick].epoch != system.epoch)
        {
            requestedTickData.header.randomizeDejavu();
            requestedTickData.requestTickData.requestedTickData.tick = futureTick;
            pushPreferringAtOrAbove(&requestedTickData.header, futureTick);
        }
    }
    ts.tickData.releaseLock();
}

// Request quorum tick (votes) for ticks +2..+depth.
static void requestFutureQuorumTicks(unsigned int prefetchDepth)
{
    for (unsigned int d = 2; d <= prefetchDepth; d++)
    {
        const unsigned int futureTick = system.tick + d;
        if (!ts.tickInCurrentEpochStorage(futureTick)) break;
        requestedQuorumTick.header.randomizeDejavu();
        requestedQuorumTick.requestQuorumTick.quorumTick.tick = futureTick;
        setMem(&requestedQuorumTick.requestQuorumTick.quorumTick.voteFlags,
               sizeof(requestedQuorumTick.requestQuorumTick.quorumTick.voteFlags), 0);
        auto tsCompTicks = ts.ticks.getByTickInCurrentEpoch(futureTick);
        for (unsigned int i = 0; i < NUMBER_OF_COMPUTORS; i++)
        {
            if (tsCompTicks[i].epoch == system.epoch)
            {
                requestedQuorumTick.requestQuorumTick.quorumTick.voteFlags[i >> 3] |= (1 << (i & 7));
            }
        }
        pushPreferringAtOrAbove(&requestedQuorumTick.header, futureTick);
    }
}

// During catch-up, request all transactions for future ticks that already have tickData.
// Caller must NOT hold ts.tickData lock; this function acquires/releases it internally.
// Mutates requestedTickTransactions (resets .tick to 0 at the end).
static void requestFutureTickTransactions(unsigned int prefetchDepth)
{
    if (prefetchDepth <= 2) return;
    ts.tickData.acquireLock();
    for (unsigned int d = 2; d <= prefetchDepth; d++)
    {
        const unsigned int futureTick = system.tick + d;
        if (!ts.tickInCurrentEpochStorage(futureTick)) break;
        if (ts.tickData[futureTick - system.initialTick].epoch == system.epoch)
        {
            requestedTickTransactions.header.randomizeDejavu();
            requestedTickTransactions.requestedTickTransactions.tick = futureTick;
            setMem(requestedTickTransactions.requestedTickTransactions.transactionFlags,
                   sizeof(requestedTickTransactions.requestedTickTransactions.transactionFlags), 0);
            pushPreferringAtOrAbove(&requestedTickTransactions.header, futureTick);
        }
    }
    ts.tickData.releaseLock();
    requestedTickTransactions.requestedTickTransactions.tick = 0;
}

} // namespace opt_future_tick_prefetch

#endif // USE_FUTURE_TICK_PREFETCH
