#pragma once

#include "platform/m256.h"
#include "mining/mining.h"
#include "mining/ant_colony/ant_colony_bpp9000.h"
#include "mining/score_common.h"
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

inline void broadcastSolution(unsigned int computorIdx, const m256i& currentMiningSeed, unsigned int txTick, unsigned int claimedScore)
{
    MiningSolutionTransaction payload{};
    payload.sourcePublicKey      = computorPublicKeys[computorIdx];
    payload.destinationPublicKey = m256i::zero();
    payload.amount               = MiningSolutionTransaction::minAmount();
    payload.tick                 = txTick;
    payload.inputType            = MiningSolutionTransaction::transactionType();
    payload.inputSize            = MiningSolutionTransaction::minInputSize();

    payload.miningSeed = currentMiningSeed;
    payload.miningSeed.m256i_u64[0] ^= 1;
    payload.nonce.setRandomValue();
    payload.nonce.m256i_u8[0] = (unsigned char)score_engine::AlgoType::Bpp9000;
    payload.score = claimedScore;
    payload.reserved = 0;

    unsigned char digest[32];
    KangarooTwelve(&payload, sizeof(Transaction) + MiningSolutionTransaction::minInputSize(), digest, sizeof(digest));
    sign(computorSubseeds[computorIdx].m256i_u8, computorPublicKeys[computorIdx].m256i_u8, digest, payload.signature);

    enqueueResponse(NULL, sizeof(payload), BROADCAST_TRANSACTION, 0, &payload);
}

} // namespace detail

inline bool broadcastN(const m256i& currentMiningSeed, unsigned int txTick, unsigned int count, bool sameComputor, unsigned int claimedScore)
{
    if (computorSeedsCount == 0)
        return false;

    m256i randomValue;
    randomValue.setRandomValue();
    const unsigned int firstComputorIndex = (unsigned int)(randomValue.m256i_u64[0] % computorSeedsCount);
    for (unsigned int solutionIndex = 0; solutionIndex < count; solutionIndex++)
    {
        const unsigned int computorIndex = sameComputor ? firstComputorIndex
            : (unsigned int)((firstComputorIndex + solutionIndex) % computorSeedsCount);
        detail::broadcastSolution(computorIndex, currentMiningSeed, txTick, claimedScore);
    }
    return true;
}

inline bool broadcastRandom(const m256i& currentMiningSeed, unsigned int txTick, unsigned int claimedScore)
{
    if (computorSeedsCount == 0)
    {
        return false;
    }

    m256i randomValue;
    randomValue.setRandomValue();
    const unsigned int computorIndex = (unsigned int)(randomValue.m256i_u64[0] % computorSeedsCount);

    // Exercise rollback with an invalid solution followed by ordinary transfers that must survive.
    detail::broadcastSolution(computorIndex, currentMiningSeed, txTick, claimedScore);

    const long long transferAmount = 1;
    detail::broadcastTransfer(computorIndex, computorPublicKeys[computorIndex],
                              transferAmount,
                              txTick);

    m256i randomComputorSelector;
    randomComputorSelector.setRandomValue();
    const unsigned int randomComputorIndex = (unsigned int)(randomComputorSelector.m256i_u64[0] % NUMBER_OF_COMPUTORS);
    detail::broadcastTransfer(computorIndex, broadcastedComputors.computors.publicKeys[randomComputorIndex],
                              transferAmount,
                              txTick);

    m256i randomDestination;
    randomDestination.setRandomValue();
    detail::broadcastTransfer(computorIndex, randomDestination,
                              transferAmount,
                              txTick);

    return true;
}


// Ant-colony injector. The node mines against its own colony, so this drives the whole inputType-12
// path on one machine: broadcast, pre-score, publish, commit, deposit, ranking. Each mode aims at one
// branch of the accept rules, so every ValidityResult is reachable without a second node.
// Same value as MIN_MINING_SOLUTIONS_PUBLICATION_OFFSET, which is defined after this header is included.
static constexpr unsigned int ANT_INJECT_PUBLICATION_OFFSET = 3;

enum class AntInjectMode
{
    Valid,          // honest solution: accepted, deposit refunded, ranked
    BadClaim,       // right nonce, wrong claimedScore: committed and folded, deposit kept
    NonCanonical,   // nonce[1] out of range
    WrongTree,      // parent belonging to another identity
    Stale,          // anchor older than the publish window
    FutureParent,   // parent ref into the current tick
    LeParent,       // child that does not beat its parent
};

namespace detail
{

inline void signAndBroadcastAntSolution(unsigned int computorIdx,
                                        const SolutionRef& parentRef,
                                        unsigned int anchorTick,
                                        unsigned int claimedScore,
                                        const m256i& nonce,
                                        unsigned int txTick)
{
    AntColonyMiningSolutionTransaction payload;
    setMem(&payload, sizeof(payload), 0);
    payload.sourcePublicKey           = computorPublicKeys[computorIdx];
    payload.destinationPublicKey      = m256i::zero();
    payload.amount                    = AntColonyMiningSolutionTransaction::minAmount();
    payload.tick                      = txTick;
    payload.inputType                 = AntColonyMiningSolutionTransaction::transactionType();
    payload.inputSize                 = AntColonyMiningSolutionTransaction::minInputSize();
    payload.parentTick                = parentRef.tick;
    payload.parentSolutionIndexInTick = parentRef.solutionIndexInTick;
    payload.anchorTick                = anchorTick;
    payload.claimedScore              = claimedScore;
    payload.nonce                     = nonce;

    unsigned char digest[32];
    KangarooTwelve(&payload,
                   sizeof(Transaction) + AntColonyMiningSolutionTransaction::minInputSize(),
                   digest,
                   sizeof(digest));
    sign(computorSubseeds[computorIdx].m256i_u8,
         computorPublicKeys[computorIdx].m256i_u8,
         digest,
         payload.signature);

    enqueueResponse(NULL, sizeof(payload), BROADCAST_TRANSACTION, 0, &payload);
}

} // namespace detail

// ColonyT and ScoreT stay template parameters so this header keeps compiling where it is included,
// which is before qubic.cpp declares gAntColony and score.
template<typename ColonyT, typename ScoreT>
// The publish tick is read from system.tick when the transaction is signed, not when the walk
// starts: a walk takes seconds and the chain may have moved dozens of ticks meanwhile.
inline bool broadcastAntSolution(ColonyT& colony,
                                 ScoreT& scoreFn,
                                 unsigned long long processorNumber,
                                 unsigned int anchorTick,
                                 AntInjectMode mode,
                                 unsigned int attempts = 8)
{
    if (computorSeedsCount == 0)
    {
        return false;
    }

    // One identity, so successive solutions chain into a deepening tree rather than 676 depth-1 stubs.
    // Seat 0's root network fails to self-clock under this pool, so mine seat 1.
    const unsigned int computorIdx = 1;
    const m256i& minerKey = computorPublicKeys[computorIdx];

    // Extend this identity's best node when it has one, otherwise start its tree from the root.
    SolutionRef parentRef = ROOT_REF;
    const AntSolutionRecord* parentRec = nullptr;
    score_engine::Rating parentRating = score_engine::Rating::worst();
    for (unsigned int i = 0; i < colony.solutionCount(); i++)
    {
        const AntSolutionRecord* rec = colony.recordAt((long long)i);
        if (rec == nullptr || !(rec->pubkey == minerKey))
        {
            continue;
        }
        const score_engine::Rating recRating{ rec->score, rec->shift };
        if (recRating.isBetterThan(parentRating))
        {
            parentRating = recRating;
            parentRef = rec->selfRef;
            parentRec = rec;
        }
    }

    if (mode == AntInjectMode::WrongTree)
    {
        // Any node owned by somebody else. Without one the rule is not reachable yet.
        parentRec = nullptr;
        for (unsigned int i = 0; i < colony.solutionCount(); i++)
        {
            const AntSolutionRecord* rec = colony.recordAt((long long)i);
            if (rec != nullptr && !(rec->pubkey == minerKey))
            {
                parentRef = rec->selfRef;
                parentRec = rec;
                break;
            }
        }
        if (parentRec == nullptr)
        {
            return false;
        }
    }
    else if (mode == AntInjectMode::FutureParent)
    {
        parentRef.tick = system.tick + ANT_INJECT_PUBLICATION_OFFSET;
        parentRef.solutionIndexInTick = 0;
        parentRec = nullptr;
    }

    unsigned int usedAnchorTick = anchorTick;
    if (mode == AntInjectMode::Stale)
    {
        // Far enough back that the ring no longer holds it.
        usedAnchorTick = (anchorTick > ANT_PUBLISH_WINDOW_TICKS + 1)
            ? (anchorTick - ANT_PUBLISH_WINDOW_TICKS - 1)
            : 0;
    }

    // Anchors are only recorded for non-empty ticks, so an idle testnet has none and the injector
    // would never fire. Walk back a short window, and if nothing is there put a transfer on chain to
    // make this tick non-empty - that seeds the ring for the next attempt.
    m256i anchorDigest = m256i::zero();
    if (mode != AntInjectMode::Stale)
    {
        bool haveAnchor = false;
        for (unsigned int back = 0; back < 16 && back < usedAnchorTick; back++)
        {
            if (colony.getAnchorDigest(usedAnchorTick - back, anchorDigest))
            {
                usedAnchorTick -= back;
                haveAnchor = true;
                break;
            }
        }
        if (!haveAnchor)
        {
            detail::broadcastTransfer(computorIdx, computorPublicKeys[computorIdx], 1,
                                      system.tick + ANT_INJECT_PUBLICATION_OFFSET);
            return false;
        }
    }

    // The parent's network is what the child inherits; the scorer derives the root itself when the
    // parent is ROOT_REF.
    typename ColonyT::Ann parentAnn;
    typename ColonyT::Ann childAnn;
    const typename ColonyT::Ann* parentAnnPtr = nullptr;
    if (parentRec != nullptr)
    {
        if (!colony.annOfNonRoot(*parentRec, parentAnn))
        {
            return false;
        }
        parentAnnPtr = &parentAnn;
    }

    m256i nonce;
    nonce.setRandomValue();
    nonce.m256i_u8[0] = (unsigned char)score_engine::AlgoType::Bpp9000;

    if (mode == AntInjectMode::NonCanonical)
    {
        nonce.m256i_u8[1] = 0;   // L below range; the scorer refuses before walking
        detail::signAndBroadcastAntSolution(computorIdx, parentRef, usedAnchorTick, 0, nonce,
                                            system.tick + ANT_INJECT_PUBLICATION_OFFSET);
        return true;
    }

    // A root parent sits at frame 0.
    const unsigned int parentShift = (parentRec != nullptr) ? parentRec->shift : 0u;
    score_engine::Rating childRating = score_engine::Rating::worst();
    bool found = false;
    for (unsigned int attempt = 0; attempt < attempts && !found; attempt++)
    {
        nonce.setRandomValue();
        nonce.m256i_u8[0] = (unsigned char)score_engine::AlgoType::Bpp9000;
        const unsigned char changesPerStep = (unsigned char)(1 + (nonce.m256i_u8[1] % score_engine::BPP9000_MAX_CHANGES_PER_STEP));
        const unsigned char mutationMode = (unsigned char)(score_engine::BPP9000_MODE_START + (nonce.m256i_u8[3] % 3));
        nonce.m256i_u8[1] = (unsigned char)(changesPerStep | (mutationMode << 4));
        nonce.m256i_u8[2] = 0;   // no explore steps: pure descent gives the best odds of beating the parent

        const unsigned long long walkStart = __rdtsc();
        childRating = scoreFn.computeAntChildScore(processorNumber, parentAnnPtr, parentShift, minerKey, nonce,
                                                   anchorDigest, childAnn);
        {
            CHAR16 line[224];
            setText(line, L"ANT-INJECT attempt score=");
            appendNumber(line, childRating.error, FALSE);
            appendText(line, L" shift=");
            appendNumber(line, childRating.shift, FALSE);
            appendText(line, L" parentScore=");
            appendNumber(line, parentRating.error, FALSE);
            appendText(line, L" parentShift=");
            appendNumber(line, parentShift, FALSE);
            appendText(line, L" threshold=");
            appendNumber(line, colony.errorThreshold(), FALSE);
            appendText(line, L" ms=");
            appendNumber(line, (__rdtsc() - walkStart) / (frequency / 1000), FALSE);
            logToConsole(line);
        }
        if (!childRating.isValid())
        {
            continue;
        }

        const bool beatsParent = childRating.isBetterThan(parentRating);
        const bool clearsThreshold = childRating.clearsFloor(colony.errorThreshold());
        // Every walk beats the root's WORST_SCORE, so LeParent is unreachable until the identity has a real parent: seed one first.
        found = (mode == AntInjectMode::LeParent && parentRec != nullptr)
            ? (clearsThreshold && !beatsParent)
            : (beatsParent && clearsThreshold);
    }

    if (!found)
    {
        return false;
    }

    // BadClaim keeps the honest nonce so the node's recompute succeeds and then disagrees, which is
    // what forfeits the deposit.
    const unsigned int claimedScore =
        (mode == AntInjectMode::BadClaim) ? (childRating.error + 1) : childRating.error;

    const unsigned int publishTick = system.tick + ANT_INJECT_PUBLICATION_OFFSET;
    detail::signAndBroadcastAntSolution(computorIdx, parentRef, usedAnchorTick, claimedScore, nonce, publishTick);

    CHAR16 line[192];
    setText(line, L"ANT-INJECT published score=");
    appendNumber(line, childRating.error, FALSE);
    appendText(line, L" shift=");
    appendNumber(line, childRating.shift, FALSE);
    appendText(line, L" claimed=");
    appendNumber(line, claimedScore, FALSE);
    appendText(line, L" parent=");
    appendNumber(line, parentRef.tick, FALSE);
    appendText(line, L"/");
    appendNumber(line, parentRef.solutionIndexInTick, FALSE);
    appendText(line, L" anchor=");
    appendNumber(line, usedAnchorTick, FALSE);
    appendText(line, L" for tick ");
    appendNumber(line, publishTick, FALSE);
    logToConsole(line);
    return true;
}

} // namespace TestInvalidSolution
