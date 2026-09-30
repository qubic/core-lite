// Lite-only ant colony tests. Built as one translation unit with the upstream tests: their helpers are
// file-static, and a second unit would allocate a second colony.
#include "ant_colony.cpp"

#include "../src/extensions/ant_colony_maintenance.h"

// Same wrapper for a caller that took the score on trust instead of computing it.
static ValidityResult admitTrusted(const ChildCandidate& child, const AntSolutionRecord* parent,
    unsigned int childCount)
{
    return AntColonyBpp9000T::validateChild(child, parent, childCount, TEST_THRESHOLD, true);
}

// Neither rule that judges the score may run on a trusted one: rejecting leaves no refund for the
// quorum to disagree with, so the tree would diverge with nothing to detect it.
TEST(TestAntColonyValidate, TrustedScoreSkipsBothScoreRules)
{
    const m256i me = makeKey(20);
    const AntSolutionRecord parent = makeParent(me, 3800);

    EXPECT_EQ(admit(makeChild(me, 3900), &parent, 0), ValidityResult::RejectBelowThreshold);
    EXPECT_EQ(admitTrusted(makeChild(me, 3900), &parent, 0), ValidityResult::Valid);

    EXPECT_EQ(admit(makeChild(me, 3800), &parent, 0), ValidityResult::RejectLeParent);
    EXPECT_EQ(admitTrusted(makeChild(me, 3800), &parent, 0), ValidityResult::Valid);

    // Even a score the scorer would never return at all.
    EXPECT_EQ(admitTrusted(makeChild(me, WORST_SCORE), &parent, 0), ValidityResult::Valid);
}

// Metadata rules still apply: every node reads those the same way, so over-accepting them would
// manufacture a disagreement with nothing behind it.
TEST(TestAntColonyValidate, TrustedScoreStillHonoursTheMetadataRules)
{
    const m256i me = makeKey(21);
    const m256i someoneElse = makeKey(22);

    const AntSolutionRecord theirNode = makeParent(someoneElse, 3800);
    EXPECT_EQ(admitTrusted(makeChild(me, 3900), &theirNode, 0), ValidityResult::RejectWrongTree);

    const AntSolutionRecord myNode = makeParent(me, 3800);
    EXPECT_EQ(admitTrusted(makeChild(me, 3900, 1000, 999), &myNode, 0), ValidityResult::RejectStale);

    const unsigned int cap = ANT_MAX_CHILDREN_PER_PARENT;
    if (cap != 0)
    {
        EXPECT_EQ(admitTrusted(makeChild(me, 3900), &myNode, cap),
            ValidityResult::RejectMaxChildrenPerParent);
    }
}

// Commits one root child with no network, the way an AUX node commits a solution it took on trust.
static long long commitRootChildWithoutAnn(AntColonyBpp9000T* colony, const m256i& owner,
    unsigned int score, unsigned int txIdx, unsigned long long nonceSeed, unsigned int tick = 100000)
{
    AntCommitInput in;
    in.pubkey = owner;
    in.nonce = makeKey(nonceSeed);
    in.parentRef = ROOT_REF;
    in.selfRef.tick = tick;
    in.selfRef.solutionIndexInTick = txIdx;
    in.anchorTick = tick;
    in.publishTick = tick;

    const long long landsAt = (long long)colony->solutionCount();
    if (colony->commit(in, nullptr, score_engine::Rating{ score, 0 }, nullptr, 0, true) != ValidityResult::Valid)
    {
        return ANT_INVALID_INDEX;
    }
    return landsAt;
}

// The record is addressable but has no network yet, so every reader must see that rather than pool garbage.
TEST(TestAntColonyStore, RecordCommitsWithoutItsNetwork)
{
    AntColonyBpp9000T* colony = freshColony();
    ASSERT_NE(colony, nullptr) << "colony init failed; needs ~6.2 GB";

    const m256i me = makeKey(30);
    const long long idx = commitRootChildWithoutAnn(colony, me, 3800, 0, 700);
    ASSERT_NE(idx, ANT_INVALID_INDEX);

    EXPECT_FALSE(colony->isAnnMaterialised((unsigned int)idx));
    EXPECT_EQ(colony->recordAt(idx)->annStateSlot, ANT_ANN_UNMATERIALISED);
    EXPECT_EQ(colony->recordAt(idx)->score, 3800u);

    AntColonyBpp9000T::Ann out;
    EXPECT_FALSE(colony->annOfNonRoot(*colony->recordAt(idx), out));
}

// One claim wins and the loser is told to wait; the published network reads back byte for byte.
TEST(TestAntColonyStore, ClaimThenPublishSuppliesTheNetwork)
{
    AntColonyBpp9000T* colony = freshColony();
    ASSERT_NE(colony, nullptr) << "colony init failed; needs ~6.2 GB";

    const m256i me = makeKey(31);
    const long long idx = commitRootChildWithoutAnn(colony, me, 3800, 0, 701);
    ASSERT_NE(idx, ANT_INVALID_INDEX);
    const unsigned int slot = (unsigned int)idx;

    ASSERT_EQ(colony->tryClaimAnn(slot), AntColonyBpp9000T::AnnClaimOwned);
    EXPECT_TRUE(colony->isAnnClaimHeld(slot));
    EXPECT_EQ(colony->tryClaimAnn(slot), AntColonyBpp9000T::AnnClaimBusy);

    // A claim that produces nothing must be releasable, or the slot is never rebuildable again.
    colony->releaseAnnClaim(slot);
    EXPECT_FALSE(colony->isAnnClaimHeld(slot));
    ASSERT_EQ(colony->tryClaimAnn(slot), AntColonyBpp9000T::AnnClaimOwned);

    AntColonyBpp9000T::Ann rebuilt;
    setMem(&rebuilt, sizeof(rebuilt), 0);
    rebuilt.lut[0] = 2;
    rebuilt.lut[5] = 1;
    unsigned int annHash;
    KangarooTwelve(&rebuilt, sizeof(rebuilt), &annHash, sizeof(annHash));
    colony->publishAnn(slot, rebuilt, annHash, 0);

    EXPECT_TRUE(colony->isAnnMaterialised(slot));
    EXPECT_EQ(colony->tryClaimAnn(slot), AntColonyBpp9000T::AnnClaimReady);
    EXPECT_EQ(colony->recordAt(idx)->childAnnHash, annHash);

    AntColonyBpp9000T::Ann out;
    ASSERT_TRUE(colony->annOfNonRoot(*colony->recordAt(idx), out));
    EXPECT_EQ(memcmp(&out, &rebuilt, sizeof(out)), 0);
}

// A record with no network must survive the round trip, since the loader re-derives childAnnHash from
// a network that is not there. A claim saved mid-rebuild must come back rebuildable, not stuck.
TEST(TestAntColonySnapshot, UnmaterialisedRecordsSurviveTheRoundTrip)
{
    AntColonyBpp9000T* colony = freshColony();
    ASSERT_NE(colony, nullptr) << "colony init failed; needs ~6.2 GB";

    const m256i me = makeKey(32);
    ASSERT_NE(commitRootChild(colony, me, 3800, 0, 800), ANT_INVALID_INDEX);
    ASSERT_NE(commitRootChildWithoutAnn(colony, me, 3810, 1, 801), ANT_INVALID_INDEX);
    ASSERT_NE(commitRootChildWithoutAnn(colony, me, 3820, 2, 802), ANT_INVALID_INDEX);

    // The third one is saved mid-rebuild.
    ASSERT_EQ(colony->tryClaimAnn(2), AntColonyBpp9000T::AnnClaimOwned);

    ASSERT_TRUE(saveWipeLoad(colony));

    ASSERT_EQ(colony->solutionCount(), 3u);
    EXPECT_TRUE(colony->isAnnMaterialised(0));
    EXPECT_FALSE(colony->isAnnMaterialised(1));

    EXPECT_FALSE(colony->isAnnMaterialised(2));
    EXPECT_FALSE(colony->isAnnClaimHeld(2));
    EXPECT_EQ(colony->tryClaimAnn(2), AntColonyBpp9000T::AnnClaimOwned);

    // The tree itself is intact, so these records still resolve and still parent children.
    EXPECT_EQ(colony->recordAt(1)->score, 3810u);
    const SolutionRef ref = { TEST_PUBLISH_TICK, 2 };
    EXPECT_EQ(colony->findIndexBySolutionRef(ref), 2LL);
}

// A child of an existing node committed on trust: no network, the way an AUX node stores one.
static long long commitChildWithoutAnn(AntColonyBpp9000T* colony, const m256i& owner, const SolutionRef& parentRef, unsigned int score,
    unsigned int txIdx, unsigned long long nonceSeed, unsigned int tick = 100000)
{
    const AntSolutionRecord* parentRec = nullptr;
    if (colony->tryGetParent(parentRef, &parentRec) != ValidityResult::Valid)
    {
        return ANT_INVALID_INDEX;
    }

    AntCommitInput in;
    in.pubkey = owner;
    in.nonce = makeKey(nonceSeed);
    in.parentRef = parentRef;
    in.selfRef.tick = tick;
    in.selfRef.solutionIndexInTick = txIdx;
    in.anchorTick = tick;
    in.publishTick = tick;

    const long long landsAt = (long long)colony->solutionCount();
    if (colony->commit(in, parentRec, score_engine::Rating{ score, 0 }, nullptr, 0, true) != ValidityResult::Valid)
    {
        return ANT_INVALID_INDEX;
    }
    return landsAt;
}

// A claim held at fork time has no owner in the child, and the on-demand waiter would spin on it.
TEST(TestAntColonyMaintenance, PromoteReleasesAnInheritedClaim)
{
    AntColonyBpp9000T* colony = freshColony();
    ASSERT_NE(colony, nullptr) << "colony init failed; needs ~6.2 GB";

    const m256i me = makeKey(41);
    const long long idx = commitRootChildWithoutAnn(colony, me, 3800, 0, 901);
    ASSERT_NE(idx, ANT_INVALID_INDEX);
    const unsigned int slot = (unsigned int)idx;

    ASSERT_EQ(colony->tryClaimAnn(slot), AntColonyBpp9000T::AnnClaimOwned);
    ASSERT_TRUE(colony->isAnnClaimHeld(slot));

    EXPECT_EQ(AntColonyMaintenance::releaseInheritedClaims(*colony), 1u);
    EXPECT_FALSE(colony->isAnnClaimHeld(slot));
    // Retryable, not merely unclaimed: a Busy here is the hang the sweep exists to prevent.
    EXPECT_EQ(colony->tryClaimAnn(slot), AntColonyBpp9000T::AnnClaimOwned);
    colony->releaseAnnClaim(slot);

    EXPECT_EQ(AntColonyMaintenance::releaseInheritedClaims(*colony), 0u);
}

// A rebuild starts from the parent's network, so a record whose parent has none cannot be taken yet.
TEST(TestAntColonyMaintenance, RebuildableOnlyOnceTheParentHasItsNetwork)
{
    AntColonyBpp9000T* colony = freshColony();
    ASSERT_NE(colony, nullptr) << "colony init failed; needs ~6.2 GB";

    const m256i me = makeKey(42);
    const long long parentIdx = commitRootChildWithoutAnn(colony, me, 3900, 0, 902);
    ASSERT_NE(parentIdx, ANT_INVALID_INDEX);
    SolutionRef parentRef;
    parentRef.tick = 100000;
    parentRef.solutionIndexInTick = 0;
    const long long childIdx = commitChildWithoutAnn(colony, me, parentRef, 3800, 1, 903);
    ASSERT_NE(childIdx, ANT_INVALID_INDEX);

    // The root is closed-form, so the parent is takeable immediately and the child is not.
    EXPECT_TRUE(AntColonyMaintenance::isRebuildableNow(*colony, (unsigned int)parentIdx));
    EXPECT_FALSE(AntColonyMaintenance::isRebuildableNow(*colony, (unsigned int)childIdx));

    AntColonyBpp9000T::Ann ann;
    setMem(&ann, sizeof(ann), 0);
    unsigned int annHash;
    KangarooTwelve(&ann, sizeof(ann), &annHash, sizeof(annHash));
    ASSERT_EQ(colony->tryClaimAnn((unsigned int)parentIdx), AntColonyBpp9000T::AnnClaimOwned);
    colony->publishAnn((unsigned int)parentIdx, ann, annHash, 0);

    // Materialised records are done, and the level below is now unblocked.
    EXPECT_FALSE(AntColonyMaintenance::isRebuildableNow(*colony, (unsigned int)parentIdx));
    EXPECT_TRUE(AntColonyMaintenance::isRebuildableNow(*colony, (unsigned int)childIdx));
}

// Two rebuilders must not walk the same record: the claim is what keeps the second one moving on.
TEST(TestAntColonyMaintenance, AClaimedRecordIsNotOfferedAgain)
{
    AntColonyBpp9000T* colony = freshColony();
    ASSERT_NE(colony, nullptr) << "colony init failed; needs ~6.2 GB";

    const m256i me = makeKey(43);
    const long long idx = commitRootChildWithoutAnn(colony, me, 3800, 0, 904);
    ASSERT_NE(idx, ANT_INVALID_INDEX);

    EXPECT_TRUE(AntColonyMaintenance::isRebuildableNow(*colony, (unsigned int)idx));
    ASSERT_EQ(colony->tryClaimAnn((unsigned int)idx), AntColonyBpp9000T::AnnClaimOwned);
    EXPECT_FALSE(AntColonyMaintenance::isRebuildableNow(*colony, (unsigned int)idx));

    colony->releaseAnnClaim((unsigned int)idx);
    EXPECT_TRUE(AntColonyMaintenance::isRebuildableNow(*colony, (unsigned int)idx));
}

// A rebuild that reaches past the guess re-ranks its miner once; one that confirms the guess ranks nothing.
TEST(TestAntColonyMaintenance, RaisedShiftIsRankedOnce)
{
    AntColonyBpp9000T* colony = freshColony();
    ASSERT_NE(colony, nullptr) << "colony init failed; needs ~6.2 GB";
    AntColonyMaintenance::clearRaisedShifts();

    const m256i me = makeKey(44);
    const long long raisedIdx = commitRootChildWithoutAnn(colony, me, 3800, 0, 905);
    const long long confirmedIdx = commitRootChildWithoutAnn(colony, me, 3810, 1, 906);
    ASSERT_NE(raisedIdx, ANT_INVALID_INDEX);
    ASSERT_NE(confirmedIdx, ANT_INVALID_INDEX);

    AntColonyBpp9000T::Ann ann;
    setMem(&ann, sizeof(ann), 0);
    unsigned int annHash;
    KangarooTwelve(&ann, sizeof(ann), &annHash, sizeof(annHash));

    ASSERT_EQ(colony->tryClaimAnn((unsigned int)raisedIdx), AntColonyBpp9000T::AnnClaimOwned);
    AntColonyMaintenance::publishRebuilt(*colony, (unsigned int)raisedIdx, ann, annHash, score_engine::Rating{ 3800, 3 });
    ASSERT_EQ(colony->tryClaimAnn((unsigned int)confirmedIdx), AntColonyBpp9000T::AnnClaimOwned);
    AntColonyMaintenance::publishRebuilt(*colony, (unsigned int)confirmedIdx, ann, annHash, score_engine::Rating{ 3810, 0 });

    EXPECT_TRUE(colony->isAnnMaterialised((unsigned int)raisedIdx));
    EXPECT_EQ(colony->recordAt(raisedIdx)->shift, 3u);
    EXPECT_EQ(colony->recordAt(confirmedIdx)->shift, 0u);

    unsigned int calls = 0;
    unsigned int rankedKey = 0;
    unsigned int rankedTick = 0;
    m256i rankedMiner = m256i::zero();
    const auto rank = [&](const m256i& publicKey, unsigned int rankingKey, unsigned int tick)
    {
        calls++;
        rankedMiner = publicKey;
        rankedKey = rankingKey;
        rankedTick = tick;
    };

    EXPECT_EQ(AntColonyMaintenance::drainRaisedShifts(*colony, rank), 1u);
    EXPECT_EQ(calls, 1u);
    EXPECT_TRUE(rankedMiner == me);
    EXPECT_EQ(rankedKey, (score_engine::Rating{ 3800, 3 }).rankingKey());
    EXPECT_EQ(rankedTick, 100000u);

    EXPECT_EQ(AntColonyMaintenance::drainRaisedShifts(*colony, rank), 0u);
    EXPECT_EQ(calls, 1u);
}
