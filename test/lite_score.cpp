// Lite-only score tests. Built as one translation unit with the upstream tests: their helpers are
// file-static.
#include "score.cpp"

namespace
{

// The production task at another frame width and tick budget. The frame-0 floor only has to sit
// between the advance gate and the frame width; the replay never reads it.
template<unsigned long long windowWidth, unsigned long long maxTicks>
using ReplayConfig = score_engine::Bpp9000Params<
    BPP9000_NUMBER_OF_INPUT_NEURONS, BPP9000_NUMBER_OF_OUTPUT_NEURONS, BPP9000_SEQUENCE_LENGTH,
    windowWidth, maxTicks, BPP9000_NUMBER_OF_NEIGHBORS,
    BPP9000_POPULATION_THRESHOLD, BPP9000_NUMBER_OF_MUTATIONS, BPP9000_ADVANCE_THRESHOLD + 1, BPP9000_SHIFT_CAP>;

// A frame narrow enough that some networks clear the advance gate and the frame slides.
using SlidingConfig = ReplayConfig<4500, BPP9000_MAX_NUMBER_OF_TICKS>;
// Narrower still: most networks clear every frame and stop at the shift cap.
using CapReachingConfig = ReplayConfig<3200, BPP9000_MAX_NUMBER_OF_TICKS>;
// A budget near the ticks a replay needs, so some run out in the first frame and some in a later one.
using TightBudgetConfig = ReplayConfig<3200, 4800>;

// Restores the switch however the test leaves.
struct FastReplaySwitch
{
    bool saved;

    FastReplaySwitch()
    {
        saved = LiteBpp9000::fastReplayEnabled;
    }

    ~FastReplaySwitch()
    {
        LiteBpp9000::fastReplayEnabled = saved;
    }
};

struct AntSample
{
    m256i pubkey;
    m256i nonce;
    m256i anchor;
};

// One lineage of the production file per entry, in file order.
struct AntSamples
{
    m256i seed;
    std::vector<std::vector<AntSample>> chains;
};

static AntSamples loadAntSamples(size_t chainCount)
{
    AntSamples samples;
    samples.seed = m256i::zero();
    std::vector<int> chainIds;

    auto rows = readCSV(PRODUCTION_ANT_FILE_NAME);
    for (size_t i = 1; i < rows.size(); ++i)
    {
        const int chainId = std::stoi(trim(rows[i][0]));
        const size_t depth = (size_t)std::stoi(trim(rows[i][1]));

        size_t chainIndex = 0;
        while (chainIndex < chainIds.size() && chainIds[chainIndex] != chainId)
        {
            chainIndex++;
        }
        if (chainIndex == chainIds.size())
        {
            if (chainIds.size() == chainCount)
            {
                continue;
            }
            chainIds.push_back(chainId);
            samples.chains.emplace_back();
        }

        AntSample sample;
        sample.pubkey = hexTo32Bytes(trim(rows[i][2]), 32);
        sample.nonce = hexTo32Bytes(trim(rows[i][3]), 32);
        sample.anchor = hexTo32Bytes(trim(rows[i][4]), 32);
        samples.seed = hexTo32Bytes(trim(rows[i][5]), 32);

        std::vector<AntSample>& chain = samples.chains[chainIndex];
        if (depth >= chain.size())
        {
            chain.resize(depth + 1);
        }
        chain[depth] = sample;
    }
    return samples;
}

template<typename Cfg>
struct ReplayFixture
{
    AntSamples samples;
    std::vector<unsigned char> pool;
    std::vector<unsigned char> taskBytes;
    std::unique_ptr<score_engine::ScoreBpp9000<Cfg>> engine;
};

template<typename Cfg>
static bool makeReplayFixture(size_t chainCount, ReplayFixture<Cfg>& fixture)
{
    fixture.samples = loadAntSamples(chainCount);
    if (fixture.samples.chains.empty())
    {
        ADD_FAILURE() << "missing/empty " << PRODUCTION_ANT_FILE_NAME;
        return false;
    }
    generatePool(fixture.samples.seed, fixture.pool);

    fixture.taskBytes = readBinaryFile(PRODUCTION_TASK_FILE_NAME);
    if (fixture.taskBytes.size() <= sizeof(score_task_file::TaskFileHeader))
    {
        ADD_FAILURE() << "missing/short " << PRODUCTION_TASK_FILE_NAME;
        return false;
    }
    const TaskBlocks blocks = taskSubview<Cfg>(fixture.taskBytes);
    fixture.engine = makeEngine<Cfg>(blocks.topo, blocks.data);
    if (!fixture.engine)
    {
        return false;
    }
    fixture.engine->deriveControlOutput(fixture.samples.seed.m256i_u8, fixture.pool.data());
    return true;
}

struct ReplayCoverage
{
    unsigned long long replays = 0;
    unsigned long long valid = 0;
    unsigned long long timedOut = 0;
    unsigned long long advanced = 0;
    unsigned long long reachedCap = 0;
    // Only the per-frame kernel shows this: it moves the shift before the frame that times out.
    unsigned long long timedOutPastFirstFrame = 0;
};

// Replays the same drifting networks through the engine's own kernel and through the fast replay,
// from several starting shifts.
template<typename Cfg>
static ReplayCoverage compareReplays(size_t chainCount, unsigned int stepsPerChain)
{
    using Engine = score_engine::ScoreBpp9000<Cfg>;
    ReplayCoverage coverage;
    FastReplaySwitch restoreSwitch;

    ReplayFixture<Cfg> fixture;
    if (!makeReplayFixture<Cfg>(chainCount, fixture))
    {
        return coverage;
    }
    Engine& engine = *fixture.engine;
    const unsigned char* pool = fixture.pool.data();

    const unsigned long long startShifts[] = { 0, 3, Engine::shiftCap };

    for (size_t chainIndex = 0; chainIndex < fixture.samples.chains.size(); ++chainIndex)
    {
        const AntSample& sample = fixture.samples.chains[chainIndex][0];
        typename Engine::ANN root;
        engine.deriveRootANN(sample.pubkey.m256i_u8, pool, root);
        engine.expand(root);
        engine.deriveMutationSeeds(sample.pubkey.m256i_u8, sample.nonce.m256i_u8, sample.anchor.m256i_u8, pool);

        const unsigned int changes = Engine::changesPerStep(sample.nonce.m256i_u8);
        const unsigned char mode = Engine::modeOf(sample.nonce.m256i_u8);
        engine.activeMode = mode;

        for (unsigned int step = 0; step < stepsPerChain; ++step)
        {
            for (unsigned int i = 0; i < changes; ++i)
            {
                engine.mutate(mode, engine.mutationSeed[step * score_engine::BPP9000_MAX_CHANGES_PER_STEP + i]);
            }
#ifdef __AVX512F__
            // The engine's kernel refreshes only the active mode's cache; the network was built by hand.
            engine.buildGatherControls();
            engine.buildTableArea();
#endif

            for (unsigned long long startShift : startShifts)
            {
                LiteBpp9000::fastReplayEnabled = false;
                engine.shift = startShift;
                const score_engine::Rating kernelRating = engine.advanceShift();
                const unsigned long long kernelShift = engine.shift;

                LiteBpp9000::fastReplayEnabled = true;
                engine.shift = startShift;
                const score_engine::Rating fastRating = engine.advanceShift();
                const unsigned long long fastShift = engine.shift;

                EXPECT_EQ(fastRating.error, kernelRating.error) << "chain " << chainIndex << " step " << step << " shift " << startShift;
                EXPECT_EQ(fastRating.shift, kernelRating.shift) << "chain " << chainIndex << " step " << step << " shift " << startShift;
                EXPECT_EQ(fastShift, kernelShift) << "chain " << chainIndex << " step " << step << " shift " << startShift;

                coverage.replays++;
                if (kernelRating.isValid())
                {
                    coverage.valid++;
                    if (kernelRating.shift > startShift)
                    {
                        coverage.advanced++;
                        if (kernelRating.shift == Engine::shiftCap)
                        {
                            coverage.reachedCap++;
                        }
                    }
                }
                else
                {
                    coverage.timedOut++;
                    if (kernelShift > startShift)
                    {
                        coverage.timedOutPastFirstFrame++;
                    }
                }
            }
        }
    }
    return coverage;
}

}

TEST(TestLiteBpp9000FastReplay, ReplayMatchesKernelOnTheProductionFrame)
{
    const ReplayCoverage coverage = compareReplays<ProductionConfig>(8, 32);
    EXPECT_GT(coverage.valid, 0u);
}

TEST(TestLiteBpp9000FastReplay, ReplayMatchesKernelWhenTheFrameSlides)
{
    const ReplayCoverage coverage = compareReplays<SlidingConfig>(8, 32);
    EXPECT_GT(coverage.advanced, 0u) << "no replay cleared the advance gate";
    EXPECT_LT(coverage.advanced, coverage.valid) << "every replay cleared the advance gate";
}

TEST(TestLiteBpp9000FastReplay, ReplayMatchesKernelUpToTheShiftCap)
{
    const ReplayCoverage coverage = compareReplays<CapReachingConfig>(8, 32);
    EXPECT_GT(coverage.reachedCap, 0u) << "no replay slid to the shift cap";
}

TEST(TestLiteBpp9000FastReplay, ReplayMatchesKernelWhenTheBudgetRunsOut)
{
    const ReplayCoverage coverage = compareReplays<TightBudgetConfig>(8, 32);
    EXPECT_GT(coverage.timedOut, 0u) << "no replay ran out of ticks";
    EXPECT_GT(coverage.valid, 0u) << "every replay ran out of ticks";
#ifndef __AVX512F__
    EXPECT_GT(coverage.timedOutPastFirstFrame, 0u) << "no replay ran out of ticks after a mastered frame";
#endif
}

// Whole walks down production lineages: the rating, the shift and the network the walk keeps.
TEST(TestLiteBpp9000FastReplay, WalkMatchesKernelOnProductionLineages)
{
    using Engine = score_engine::ScoreBpp9000<ProductionConfig>;
    FastReplaySwitch restoreSwitch;

    ReplayFixture<ProductionConfig> fixture;
    ASSERT_TRUE(makeReplayFixture<ProductionConfig>(4, fixture));
    Engine& engine = *fixture.engine;
    const unsigned char* pool = fixture.pool.data();

    for (size_t chainIndex = 0; chainIndex < fixture.samples.chains.size(); ++chainIndex)
    {
        const std::vector<AntSample>& chain = fixture.samples.chains[chainIndex];
        Engine::ANN parent;
        engine.deriveRootANN(chain[0].pubkey.m256i_u8, pool, parent);
        unsigned long long parentShift = 0;

        for (size_t depth = 0; depth < chain.size(); ++depth)
        {
            const AntSample& sample = chain[depth];

            LiteBpp9000::fastReplayEnabled = false;
            const score_engine::Rating kernelRating = engine.computeScoreFromParent(
                parent, parentShift, sample.pubkey.m256i_u8, sample.nonce.m256i_u8, sample.anchor.m256i_u8, pool);
            const unsigned long long kernelShift = engine.shift;
            Engine::ANN kernelBest;
            engine.getBestANN(kernelBest);

            LiteBpp9000::fastReplayEnabled = true;
            const score_engine::Rating fastRating = engine.computeScoreFromParent(
                parent, parentShift, sample.pubkey.m256i_u8, sample.nonce.m256i_u8, sample.anchor.m256i_u8, pool);
            const unsigned long long fastShift = engine.shift;
            Engine::ANN fastBest;
            engine.getBestANN(fastBest);

            EXPECT_EQ(fastRating.error, kernelRating.error) << "chain " << chainIndex << " depth " << depth;
            EXPECT_EQ(fastRating.shift, kernelRating.shift) << "chain " << chainIndex << " depth " << depth;
            EXPECT_EQ(fastShift, kernelShift) << "chain " << chainIndex << " depth " << depth;
            EXPECT_EQ(memcmp(&fastBest, &kernelBest, sizeof(fastBest)), 0) << "chain " << chainIndex << " depth " << depth;

            parent = fastBest;
            parentShift = fastRating.shift;
        }
    }
}
