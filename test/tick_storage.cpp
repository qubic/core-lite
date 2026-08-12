#define NO_UEFI

#include "gtest/gtest.h"

#include "../src/public_settings.h"
#undef MAX_NUMBER_OF_TICKS_PER_EPOCH
#define MAX_NUMBER_OF_TICKS_PER_EPOCH 50
#undef TICKS_TO_KEEP_FROM_PRIOR_EPOCH
#define TICKS_TO_KEEP_FROM_PRIOR_EPOCH 5
#include "../src/ticking/tick_storage.h"
#include "../src/extensions/tick_storage_scan.h"

#include <random>


class TestTickStorage : public TickStorage
{
    unsigned char transactionBuffer[MAX_TRANSACTION_SIZE];
public:

    void addTransaction(unsigned int tick, unsigned int transactionIdx, unsigned int inputSize)
    {
        ASSERT_TRUE(inputSize <= MAX_INPUT_SIZE);
        Transaction* transaction = (Transaction*)transactionBuffer;
        transaction->amount = 10;
        transaction->destinationPublicKey.setRandomValue();
        transaction->sourcePublicKey.setRandomValue();
        transaction->inputSize = inputSize;
        transaction->inputType = 0;
        transaction->tick = tick;

        unsigned int transactionSize = transaction->totalSize();

        auto* offsets = tickTransactionOffsets.getByTickInCurrentEpoch(tick);
        if (nextTickTransactionOffset + transactionSize <= tickTransactions.storageSpaceCurrentEpoch)
        {
            EXPECT_EQ(offsets[transactionIdx], 0);
            offsets[transactionIdx] = nextTickTransactionOffset;
            copyMem(tickTransactions(nextTickTransactionOffset), transaction, transactionSize);
            nextTickTransactionOffset += transactionSize;
        }
    }

    void addTick(unsigned int tick, unsigned int seed, unsigned short maxTransactions)
    {
        // use pseudo-random sequence
        std::mt19937 gen32(seed);

        // add tick data
        TickData& td = tickData.getByTickInCurrentEpoch(tick);
        td.epoch = 1234;
        td.tick = tick;

        // add computor ticks
        Tick* computorTicks = ticks.getByTickInCurrentEpoch(tick);
        for (int i = 0; i < NUMBER_OF_COMPUTORS; ++i)
        {
            computorTicks[i].epoch = 1234;
            computorTicks[i].computorIndex = i;
            computorTicks[i].tick = tick;
            computorTicks[i].prevResourceTestingDigest = gen32();
        }

        // add transactions of tick
        unsigned int transactionNum = gen32() % (maxTransactions + 1);
        unsigned int orderMode = gen32() % 2;
        unsigned int transactionSlot;
        for (unsigned int transaction = 0; transaction < transactionNum; ++transaction)
        {
            if (orderMode == 0)
                transactionSlot = transaction;  // standard order
            else if (orderMode == 1)
                transactionSlot = transactionNum - 1 - transaction;  // backward order
            addTransaction(tick, transactionSlot, gen32() % MAX_INPUT_SIZE);
        }
        checkStateConsistencyWithAssert();
    }

    void checkTick(unsigned int tick, unsigned int seed, unsigned short maxTransactions, bool previousEpoch = false)
    {
        // only last ticks of previous epoch are kept in storage -> check okay
        if (previousEpoch && !tickInPreviousEpochStorage(tick))
            return;

        // use pseudo-random sequence
        std::mt19937 gen32(seed);

        // check tick data
        TickData& td = previousEpoch ? tickData.getByTickInPreviousEpoch(tick) : tickData.getByTickInCurrentEpoch(tick);
        EXPECT_EQ((int)td.epoch, (int)1234);
        EXPECT_EQ(td.tick, tick);

        // check computor ticks
        Tick* computorTicks = previousEpoch ? ticks.getByTickInPreviousEpoch(tick) : ticks.getByTickInCurrentEpoch(tick);
        for (int i = 0; i < NUMBER_OF_COMPUTORS; ++i)
        {
            EXPECT_EQ((int)computorTicks[i].epoch, (int)1234);
            EXPECT_EQ((int)computorTicks[i].computorIndex, (int)i);
            EXPECT_EQ(computorTicks[i].tick, tick);
            EXPECT_EQ(computorTicks[i].prevResourceTestingDigest, gen32());
        }

        // check transactions of tick
        {
            const auto* offsets = previousEpoch ? tickTransactionOffsets.getByTickInPreviousEpoch(tick) : tickTransactionOffsets.getByTickInCurrentEpoch(tick);
            unsigned int transactionNum = gen32() % (maxTransactions + 1);
            unsigned int orderMode = gen32() % 2;
            unsigned int transactionSlot;

            for (unsigned int transaction = 0; transaction < transactionNum; ++transaction)
            {
                int expectedInputSize = (int)(gen32() % MAX_INPUT_SIZE);

                if (orderMode == 0)
                    transactionSlot = transaction;  // standard order
                else if (orderMode == 1)
                    transactionSlot = transactionNum - 1 - transaction;  // backward order

                // If previousEpoch, some transactions at the beginning may not have fit into the storage and are missing -> check okay
                // If current epoch, some may be missing at he end due to limited storage -> check okay
                if (!offsets[transactionSlot])
                    continue;

                Transaction* tp = tickTransactions(offsets[transactionSlot]);
                EXPECT_TRUE(tp->checkValidity());
                EXPECT_EQ(tp->tick, tick);
                EXPECT_EQ((int)tp->inputSize, expectedInputSize);
            }
        }
    }
};


#ifdef __linux__
TEST(TickStorageScan, SkipsInvalidatedAndDetectsTransactionCorruption)
{
    constexpr unsigned short epoch = 1234;
    constexpr unsigned int tick = 1000;

    ASSERT_TRUE(ts.init());
    ts.beginEpoch(tick);

    TickData& tickData = ts.tickData.getByTickInCurrentEpoch(tick);
    setMem(&tickData, sizeof(tickData), 0);
    tickData.epoch = INVALIDATED_TICK_DATA;

    const unsigned long long transactionOffset = ts.nextTickTransactionOffset;
    Transaction* transaction = ts.tickTransactions(transactionOffset);
    setMem(transaction, sizeof(Transaction) + SIGNATURE_SIZE, 0);
    transaction->amount = 1;
    transaction->tick = tick + 1;

    const unsigned int transactionSize = transaction->totalSize();
    ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick)[0] = transactionOffset;
    ts.nextTickTransactionOffset += transactionSize;

    Tick* tickVotes = ts.ticks.getByTickInCurrentEpoch(tick);
    setMem(tickVotes, sizeof(Tick) * NUMBER_OF_COMPUTORS, 0);
    for (unsigned int computor = 0; computor < QUORUM; computor++)
    {
        tickVotes[computor].epoch = epoch;
        tickVotes[computor].tick = tick;
        tickVotes[computor].computorIndex = computor;
    }

    tickStorageScan::Progress progress(false);

    const tickStorageScan::ScanResult invalidatedResult =
        tickStorageScan::scanLoadedRange(ts, epoch, tick, tick + 1, progress);
    EXPECT_EQ(invalidatedResult.issues.total, 0ULL);
    EXPECT_EQ(invalidatedResult.transactionsChecked, 0ULL);

    transaction->tick = tick;
    tickData.epoch = epoch;
    tickData.tick = tick;
    tickData.computorIndex = tick % NUMBER_OF_COMPUTORS;
    KangarooTwelve(
        transaction,
        transactionSize,
        &tickData.transactionDigests[0],
        sizeof(tickData.transactionDigests[0]));

    m256i tickDataDigest;
    KangarooTwelve(&tickData, sizeof(tickData), &tickDataDigest, sizeof(tickDataDigest));
    for (unsigned int computor = 0; computor < QUORUM; computor++)
        tickVotes[computor].transactionDigest = tickDataDigest;

    const unsigned long long staleTransactionOffset = ts.nextTickTransactionOffset;
    Transaction* staleTransaction = ts.tickTransactions(staleTransactionOffset);
    setMem(staleTransaction, sizeof(Transaction) + SIGNATURE_SIZE, 0);
    staleTransaction->amount = 1;
    staleTransaction->tick = tick + 1;
    ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick)[1] = staleTransactionOffset;
    ts.nextTickTransactionOffset += staleTransaction->totalSize();

    const tickStorageScan::ScanResult cleanResult =
        tickStorageScan::scanLoadedRange(ts, epoch, tick, tick + 1, progress);
    EXPECT_EQ(cleanResult.issues.total, 0ULL);
    EXPECT_EQ(cleanResult.transactionsChecked, 1ULL);

    transaction->tick++;
    const tickStorageScan::ScanResult corruptedResult =
        tickStorageScan::scanLoadedRange(ts, epoch, tick, tick + 1, progress);
    EXPECT_EQ(corruptedResult.issues.total, 2ULL);
    EXPECT_EQ(corruptedResult.issues.categoryTotals.at("transaction-tick"), 1ULL);
    EXPECT_EQ(corruptedResult.issues.categoryTotals.at("transaction-digest"), 1ULL);

    transaction->tick = tick;
    ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick)[0] = 0;
    const tickStorageScan::ScanResult missingOffsetResult =
        tickStorageScan::scanLoadedRange(ts, epoch, tick, tick + 1, progress);
    EXPECT_EQ(missingOffsetResult.issues.total, 1ULL);
    EXPECT_EQ(missingOffsetResult.transactionsChecked, 0ULL);
    EXPECT_EQ(missingOffsetResult.issues.categoryTotals.at("transaction-pair"), 1ULL);

    ts.beginEpoch(0);
}
#endif


TEST(TestCoreTickStorage, EpochTransition)
{
    TestTickStorage ts;
    unsigned int seed = 42;

    // use pseudo-random sequence
    std::mt19937 gen32(seed);

    // 5x test with running 2 epoch transitions
    for (int testIdx = 0; testIdx < 6; ++testIdx)
    {
        // first, test case of having no transactions
        unsigned short maxTransactions = (testIdx == 0) ? 0 : NUMBER_OF_TRANSACTIONS_PER_TICK;

        ts.init();
        ts.checkStateConsistencyWithAssert();

        const int firstEpochTicks = gen32() % (MAX_NUMBER_OF_TICKS_PER_EPOCH + 1);
        const int secondEpochTicks = gen32() % (MAX_NUMBER_OF_TICKS_PER_EPOCH + 1);
        const int thirdEpochTicks = gen32() % (MAX_NUMBER_OF_TICKS_PER_EPOCH + 1);
        const unsigned int firstEpochTick0 = gen32() % 10000000;
        const unsigned int secondEpochTick0 = firstEpochTick0 + firstEpochTicks;
        const unsigned int thirdEpochTick0 = secondEpochTick0 + secondEpochTicks;
        unsigned int firstEpochSeeds[MAX_NUMBER_OF_TICKS_PER_EPOCH];
        unsigned int secondEpochSeeds[MAX_NUMBER_OF_TICKS_PER_EPOCH];
        unsigned int thirdEpochSeeds[MAX_NUMBER_OF_TICKS_PER_EPOCH];
        for (int i = 0; i < firstEpochTicks; ++i)
            firstEpochSeeds[i] = gen32();
        for (int i = 0; i < secondEpochTicks; ++i)
            secondEpochSeeds[i] = gen32();
        for (int i = 0; i < thirdEpochTicks; ++i)
            thirdEpochSeeds[i] = gen32();

        // first epoch
        ts.beginEpoch(firstEpochTick0);
        ts.checkStateConsistencyWithAssert();

        // add ticks
        for (int i = 0; i < firstEpochTicks; ++i)
            ts.addTick(firstEpochTick0 + i, firstEpochSeeds[i], maxTransactions);

        // check ticks
        for (int i = 0; i < firstEpochTicks; ++i)
            ts.checkTick(firstEpochTick0 + i, firstEpochSeeds[i], maxTransactions);

        // Epoch transistion
        ts.beginEpoch(secondEpochTick0);
        ts.checkStateConsistencyWithAssert();

        // add ticks
        for (int i = 0; i < secondEpochTicks; ++i)
            ts.addTick(secondEpochTick0 + i, secondEpochSeeds[i], maxTransactions);

        // check ticks
        for (int i = 0; i < secondEpochTicks; ++i)
            ts.checkTick(secondEpochTick0 + i, secondEpochSeeds[i], maxTransactions);
        bool previousEpoch = true;
        for (int i = 0; i < firstEpochTicks; ++i)
            ts.checkTick(firstEpochTick0 + i, firstEpochSeeds[i], maxTransactions, previousEpoch);

        // Epoch transistion
        ts.beginEpoch(thirdEpochTick0);
        ts.checkStateConsistencyWithAssert();

        // add ticks
        for (int i = 0; i < thirdEpochTicks; ++i)
            ts.addTick(thirdEpochTick0 + i, thirdEpochSeeds[i], maxTransactions);

        // check ticks
        for (int i = 0; i < thirdEpochTicks; ++i)
            ts.checkTick(thirdEpochTick0 + i, thirdEpochSeeds[i], maxTransactions);
        for (int i = 0; i < secondEpochTicks; ++i)
            ts.checkTick(secondEpochTick0 + i, secondEpochSeeds[i], maxTransactions, previousEpoch);

        ts.deinit();
    }
}
