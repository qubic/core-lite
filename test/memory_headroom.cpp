#define NO_UEFI

#include "gtest/gtest.h"
#include "../src/platform/memory_headroom.h"

using namespace memoryHeadroom;

namespace
{
    constexpr unsigned long long oneMB = 1ULL << 20;
    constexpr long long oneGB = 1LL << 30;
}

TEST(MemoryHeadroom, ParsesPrivateDirtyFromSmapsRollup)
{
    const char* rollup =
        "00400000-7ffce81af000 ---p 00000000 00:00 0 [rollup]\n"
        "Rss:            45533532 kB\n"
        "Shared_Dirty:         28 kB\n"
        "Private_Clean:    122860 kB\n"
        "Private_Dirty:     2048 kB\n"
        "Swap:            8846924 kB\n";
    EXPECT_EQ(parseSmapsRollupPrivateDirtyBytes(rollup), 2048LL * 1024);
    EXPECT_EQ(parseSmapsRollupPrivateDirtyBytes("Rss: 1 kB\n"), -1);
}

TEST(MemoryHeadroom, WithoutMeasurementFloorsAreTheFormerFixedOnes)
{
    EXPECT_EQ(forkNeedBytes(0, 256), baseMarginBytes);
    gForkNeedBytes.store(forkNeedBytes(0, 256));
    EXPECT_EQ(cacheFloorBytes(), 2 * baseMarginBytes);
    EXPECT_EQ(forkFloorBytes(), baseMarginBytes + forkReserveBytes);
}

TEST(MemoryHeadroom, NeedScalesWithRateTicksAndSafetyFactor)
{
    const long long need = forkNeedBytes(10 * oneMB, 100);
    EXPECT_EQ(need, (long long)(10 * oneMB / 100 * forkSafetyPercent * 100));
    EXPECT_LT(forkNeedBytes(10 * oneMB, 50), need);

    gForkNeedBytes.store(need);
    EXPECT_EQ(cacheFloorBytes(), need + baseMarginBytes);
    // The cache gives memory back before the window has to close.
    EXPECT_GT(cacheFloorBytes(), forkFloorBytes());
}

TEST(MemoryHeadroom, WindowIsSizedToAvailableMemory)
{
    const unsigned long long rate = 20 * oneMB;
    // Plenty of memory: the nominal window.
    EXPECT_EQ(windowTicksThatFit(64 * oneGB, rate, 256), 256u);
    // Not enough for a full window: a shorter one, whose need then fits above the reserve.
    const long long available = forkReserveBytes + 2 * oneGB;
    const unsigned int ticks = windowTicksThatFit(available, rate, 256);
    EXPECT_GT(ticks, 1u);
    EXPECT_LT(ticks, 256u);
    EXPECT_LE(forkNeedBytes(rate, ticks) + forkReserveBytes, available);
    // No room at all: still a valid window length.
    EXPECT_EQ(windowTicksThatFit(forkReserveBytes, rate, 256), 1u);
    EXPECT_EQ(windowTicksThatFit(0, rate, 256), 1u);
    // Not measured yet: the nominal window.
    EXPECT_EQ(windowTicksThatFit(0, 0, 256), 256u);
}

TEST(MemoryHeadroom, RateHistoryKeepsTheMaximumThenAgesOut)
{
    WindowRateHistory history;
    EXPECT_EQ(history.maxBytesPerTick(), 0u);
    history.record(10);
    history.record(50);
    history.record(20);
    EXPECT_EQ(history.maxBytesPerTick(), 50u);

    // Windows without a measurement age the old rates out.
    for (unsigned int i = 0; i < WindowRateHistory::capacity - 1; i++)
    {
        history.record(0);
    }
    EXPECT_EQ(history.maxBytesPerTick(), 20u);
    history.record(0);
    EXPECT_EQ(history.maxBytesPerTick(), 0u);

    history.record(7);
    history.reset();
    EXPECT_EQ(history.maxBytesPerTick(), 0u);
}
