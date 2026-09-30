#pragma once

#include <vector>

// Colony upkeep outside consensus. Pure functions of a colony, so they are testable without a node.

namespace AntColonyMaintenance
{
// Records whose rebuild found a shift above the guess they were ranked on. Ranking belongs to the tick
// processor, so a rebuild on any other thread leaves the index here.
inline volatile char gRaisedShiftLock = 0;
inline std::vector<unsigned int> gRaisedShiftRecords;

inline void publishRebuilt(AntColonyBpp9000T& colony, unsigned int index, const AntColonyBpp9000T::Ann& ann, unsigned int annHash,
    const score_engine::Rating& rating)
{
    const AntSolutionRecord* record = colony.recordAt(index);
    const bool raised = (record != nullptr) && (rating.shift > record->shift);
    colony.publishAnn(index, ann, annHash, rating.shift);
    if (raised)
    {
        LockGuard guard(gRaisedShiftLock);
        gRaisedShiftRecords.push_back(index);
    }
}

// Tick processor only. rank(publicKey, rankingKey, tick) is the node's miner ranking update.
template<typename RankFn>
inline unsigned int drainRaisedShifts(AntColonyBpp9000T& colony, RankFn&& rank)
{
    std::vector<unsigned int> raised;
    {
        LockGuard guard(gRaisedShiftLock);
        raised.swap(gRaisedShiftRecords);
    }
    for (unsigned int index : raised)
    {
        const AntSolutionRecord* record = colony.recordAt(index);
        if (record == nullptr)
        {
            continue;
        }
        const score_engine::Rating rating{ record->score, record->shift };
        rank(record->pubkey, rating.rankingKey(), record->selfRef.tick);
    }
    return (unsigned int)raised.size();
}

// The indices name records of the epoch that queued them.
inline void clearRaisedShifts()
{
    LockGuard guard(gRaisedShiftLock);
    gRaisedShiftRecords.clear();
}

// fork() clones only the calling thread, so a promoted child can inherit a claim with no owner and
// ensureAntRecordAnn's waiter would spin on it forever.
inline unsigned int releaseInheritedClaims(AntColonyBpp9000T& colony)
{
    unsigned int released = 0;
    const unsigned int recordCount = colony.solutionCount();
    for (unsigned int index = 0; index < recordCount; index++)
    {
        if (colony.isAnnClaimHeld(index))
        {
            colony.releaseAnnClaim(index);
            released++;
        }
    }
    return released;
}

// A rebuild starts from the parent's network, so a record whose parent has none cannot be taken.
inline bool isRebuildableNow(AntColonyBpp9000T& colony, unsigned int index)
{
    if (colony.isAnnMaterialised(index) || colony.isAnnClaimHeld(index))
    {
        return false;
    }
    const AntSolutionRecord* record = colony.recordAt(index);
    if (record == nullptr)
    {
        return false;
    }
    if (record->parentRef.isRoot())
    {
        return true;
    }
    const long long parentIndex = colony.findIndexBySolutionRef(record->parentRef);
    return parentIndex != ANT_INVALID_INDEX && colony.isAnnMaterialised((unsigned int)parentIndex);
}
}
