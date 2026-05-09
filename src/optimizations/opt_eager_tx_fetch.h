#pragma once

// Eager transaction fetch: when tickData arrives, immediately check which
// transactions are missing from the pending pool and tick storage, and
// request them from multiple peers right away — instead of waiting for the
// ticker to discover missing transactions on its next loop and stalling on
// the request round-trip.
//
// During catch-up the ticker processes many ticks in succession; without this
// optimization each tick that has missing transactions stalls for a peer RTT
// (~50-200ms) while the request is in flight. With eager fetch the requests
// pipeline across many future ticks while the ticker is busy with earlier
// ones, so transactions are typically already local by the time the ticker
// reaches them.
//
// Called from processBroadcastFutureTickData() on request-processor threads.
// Only reads pending pool / tick storage (no writes — that is the ticker's job
// via prepareNextTickTransactions()). Requires the caller to have released
// ts.tickData lock if held.

#if USE_EAGER_TX_FETCH

static void eagerFetchMissingTransactions(const TickData& tickData)
{
    const unsigned int tick = tickData.tick;
    const unsigned int tickIndex = ts.tickToIndexCurrentEpoch(tick);
    const auto* txOffsets = ts.tickTransactionOffsets.getByTickIndex(tickIndex);
    const unsigned int numPending = pendingTxsPool.getNumberOfPendingTickTxs(tick);

    // Bit i clear == request transaction i; default all set (= "do not request").
    unsigned char txFlags[NUMBER_OF_TRANSACTIONS_PER_TICK / 8];
    setMem(txFlags, sizeof(txFlags), 0xff);

    unsigned int missingTx = 0;
    for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
    {
        if (isZero(tickData.transactionDigests[i]))
            continue;

        // Already in tick storage.
        if (txOffsets[i])
            continue;

        // Already in pending pool.
        bool found = false;
        for (unsigned int p = 0; p < numPending; p++)
        {
            const m256i* digest = pendingTxsPool.getDigest(tick, p);
            if (digest && *digest == tickData.transactionDigests[i])
            {
                found = true;
                break;
            }
        }
        if (found)
            continue;

        // Missing — request from peers.
        txFlags[i >> 3] &= ~(1 << (i & 7));
        missingTx++;
    }

    if (missingTx == 0)
        return;

    char reqBuffer[sizeof(RequestResponseHeader) + sizeof(RequestTickTransactions)];
    RequestResponseHeader* reqHeader = (RequestResponseHeader*)reqBuffer;
    reqHeader->setSize2(sizeof(reqBuffer));
    reqHeader->setType(RequestTickTransactions::type());
    reqHeader->setDejavu(0);
    RequestTickTransactions* req = (RequestTickTransactions*)(reqHeader + 1);
    req->tick = tick;
    copyMem(req->transactionFlags, txFlags, sizeof(txFlags));

    pushToSeveral(reqHeader);
}

#endif // USE_EAGER_TX_FETCH
