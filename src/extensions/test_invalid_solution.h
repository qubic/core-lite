#pragma once

// =====================================================================
// Test helper: broadcast a fully-random MiningSolutionTransaction.
//
// Mining solutions are normally added to system.solutions[] only after
// passing isValidScore() && isGoodScore() (qubic.cpp:741-743), so the
// regular broadcast path at qubic.cpp:3975-4063 will only ever publish
// *correct* solutions. That makes it impossible to exercise the
// reprocessSolutionTransaction() rollback path (qubic.cpp:5640) without
// hacking many places in the source.
//
// This helper synthesizes a syntactically valid solution tx out of
// thin air — picks one of our own computors, fills miningSeed with the
// node's current random seed, fills nonce with random bytes, signs with
// that computor's subseed, and enqueues it as BROADCAST_TRANSACTION.
//
// The tx will be accepted into a future tick by the network (well-formed
// + valid signature + funded computor address). When the receiving nodes
// process it inside processTickTransactionSolution() (qubic.cpp:2552),
// the score for the random (computor, miningSeed, nonce) tuple will not
// reach the threshold, the deposit will be forfeited, and the rollback
// path inside reprocessSolutionTransaction() will fire as soon as the
// spectrum-digest mismatch is detected at qubic.cpp:6163-6169.
//
// Wiring:
//   1. One #include of this file in qubic.cpp (after spectrum/special_entities.h
//      and network_core/peers.h are visible).
//   2. Call TestInvalidSolution::broadcastRandom(score->currentRandomSeed,
//                                                system.tick + MIN_MINING_SOLUTIONS_PUBLICATION_OFFSET);
//      from wherever you want to inject (e.g. behind a runtime flag like
//      forceVerifySolutions, or a fixed cadence inside processTick()).
// =====================================================================

#include "platform/m256.h"
#include "mining/mining.h"
#include "spectrum/special_entities.h"
#include "network_core/peers.h"
#include "network_messages/network_message_type.h"
#include "network_messages/transactions.h"
#include "kangaroo_twelve.h"
#include "four_q.h"

namespace TestInvalidSolution
{

namespace detail
{

// Sign and broadcast a zero-input standard QU transfer from `sourceComputorIdx`
// (one of our own computor seeds) to `destinationPublicKey`. Used by
// broadcastRandom() to enqueue extra normal transfers alongside the invalid
// solution tx, exercising the rollback path's preservation of neighboring
// spectrum entries via latestIncomingTransferTickPreserveSpectrumIndexes.
inline void broadcastTransfer(unsigned int sourceComputorIdx,
                              const m256i& destinationPublicKey,
                              long long amount,
                              unsigned int txTick)
{
    struct
    {
        Transaction transaction;
        unsigned char signature[SIGNATURE_SIZE];
    } payload;
    static_assert(sizeof(payload) == sizeof(Transaction) + SIGNATURE_SIZE,
                  "TestInvalidSolution transfer payload layout drifted");

    payload.transaction.sourcePublicKey      = computorPublicKeys[sourceComputorIdx];
    payload.transaction.destinationPublicKey = destinationPublicKey;
    payload.transaction.amount               = amount;
    payload.transaction.tick                 = txTick;
    payload.transaction.inputType            = 0;
    payload.transaction.inputSize            = 0;

    unsigned char digest[32];
    KangarooTwelve(&payload.transaction,
                   sizeof(payload.transaction),
                   digest,
                   sizeof(digest));
    sign(computorSubseeds[sourceComputorIdx].m256i_u8,
         computorPublicKeys[sourceComputorIdx].m256i_u8,
         digest,
         payload.signature);

    enqueueResponse(NULL, sizeof(payload), BROADCAST_TRANSACTION, 0, &payload);
}

} // namespace detail

// Build a syntactically valid MiningSolutionTransaction signed by a random
// one of our own computors, with a random nonce, and enqueue it for
// broadcast. The score of the resulting (pubkey, miningSeed, nonce) tuple
// will (with overwhelming probability) be below threshold, exercising the
// rollback path on the receiving side.
//
// Alongside the invalid solution we also enqueue three standard QU transfers
// (signed by the same own-computor) destined for: the wrong-sol broadcaster
// itself, a random network computor, and a fully random id. Their presence
// in the same tick makes the receiving node bump latestIncomingTransferTick
// on neighboring spectrum entries, so the rollback must preserve those bumps
// (latestIncomingTransferTickPreserveSpectrumIndexes path).
//
// Returns false if we hold no computor seeds (nothing to sign with).
//
// `currentMiningSeed` should be score->currentRandomSeed.
// `txTick`            should be the tick the tx should land in, typically
//                     `system.tick + MIN_MINING_SOLUTIONS_PUBLICATION_OFFSET`.
inline bool broadcastRandom(const m256i& currentMiningSeed, unsigned int txTick)
{
    if (computorSeedsCount == 0)
    {
        return false;
    }

    // Pick a random one of our computors.
    m256i rnd;
    rnd.setRandomValue();
    const unsigned int computorIdx = (unsigned int)(rnd.m256i_u64[0] % computorSeedsCount);

    // ---- 1) Invalid solution tx ----
    {
        struct
        {
            Transaction transaction;
            m256i       miningSeed;
            m256i       nonce;
            unsigned char signature[SIGNATURE_SIZE];
        } payload;
        static_assert(sizeof(payload) == sizeof(Transaction) + 32 + 32 + SIGNATURE_SIZE,
                      "TestInvalidSolution payload layout drifted");

        payload.transaction.sourcePublicKey      = computorPublicKeys[computorIdx];
        payload.transaction.destinationPublicKey = m256i::zero();
        payload.transaction.amount               = MiningSolutionTransaction::minAmount();
        payload.transaction.tick                 = txTick;
        payload.transaction.inputType            = MiningSolutionTransaction::transactionType();
        payload.transaction.inputSize            = sizeof(payload.miningSeed) + sizeof(payload.nonce);

        payload.miningSeed = currentMiningSeed;
        payload.nonce.setRandomValue();

        unsigned char digest[32];
        KangarooTwelve(&payload.transaction,
                       sizeof(payload.transaction) + sizeof(payload.miningSeed) + sizeof(payload.nonce),
                       digest,
                       sizeof(digest));
        sign(computorSubseeds[computorIdx].m256i_u8,
             computorPublicKeys[computorIdx].m256i_u8,
             digest,
             payload.signature);

        enqueueResponse(NULL, sizeof(payload), BROADCAST_TRANSACTION, 0, &payload);
    }

    // ---- 2) Standard QU transfer to the id that signed the wrong sol ----
    const long long transferAmount = 1;
    detail::broadcastTransfer(computorIdx,
                              computorPublicKeys[computorIdx],
                              transferAmount,
                              txTick);

    // ---- 3) Standard QU transfer to a random network computor ----
    m256i randomComputorRnd;
    randomComputorRnd.setRandomValue();
    const unsigned int randomComputorIdx =
        (unsigned int)(randomComputorRnd.m256i_u64[0] % NUMBER_OF_COMPUTORS);
    detail::broadcastTransfer(computorIdx,
                              broadcastedComputors.computors.publicKeys[randomComputorIdx],
                              transferAmount,
                              txTick);

    // ---- 4) Standard QU transfer to a fully random id ----
    m256i randomId;
    randomId.setRandomValue();
    detail::broadcastTransfer(computorIdx,
                              randomId,
                              transferAmount,
                              txTick);

    return true;
}

} // namespace TestInvalidSolution
