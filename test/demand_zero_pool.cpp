#define NO_UEFI

#include "gtest/gtest.h"

#ifdef __linux__
#include "platform/msvc_polyfill.h"
#endif

#include "platform/memory_util.h"

#include <cstring>
#if defined(__linux__)
#include <cstdlib>
#include <sys/mman.h>
#endif

// A pool above the threshold arrives zeroed, and zeroing an unaligned window inside it must leave
// the bytes on either side untouched.
TEST(DemandZeroPool, UnalignedZeroPoolPreservesNeighbours)
{
    const unsigned long long size = DEMAND_ZERO_POOL_THRESHOLD;
    unsigned char* pool = nullptr;
    ASSERT_TRUE(allocSparsePoolWithErrorLog(L"DemandZeroPool", size, (void**)&pool, __LINE__));
#if defined(__linux__)
    EXPECT_TRUE(qVirtualContains(pool, size));
#endif
    for (unsigned long long i = 0; i < size; i += 4096)
    {
        ASSERT_EQ(pool[i], 0);
    }

    memset(pool, 0xAB, size);
    const unsigned long long from = 4096 * 3 + 100;
    const unsigned long long len = 4096 * 5 + 200;
    zeroPool(pool + from, len);
    EXPECT_EQ(pool[from - 1], 0xAB);
    for (unsigned long long i = from; i < from + len; i++)
    {
        ASSERT_EQ(pool[i], 0);
    }
    EXPECT_EQ(pool[from + len], 0xAB);

    // Whole-pool zero, then a second alloc/free cycle: freePool must release a mapping cleanly.
    zeroPool(pool, size);
    EXPECT_EQ(pool[size - 1], 0);
    freePool(pool);
    EXPECT_FALSE(qVirtualContains(pool, size));
}

TEST(DemandZeroPool, SmallPoolStaysOnHeap)
{
    unsigned char* pool = nullptr;
    ASSERT_TRUE(allocPoolWithErrorLog(L"SmallPool", 1000, (void**)&pool, __LINE__));
    EXPECT_FALSE(qVirtualContains(pool, 1000));
    memset(pool, 0xAB, 1000);
    zeroPool(pool + 10, 500);
    EXPECT_EQ(pool[9], 0xAB);
    EXPECT_EQ(pool[10], 0);
    EXPECT_EQ(pool[509], 0);
    EXPECT_EQ(pool[510], 0xAB);
    freePool(pool);
}

#if defined(__linux__)
// Zero pages go back to the system and still read as zero; pages holding data, and the partial pages
// at both ends of the range, stay as they are.
TEST(DemandZeroPool, ReleaseZeroPagesKeepsContent)
{
    constexpr unsigned long long pageSize = 4096;
    constexpr unsigned long long pageCount = 64;
    unsigned char* block = (unsigned char*)aligned_alloc(pageSize, pageCount * pageSize);
    ASSERT_NE(block, nullptr);
    memset(block, 0, pageCount * pageSize);
    block[3 * pageSize + 17] = 0x5A;
    block[40 * pageSize] = 0x01;

    releaseZeroPages(block + 100, pageCount * pageSize - 200);

    // Before any read: a read maps the shared zero page, which counts as present.
    unsigned char resident[pageCount];
    ASSERT_EQ(mincore(block, pageCount * pageSize, resident), 0);
    EXPECT_TRUE(resident[0] & 1);
    EXPECT_TRUE(resident[3] & 1);
    EXPECT_TRUE(resident[40] & 1);
    EXPECT_TRUE(resident[pageCount - 1] & 1);
    EXPECT_FALSE(resident[1] & 1);
    EXPECT_FALSE(resident[20] & 1);
    EXPECT_FALSE(resident[pageCount - 2] & 1);

    for (unsigned long long i = 0; i < pageCount * pageSize; i++)
    {
        const unsigned char expected = i == 3 * pageSize + 17 ? 0x5A : (i == 40 * pageSize ? 0x01 : 0);
        ASSERT_EQ(block[i], expected) << "offset " << i;
    }
    free(block);
}
#endif
