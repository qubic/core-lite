// Lite-only tests of reading prediction-phase qualification off a claimed ant score. Built as one translation
// unit with the upstream score tests: the engine helpers they use are file-static.
#include "score.cpp"

#include "../src/extensions/ant_prediction_claim.h"

#include <algorithm>
#include <random>

namespace
{
constexpr unsigned int GATE = BPP9000_ADVANCE_THRESHOLD;
constexpr unsigned int FLOOR = BPP9000_PREDICTION_THRESHOLD;
constexpr unsigned int NO_ROOT_ERROR = 0xFFFFFFFFU;

// rootError == NO_ROOT_ERROR: the root's error must not be asked for.
AntClaimInference inferClaim(unsigned int claimedError, unsigned int shiftLimit, bool parentIsRoot, unsigned int parentShift,
    unsigned int parentError, unsigned int rootError = NO_ROOT_ERROR)
{
    return antQualifiesFromClaim(claimedError, shiftLimit, parentIsRoot, parentShift, parentError, [&]() {
        EXPECT_NE(rootError, NO_ROOT_ERROR) << "the root was replayed for a claim that needs no replay";
        return rootError;
    });
}

void expectInference(const AntClaimInference& inference, AntClaimVerdict verdict, unsigned int rule, unsigned int shiftLowerBound)
{
    EXPECT_EQ((int)inference.verdict, (int)verdict);
    EXPECT_EQ((unsigned int)inference.rule, rule);
    EXPECT_EQ(inference.shiftLowerBound, shiftLowerBound);
}
}

TEST(TestAntPredictionClaim, ErrorAtTheFloorQualifiesWhateverTheParent)
{
    expectInference(inferClaim(FLOOR, 0, true, 0, 0), AntClaimVerdict::Qualifies, 1, 0);
    expectInference(inferClaim(0, 5, true, 0, 0), AntClaimVerdict::Qualifies, 1, 0);
    expectInference(inferClaim(FLOOR, 5, false, 0, FLOOR + 40), AntClaimVerdict::Qualifies, 1, 0);
    expectInference(inferClaim(FLOOR, 5, false, 3, FLOOR + 40), AntClaimVerdict::Qualifies, 1, 3);
}

TEST(TestAntPredictionClaim, ParentPastFrameZeroQualifies)
{
    expectInference(inferClaim(GATE + 500, 5, false, 1, GATE + 600), AntClaimVerdict::Qualifies, 2, 1);
    expectInference(inferClaim(GATE + 1, 0, false, 7, GATE), AntClaimVerdict::Qualifies, 2, 7);
    // A root sits at frame 0: the shift argument of a root parent is not read.
    expectInference(inferClaim(GATE + 500, 0, true, 9, 0), AntClaimVerdict::DoesNotQualify, 3, 0);
}

TEST(TestAntPredictionClaim, NoFrameToSlideToDoesNotQualify)
{
    expectInference(inferClaim(FLOOR + 1, 0, true, 0, 0), AntClaimVerdict::DoesNotQualify, 3, 0);
    expectInference(inferClaim(GATE + 1, 0, false, 0, GATE), AntClaimVerdict::DoesNotQualify, 3, 0);
    expectInference(inferClaim(GATE + 900, 0, false, 0, GATE + 1), AntClaimVerdict::DoesNotQualify, 3, 0);
}

TEST(TestAntPredictionClaim, OneErrorPastTheGateIsUndecided)
{
    if (FLOOR >= GATE + 1)
    {
        GTEST_SKIP() << "the floor takes this error first";
    }
    expectInference(inferClaim(GATE + 1, 1, true, 0, 0), AntClaimVerdict::Undecided, 4, 0);
    expectInference(inferClaim(GATE + 1, 1, false, 0, GATE + 2), AntClaimVerdict::Undecided, 4, 0);
    expectInference(inferClaim(GATE + 1, BPP9000_SHIFT_CAP, false, 0, GATE + 900), AntClaimVerdict::Undecided, 4, 0);
}

TEST(TestAntPredictionClaim, NonRootParentAtFrameZero)
{
    const unsigned int claim = std::max(GATE + 2, FLOOR + 1);

    // The parent masters its frame: the walk slides before its first step.
    expectInference(inferClaim(claim, 1, false, 0, GATE), AntClaimVerdict::Undecided, 5, 0);
    expectInference(inferClaim(claim + 300, 1, false, 0, 0), AntClaimVerdict::Undecided, 5, 0);

    // Not fewer errors than the parent: accepted only at a higher shift.
    expectInference(inferClaim(claim, 1, false, 0, GATE + 1), AntClaimVerdict::Qualifies, 6, 1);
    expectInference(inferClaim(claim, 1, false, 0, claim), AntClaimVerdict::Qualifies, 6, 1);
    expectInference(inferClaim(claim + 10, 1, false, 0, claim), AntClaimVerdict::Qualifies, 6, 1);

    // Fewer errors than a parent that started clear of the gate: the frame never moved.
    expectInference(inferClaim(claim, 1, false, 0, claim + 1), AntClaimVerdict::DoesNotQualify, 7, 0);
    expectInference(inferClaim(claim, BPP9000_SHIFT_CAP, false, 0, claim + 2000), AntClaimVerdict::DoesNotQualify, 7, 0);
}

TEST(TestAntPredictionClaim, RootParent)
{
    const unsigned int claim = std::max(GATE + 2, FLOOR + 1);

    expectInference(inferClaim(claim, 1, true, 0, 0, GATE + 2), AntClaimVerdict::DoesNotQualify, 8, 0);
    expectInference(inferClaim(claim + 700, BPP9000_SHIFT_CAP, true, 0, 0, GATE + 900), AntClaimVerdict::DoesNotQualify, 8, 0);
    // A root child may end with more errors than its root.
    expectInference(inferClaim(claim + 700, 1, true, 0, 0, GATE + 2), AntClaimVerdict::DoesNotQualify, 8, 0);

    expectInference(inferClaim(claim, 1, true, 0, 0, GATE + 1), AntClaimVerdict::Undecided, 9, 0);
    expectInference(inferClaim(claim, 1, true, 0, 0, GATE), AntClaimVerdict::Undecided, 9, 0);
    expectInference(inferClaim(claim, 1, true, 0, 0, 0), AntClaimVerdict::Undecided, 9, 0);
}

// The rating a trusted solution is stored with must pass or fail upstream's floor test exactly as the verdict says,
// since PredictionPhase::needsAnswers() reads that rating, not the verdict.
TEST(TestAntPredictionClaim, TrustedRatingAgreesWithTheVerdict)
{
    auto phase = std::make_unique<PredictionPhase>();
    phase->reset();
    const m256i identity = makePubkey(77);

    const unsigned int errors[] = { 0, GATE - 1, GATE, GATE + 1, GATE + 2, GATE + 3, FLOOR, FLOOR + 1, FLOOR + 2, GATE + 800 };
    const unsigned int limits[] = { 0, 1, 2, (unsigned int)BPP9000_SHIFT_CAP };
    const unsigned int shifts[] = { 0, 1, 5 };
    unsigned int decided = 0;
    for (unsigned int claim : errors)
    {
        for (unsigned int limit : limits)
        {
            for (unsigned int parentShift : shifts)
            {
                for (unsigned int parentError : errors)
                {
                    for (int parentIsRoot = 0; parentIsRoot < 2; parentIsRoot++)
                    {
                        const AntClaimInference inference =
                            inferClaim(claim, limit, parentIsRoot != 0, parentShift, parentError, parentError);
                        if (inference.verdict == AntClaimVerdict::Undecided)
                        {
                            continue;
                        }
                        decided++;
                        const score_engine::Rating trusted{ claim, inference.shiftLowerBound };
                        const bool qualifies = inference.verdict == AntClaimVerdict::Qualifies;
                        EXPECT_EQ(trusted.clearsFloor(BPP9000_PREDICTION_THRESHOLD), qualifies)
                            << "claim " << claim << " limit " << limit << " parent " << parentError << "@" << parentShift
                            << " root " << parentIsRoot << " rule " << (int)inference.rule;
                        EXPECT_EQ(phase->needsAnswers(identity, trusted), qualifies);
                    }
                }
            }
        }
    }
    EXPECT_GT(decided, 0u);
}

TEST(TestAntPredictionClaim, UncomputedAnswersAreFound)
{
    auto phase = std::make_unique<PredictionPhase>();
    phase->reset();
    EXPECT_FALSE(predictionHasUncomputedAnswers(*phase));

    std::vector<unsigned char> answers(PREDICTION_MAX_HOURS, 1);
    const score_engine::Rating rating{ BPP9000_PREDICTION_THRESHOLD, 0 };
    phase->recordSolution(makePubkey(1), rating, 100, 0, 0, 0, false, answers.data());
    phase->recordSolution(makePubkey(2), rating, 101, 0, 0, 0, false, answers.data());
    EXPECT_FALSE(predictionHasUncomputedAnswers(*phase));

    // Nothing past count is an entry.
    phase->entries[2].answers[0] = PREDICTION_ANSWER_NOT_COMPUTED;
    EXPECT_FALSE(predictionHasUncomputedAnswers(*phase));

    phase->entries[1].answers[PREDICTION_MAX_HOURS - 1] = PREDICTION_ANSWER_NOT_COMPUTED;
    EXPECT_TRUE(predictionHasUncomputedAnswers(*phase));

    static_assert(PREDICTION_ANSWER_NOT_COMPUTED != PREDICTION_NO_ANSWER, "an uncomputed answer must not read as a missing output");
    static_assert(PREDICTION_ANSWER_NOT_COMPUTED > 1, "an uncomputed answer must never equal a target");
}

// =============================================================================
// The rules against the real walk

namespace
{
// The production gate and floor on a shorter frame. A root leaves about a third of its hours unanswered, so
// against random targets it starts near the gate, and mutations carry a walk across it in both directions.
using ClaimWalkConfig = score_engine::Bpp9000Params<
    BPP9000_NUMBER_OF_INPUT_NEURONS, BPP9000_NUMBER_OF_OUTPUT_NEURONS, BPP9000_SEQUENCE_LENGTH,
    5400, BPP9000_MAX_NUMBER_OF_TICKS, BPP9000_NUMBER_OF_NEIGHBORS,
    64, 24, BPP9000_ADVANCE_THRESHOLD + 800, BPP9000_SHIFT_CAP>;

struct ClaimWalkStats
{
    unsigned int walks = 0;
    unsigned int valid = 0;
    unsigned int accepted = 0;
    unsigned int acceptedAfterExplore = 0;
    unsigned int rule[10] = {};
    // Walks whose start was clear of the gate, and those of them that slid anyway.
    unsigned int clearStart = 0;
    unsigned int clearStartSlid = 0;
    unsigned int clearStartUnmoved = 0;
    unsigned int startedPastFrameZero = 0;
    unsigned int slidAfterExplore = 0;
};

// Random targets, adjusted on frame 0 until the network misses exactly firstFrameErrors of its hours.
template<typename Cfg>
bool makeTargetsAround(score_engine::ScoreBpp9000<Cfg>& engine, const typename score_engine::ScoreBpp9000<Cfg>::ANN& ann,
    unsigned int firstFrameErrors, std::mt19937& rng, std::vector<unsigned char>& targets)
{
    using Engine = score_engine::ScoreBpp9000<Cfg>;
    std::vector<unsigned char> outputs(Cfg::sequenceLength);
    engine.collectOutputs(ann, 0, Cfg::sequenceLength, outputs.data());

    targets.resize(Cfg::sequenceLength);
    std::vector<unsigned int> hits;
    std::vector<unsigned int> misses;
    unsigned int errors = 0;
    for (unsigned int t = 0; t < Cfg::sequenceLength; ++t)
    {
        targets[t] = (unsigned char)(rng() & 1);
        if (t >= Cfg::windowWidth)
        {
            continue;
        }
        if (outputs[t] >= Engine::TRIT_UNKNOWN)
        {
            // An hour without an output misses either target.
            errors++;
        }
        else if (outputs[t] == targets[t])
        {
            hits.push_back(t);
        }
        else
        {
            errors++;
            misses.push_back(t);
        }
    }

    std::vector<unsigned int>& pool = (errors < firstFrameErrors) ? hits : misses;
    const unsigned int flips = (errors < firstFrameErrors) ? (firstFrameErrors - errors) : (errors - firstFrameErrors);
    if (flips > pool.size())
    {
        return false;
    }
    std::shuffle(pool.begin(), pool.end(), rng);
    for (unsigned int i = 0; i < flips; ++i)
    {
        targets[pool[i]] ^= 1;
    }
    return true;
}

testing::Message describeWalk(const score_engine::Rating& child, const score_engine::Rating& start, bool parentIsRoot,
    unsigned int limit, unsigned int explore)
{
    return testing::Message() << "child " << child.error << "@" << child.shift << " start " << (parentIsRoot ? "root " : "")
        << start.error << "@" << start.shift << " limit " << limit << " explore " << explore;
}

// What the rules rest on, on any walk that returned a rating, accepted or not.
void checkWalkAgainstRules(const score_engine::Rating& child, const score_engine::Rating& start, bool parentIsRoot, unsigned int limit,
    unsigned int explore, ClaimWalkStats& stats)
{
    stats.valid++;
    EXPECT_GE(child.shift, start.shift) << describeWalk(child, start, parentIsRoot, limit, explore);
    EXPECT_LE(child.shift, limit) << describeWalk(child, start, parentIsRoot, limit, explore);
    if (start.shift > 0)
    {
        stats.startedPastFrameZero++;
    }
    if (child.shift > start.shift && explore != 0)
    {
        stats.slidAfterExplore++;
    }

    if (start.error < GATE + 2 || limit == start.shift)
    {
        return;
    }
    stats.clearStart++;
    if (child.shift > start.shift)
    {
        stats.clearStartSlid++;
        EXPECT_LE(child.error, GATE + 1) << describeWalk(child, start, parentIsRoot, limit, explore);
    }
    else
    {
        stats.clearStartUnmoved++;
    }
}

// One accepted solution against the verdict its claim gets.
void checkClaimAgainstWalk(const score_engine::Rating& child, const score_engine::Rating& parent, bool parentIsRoot, unsigned int limit,
    unsigned int explore, ClaimWalkStats& stats)
{
    const AntClaimInference inference = antQualifiesFromClaim(child.error, limit, parentIsRoot, parent.shift, parent.error,
        [&]() { return parent.error; });
    const bool qualifies = child.clearsFloor(BPP9000_PREDICTION_THRESHOLD);

    stats.accepted++;
    stats.rule[inference.rule]++;
    if (explore != 0)
    {
        stats.acceptedAfterExplore++;
    }

    EXPECT_LE(inference.shiftLowerBound, child.shift) << describeWalk(child, parent, parentIsRoot, limit, explore);
    if (inference.verdict == AntClaimVerdict::Qualifies)
    {
        EXPECT_TRUE(qualifies) << "rule " << (int)inference.rule << " " << describeWalk(child, parent, parentIsRoot, limit, explore);
    }
    if (inference.verdict == AntClaimVerdict::DoesNotQualify)
    {
        EXPECT_FALSE(qualifies) << "rule " << (int)inference.rule << " " << describeWalk(child, parent, parentIsRoot, limit, explore);
    }
}
}

// Chains of real walks around the gate. Every accepted solution's verdict must be the truth or undecided.
TEST(TestAntPredictionClaimWalk, VerdictHoldsOnTheRealWalk)
{
    using Cfg = ClaimWalkConfig;
    using Engine = score_engine::ScoreBpp9000<Cfg>;

    AntFixtureT<Cfg> f;
    ASSERT_TRUE(makeAntFixtureT<Cfg>(f));
    Engine& engine = *f.engine;

    std::mt19937 rng(20261124u);
    ClaimWalkStats stats;
    const m256i anchor = makePubkey(200);

    const int rootErrorOffsets[] = { -30, 0, 1, 2, 3, 8, 25, 60, 120, 200, 320, 480 };
    const unsigned int limits[] = { 0, 1, 4, (unsigned int)Cfg::shiftCap };
    const unsigned char exploreSteps[] = { 0, 0, 2, 6, 12, (unsigned char)Cfg::numberOfMutations };
    constexpr unsigned int IDENTITIES = 3;
    constexpr unsigned int CHAIN_DEPTH = 4;

    auto root = std::make_unique<Engine::ANN>();
    auto parentAnn = std::make_unique<Engine::ANN>();
    std::vector<unsigned char> targets;
    unsigned char tag = 0;

    for (unsigned int identity = 0; identity < IDENTITIES; ++identity)
    {
        const m256i pk = makePubkey((unsigned char)(101 + identity));
        engine.deriveRootANN(pk.m256i_u8, f.pool.data(), *root);

        for (int offset : rootErrorOffsets)
        {
            const unsigned int rootError = (unsigned int)((int)GATE + offset);
            ASSERT_TRUE(makeTargetsAround<Cfg>(engine, *root, rootError, rng, targets)) << "identity " << identity;
            ASSERT_TRUE(engine.loadTaskTargets(targets.data(), targets.size()));

            for (unsigned int firstLimit : limits)
            {
                // A chain of solutions, each anchored where at least as many hours are known as for its parent.
                bool parentIsRoot = true;
                score_engine::Rating parent{ rootError, 0 };
                unsigned int limit = firstLimit;
                *parentAnn = *root;

                for (unsigned int depth = 0; depth < CHAIN_DEPTH; ++depth)
                {
                    const unsigned char explore = exploreSteps[rng() % (sizeof(exploreSteps) / sizeof(exploreSteps[0]))];
                    const unsigned char changes = (unsigned char)(1 + rng() % score_engine::BPP9000_MAX_CHANGES_PER_STEP);
                    const m256i nonce = makeAntNonce(changes, explore, ++tag);

                    const score_engine::Rating child = engine.computeScoreFromParent(
                        *parentAnn, parent.shift, pk.m256i_u8, nonce.m256i_u8, anchor.m256i_u8, f.pool.data(), limit);
                    stats.walks++;
                    if (!child.isValid())
                    {
                        continue;
                    }

                    // A start that does not master its frame is rated as it is stored: nothing slides before the first step.
                    const score_engine::Rating start = engine.parentRating;
                    if (parent.error > GATE)
                    {
                        EXPECT_EQ(start.error, parent.error);
                        EXPECT_EQ(start.shift, parent.shift);
                    }
                    if (start.isValid())
                    {
                        checkWalkAgainstRules(child, start, parentIsRoot, limit, explore, stats);
                    }

                    // What a node that walks accepts and refunds.
                    bool accepted = child.clearsFloor((unsigned int)Cfg::solutionThreshold);
                    if (!parentIsRoot)
                    {
                        accepted = accepted && child.isBetterThan(start) && child.isBetterThan(parent);
                    }
                    if (!accepted)
                    {
                        continue;
                    }
                    checkClaimAgainstWalk(child, parent, parentIsRoot, limit, explore, stats);

                    engine.getBestANN(*parentAnn);
                    parent = child;
                    parentIsRoot = false;
                    if (limit < Cfg::shiftCap && (rng() & 1))
                    {
                        limit = std::min<unsigned int>((unsigned int)Cfg::shiftCap, limit + 1 + rng() % 3);
                    }
                }
            }
        }
    }

    std::cout << "[claim walk] walks " << stats.walks << " valid " << stats.valid << " accepted " << stats.accepted << " (after explore "
        << stats.acceptedAfterExplore << ") | clear start " << stats.clearStart << ": slid " << stats.clearStartSlid << " unmoved "
        << stats.clearStartUnmoved << " | past frame 0 " << stats.startedPastFrameZero << " | slid after explore "
        << stats.slidAfterExplore << " | rules";
    for (unsigned int r = 1; r <= 9; ++r)
    {
        std::cout << " " << r << ":" << stats.rule[r];
    }
    std::cout << std::endl;

    // A pass must not be an empty one: the rules that read a frame as unmoved were reached, next to walks that did slide.
    // A parent past frame 0 qualifies its child because no walk ends below its start, which every walk above checks.
    EXPECT_GT(stats.acceptedAfterExplore, 0u);
    EXPECT_GT(stats.slidAfterExplore, 0u);
    EXPECT_GT(stats.startedPastFrameZero, 0u);
    EXPECT_GT(stats.clearStartSlid, 0u);
    EXPECT_GT(stats.clearStartUnmoved, 0u);
    EXPECT_GT(stats.rule[1], 0u);
    EXPECT_GT(stats.rule[3], 0u);
    EXPECT_GT(stats.rule[7], 0u);
    EXPECT_GT(stats.rule[8], 0u);
}

// The walk slides before its first step. A parent that masters frames 0 and 1 and times out in frame 2 leaves the
// frame at 2 with no rating, and any later step that returns one is kept there. Such a child sits past frame 0
// with more errors than the floor allows: the case the undecided verdicts exist for.
TEST(TestAntPredictionClaimWalk, TimedOutSlideLeavesTheFrameMoved)
{
    using Cfg = TimeoutConfig;
    using Engine = score_engine::ScoreBpp9000<Cfg>;

    std::vector<m256i> seeds;
    std::vector<m256i> pubkeys;
    std::vector<m256i> nonces;
    loadSamples(seeds, pubkeys, nonces, 1);
    ASSERT_FALSE(seeds.empty());
    std::vector<unsigned char> pool;
    generatePool(seeds[0], pool);

    // An output of 0 has GATE errors on frames 0 and 1, and one more on frame 2.
    const std::vector<unsigned char> inputs(Cfg::sequenceLength * Cfg::numberOfInputNeurons, 0);
    std::vector<unsigned char> targets(Cfg::sequenceLength, 0);
    for (unsigned long long t = 2; t < 2 + GATE; ++t)
    {
        targets[t] = 1;
    }
    for (unsigned long long t = Cfg::windowWidth + 1; t < Cfg::sequenceLength; ++t)
    {
        targets[t] = 1;
    }
    std::vector<unsigned char> data(score_task_file::dataBytes((unsigned int)Cfg::numberOfInputNeurons,
        (unsigned int)Cfg::numberOfOutputNeurons, Cfg::sequenceLength));
    score_task_file::packDataBlock((unsigned int)Cfg::numberOfInputNeurons, (unsigned int)Cfg::numberOfOutputNeurons,
        Cfg::sequenceLength, inputs.data(), targets.data(), data.data());

    auto engine = makeEngine<Cfg>(nullptr, data.data());
    ASSERT_TRUE(engine);
    engine->deriveControlOutput(seeds[0].m256i_u8, pool.data());
    const unsigned int control = engine->controlIndex;
    const unsigned int output = engine->outputIndex;

    // Control alternates 0, UNKNOWN; output is constant 0.
    const m256i pk = makePubkey(41);
    const m256i anchor = makePubkey(42);
    auto parent = std::make_unique<Engine::ANN>();
    engine->deriveRootANN(pk.m256i_u8, pool.data(), *parent);
    for (unsigned long long j = 0; j < Cfg::numberOfNeighbors; ++j)
    {
        parent->neighbor[control * Cfg::numberOfNeighbors + j] = (unsigned short)control;
    }
    parent->initialNeuronValues[control] = 0;
    parent->lut[control * Engine::lutSize + 0] = 2;
    parent->lut[control * Engine::lutSize + 26] = 0;
    for (unsigned long long e = 0; e < Engine::lutSize; ++e)
    {
        parent->lut[output * Engine::lutSize + e] = 0;
    }

    // Anchored where no later frame is known, the parent is stored with its frame-0 rating.
    const score_engine::Rating stored{ GATE, 0 };
    engine->expand(*parent);
    engine->shift = 0;
    engine->shiftLimit = 0;
    const score_engine::Rating atFrameZero = engine->advanceShift();
    ASSERT_EQ(atFrameZero.error, stored.error);
    ASSERT_EQ(atFrameZero.shift, stored.shift);

    unsigned int movedChildren = 0;
    unsigned int movedPastTheFloor = 0;
    unsigned int rule[10] = {};
    for (unsigned int n = 0; n < 24; ++n)
    {
        const m256i nonce = makeAntNonce((unsigned char)(1 + n % score_engine::BPP9000_MAX_CHANGES_PER_STEP), (unsigned char)(n % 3 == 2 ? 7 : 0),
            (unsigned char)(60 + n));
        const score_engine::Rating child = engine->computeScoreFromParent(*parent, stored.shift, pk.m256i_u8, nonce.m256i_u8,
            anchor.m256i_u8, pool.data(), Cfg::shiftCap);
        ASSERT_FALSE(engine->parentRating.isValid());
        ASSERT_EQ(engine->parentRating.shift, 2u);
        if (!child.isValid())
        {
            continue;
        }
        EXPECT_GE(child.shift, 2u);
        movedChildren++;
        if (child.error > BPP9000_PREDICTION_THRESHOLD)
        {
            movedPastTheFloor++;
        }

        const AntClaimInference inference = antQualifiesFromClaim(child.error, (unsigned int)Cfg::shiftCap, false, stored.shift, stored.error,
            []() { return 0u; });
        rule[inference.rule]++;
        EXPECT_NE((int)inference.verdict, (int)AntClaimVerdict::DoesNotQualify)
            << "child " << child.error << "@" << child.shift << " rule " << (int)inference.rule;
    }
    std::cout << "[claim walk] timed-out slide: children past frame 0: " << movedChildren << ", with more errors than the floor: "
        << movedPastTheFloor << " | rules 1:" << rule[1] << " 4:" << rule[4] << " 5:" << rule[5] << " 6:" << rule[6] << " 7:" << rule[7]
        << std::endl;
    EXPECT_GT(movedPastTheFloor, 0u);
}

// The engine kept for on-demand solution checks must score exactly like the ones processors map to.
TEST(TestAntSolutionCheck, ReservedEngineScoresLikeAProcessorEngine)
{
    AntFixture f;
    ASSERT_TRUE(makeAntFixture(f));
    std::vector<m256i> seeds;
    std::vector<m256i> pubkeys;
    std::vector<m256i> nonces;
    loadSamples(seeds, pubkeys, nonces, 1);
    ASSERT_FALSE(seeds.empty());

    const unsigned int targetCount = (unsigned int)AntCfg::windowWidth + 3;
    const std::vector<unsigned char> targets(f.engine->targetOutputs, f.engine->targetOutputs + AntCfg::sequenceLength);

    auto scoreFunction = std::make_unique<ScoreFunction<1>>();
    scoreFunction->initMemory();
    scoreFunction->initMiningData(seeds[0]);

    const m256i pk = makePubkey(41);
    const m256i nonce = makeAntNonce(3, 0, 43);
    const m256i anchor = makePubkey(42);
    auto processorAnn = std::make_unique<AntEngine::ANN>();
    auto reservedAnn = std::make_unique<AntEngine::ANN>();

    const score_engine::Rating onProcessor =
        scoreFunction->computeAntChildScore(0, nullptr, 0, targets.data(), targetCount, pk, nonce, anchor, *processorAnn);
    const score_engine::Rating onReserved = scoreFunction->computeAntChildScore(ScoreFunction<1>::RESERVED_ENGINE, nullptr, 0, targets.data(),
        targetCount, pk, nonce, anchor, *reservedAnn);

    ASSERT_TRUE(onProcessor.isValid());
    EXPECT_EQ(onReserved.error, onProcessor.error);
    EXPECT_EQ(onReserved.shift, onProcessor.shift);
    EXPECT_EQ(memcmp(reservedAnn.get(), processorAnn.get(), sizeof(*processorAnn)), 0);

    // A child of that network, so the reserved engine is also fed a stored parent.
    const m256i childNonce = makeAntNonce(3, 0, 44);
    auto processorChild = std::make_unique<AntEngine::ANN>();
    auto reservedChild = std::make_unique<AntEngine::ANN>();
    const score_engine::Rating childOnProcessor = scoreFunction->computeAntChildScore(0, processorAnn.get(), onProcessor.shift, targets.data(),
        targetCount, pk, childNonce, anchor, *processorChild);
    const score_engine::Rating childOnReserved = scoreFunction->computeAntChildScore(ScoreFunction<1>::RESERVED_ENGINE, processorAnn.get(),
        onProcessor.shift, targets.data(), targetCount, pk, childNonce, anchor, *reservedChild);
    EXPECT_EQ(childOnReserved.error, childOnProcessor.error);
    EXPECT_EQ(childOnReserved.shift, childOnProcessor.shift);
    if (childOnProcessor.isValid())
    {
        EXPECT_EQ(memcmp(reservedChild.get(), processorChild.get(), sizeof(*processorChild)), 0);
    }
}
