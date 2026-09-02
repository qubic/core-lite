#pragma once

// Colony upkeep outside consensus. Pure functions of a colony, so they are testable without a node.

namespace AntColonyMaintenance
{
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
