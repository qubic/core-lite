#define NO_UEFI

#include "gtest/gtest.h"
#include "../src/network_messages/tick.h"
#include "../src/public_settings.h"
#include "../src/platform/virtual_memory.h"
#include "../src/extensions/tick_fork_control.h"

#include <cstring>
#include <chrono>
#include <filesystem>
#include <random>
#include <thread>
#include <atomic>
#include <vector>
#if defined(__linux__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "network_messages/transactions.h"

TEST(TestVirtualMemory, TestVirtualMemory_NativeChar) {
    initFilesystem();
    registerAsynFileIO(NULL);
    const unsigned long long name_u64 = 123456789;
    const unsigned long long pageDir = 0;
    VirtualMemory<char, name_u64, pageDir, 1997, 128> test_vm;
    test_vm.init();
    std::vector<char> arr;
    // fill with random value
    const int N = 1000000;
    arr.resize(N);
    srand(0);
    for (int i = 0; i < N; i++)
    {
        arr[i] = int(rand() % 256) - 127;
    }
    // append to virtual memory via single append
    for (int i = 0; i < 113; i++)
    {
        test_vm.append(arr[i]);
    }
    int pos = 113;
    int stride = 1337;
    while (pos < N)
    {
        int s = pos;
        int e = std::min(pos + stride, N);
        int n_item = e - s;
        test_vm.appendMany(arr.data() + s, n_item);
        pos += stride;
    }
    std::vector<char> fetcher;
    
    for (int i = 0; i < 1024; i++)
    {
        int offset = rand() % (N/2);
        int test_len = rand() % (N - offset);
        fetcher.resize(test_len);
        test_vm.getMany(fetcher.data(), offset, test_len);
        EXPECT_TRUE(memcmp(fetcher.data(), arr.data() + offset, test_len) == 0);
    }
    
    for (int i = 0; i < 1024; i++)
    {
        int index = rand() % N;
        EXPECT_TRUE(test_vm[index] == arr[index]);
    }
    test_vm.deinit();
}

#define IMAX_BITS(m) ((m)/((m)%255+1) / 255%255*8 + 7-86/((m)%255+12))
#define RAND_MAX_WIDTH IMAX_BITS(RAND_MAX)
static_assert((RAND_MAX& (RAND_MAX + 1u)) == 0, "RAND_MAX not a Mersenne number");
uint64_t rand64(void) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i += RAND_MAX_WIDTH) {
        r <<= RAND_MAX_WIDTH;
        r ^= (unsigned)rand();
    }
    return r;
}

TEST(TestVirtualMemory, TestVirtualMemory_NativeU64) {
    initFilesystem();
    registerAsynFileIO(NULL);
    const unsigned long long name_u64 = 123456789;
    const unsigned long long pageDir = 0;
    VirtualMemory<unsigned long long, name_u64, pageDir, 2005, 128> test_vm;
    test_vm.init();
    std::vector<unsigned long long> arr;
    // fill with random value
    const int N = 1000000;
    arr.resize(N);
    srand(0);
    for (int i = 0; i < N; i++)
    {
        arr[i] = rand64();
    }
    // append to virtual memory via single append
    for (int i = 0; i < 113; i++)
    {
        test_vm.append(arr[i]);
    }
    int pos = 113;
    int stride = 1337;
    while (pos < N)
    {
        int s = pos;
        int e = std::min(pos + stride, N);
        int n_item = e - s;
        test_vm.appendMany(arr.data() + s, n_item);
        pos += stride;
    }
    std::vector<unsigned long long> fetcher;

    for (int i = 0; i < 1024; i++)
    {
        int offset = rand() % (N/2);
        int test_len = rand() % (N - offset);
        fetcher.resize(test_len);
        test_vm.getMany(fetcher.data(), offset, test_len);
        EXPECT_TRUE(memcmp(fetcher.data(), arr.data() + offset, test_len * sizeof(unsigned long long)) == 0);
    }
    for (int i = 0; i < 1024; i++)
    {
        int index = rand() % N;
        EXPECT_TRUE(test_vm[index] == arr[index]);
    }
    test_vm.deinit();
}

TickData randTick()
{
    TickData res;
    int* ptr = (int*)&res;
    int sz = sizeof(res);
    for (int i = 0; i < sz/4; i++) {
        ptr[i] = rand();
    }
    return res;
}

bool tickEqual(const TickData a, const TickData b)
{
    return memcmp(&a, &b, sizeof(TickData)) == 0;
}

TEST(TestVirtualMemory, TestVirtualMemory_TickStruct) {
    initFilesystem();
    registerAsynFileIO(NULL);
    const unsigned long long name_u64 = 123456789;
    const unsigned long long pageDir = 0;
    VirtualMemory<TickData, name_u64, pageDir, 2005, 128> test_vm;
    test_vm.init();
    std::vector<TickData> arr;
    // fill with random value
    const int N = 1000;
    arr.resize(N);
    srand(0);
    for (int i = 0; i < N; i++)
    {
        arr[i] = randTick();
    }
    // append to virtual memory via single append
    for (int i = 0; i < 113; i++)
    {
        test_vm.append(arr[i]);
    }
    int pos = 113;
    int stride = 1337;
    while (pos < N)
    {
        int s = pos;
        int e = std::min(pos + stride, N);
        int n_item = e - s;
        test_vm.appendMany(arr.data() + s, n_item);
        pos += stride;
    }
    std::vector<TickData> fetcher;

    for (int i = 0; i < 1024; i++)
    {
        int offset = rand() % (N/2);
        int test_len = rand() % (N - offset);
        fetcher.resize(test_len);
        test_vm.getMany(fetcher.data(), offset, test_len);
        EXPECT_TRUE(memcmp(fetcher.data(), arr.data() + offset, test_len * sizeof(TickData)) == 0);
    }
    for (int i = 0; i < 1024; i++)
    {
        int index = rand() % N;
        EXPECT_TRUE(tickEqual(test_vm[index], arr[index]));
    }
    test_vm.deinit();
}

TEST(TestVirtualMemory, TestVirtualMemory_SpecialCases) {
    initFilesystem();
    registerAsynFileIO(NULL);
    const unsigned long long name_u64 = 123456789;
    const unsigned long long pageDir = 0;
    const unsigned long long pageCap = 2001;
    VirtualMemory<char, name_u64, pageDir, pageCap, 128> test_vm;
    test_vm.init();
    std::vector<char> arr;
    // fill with random value
    const int N = 1000000;
    arr.resize(N);
    srand(0);
    for (int i = 0; i < N; i++)
    {
        arr[i] = int(rand() % 256) - 127;
    }
    // append to virtual memory via single append
    for (int i = 0; i < 113; i++)
    {
        test_vm.append(arr[i]);
    }
    int pos = 113;
    int stride = 1337;
    while (pos < N)
    {
        int s = pos;
        int e = std::min(pos + stride, N);
        int n_item = e - s;
        test_vm.appendMany(arr.data() + s, n_item);
        pos += stride;
    }

    // case: head start aligns page
    std::vector<char> fetcher;
    {
        int offset = pageCap;
        int test_len = pageCap * 5 + 1;
        fetcher.resize(test_len);
        test_vm.getMany(fetcher.data(), offset, test_len);
        EXPECT_TRUE(memcmp(fetcher.data(), arr.data() + offset, test_len) == 0);
    }

    // case: tail end aligns page
    {
        int offset = 1337;
        int test_len = pageCap * 5 + (pageCap - 1337);
        fetcher.resize(test_len);
        test_vm.getMany(fetcher.data(), offset, test_len);
        EXPECT_TRUE(memcmp(fetcher.data(), arr.data() + offset, test_len) == 0);
    }

    test_vm.deinit();
}


TEST(TestSwapVirtualMemory, TestSwapVirtualMemory_IndexModeRandomAccess) {
    initFilesystem();
    registerAsynFileIO(NULL);

    struct TxHashMapEntry {
        m256i digest;
        unsigned long long offset;
    };
    SwapVirtualMemory<TxHashMapEntry, wcharToNumber(L"txdi"), wcharToNumber(L"data"), 64, 64, INDEX_MODE, 0> test_vm;
    test_vm.init();

    {
        TxHashMapEntry &entry = test_vm.getRef(1);
        entry.digest = m256i::zero();
        entry.digest.m256i_u64[0] = 1;
        entry.offset = 1;
    }

    {
        TxHashMapEntry& entry = test_vm.getRef(1000);
        entry.digest = m256i::zero();
        entry.digest.m256i_u64[0] = 1000;
        entry.offset = 1000;
    }

    {
        TxHashMapEntry& entry = test_vm.getRef(1'000'000);
        entry.digest = m256i::zero();
        entry.digest.m256i_u64[0] = 1'000'000;
        entry.offset = 1'000'000;
    }

    for (int i = 0; i < 1024 * 128 * 64; i++) {
        PinScope _pinScope; // release this iteration's page pin (no work-unit boundary in tests)
        auto randomRange = m256i::randomValue().m256i_u64[0] % (1024*128*64);
        if (randomRange != 1 && randomRange != 1000 && randomRange != 1'000'000) {
            TxHashMapEntry& entry = test_vm.getRef(randomRange);
            entry.digest = m256i::zero();
            entry.digest.m256i_u64[0] = randomRange;
            entry.offset = randomRange;
        }
    }

    // retest if previous test is ok
    {
        TxHashMapEntry& entry = test_vm.getRef(1);
        EXPECT_TRUE(entry.digest.m256i_u64[0] == 1);
        EXPECT_TRUE(entry.offset == 1);
    }
    {
        TxHashMapEntry& entry = test_vm.getRef(1000);
        EXPECT_TRUE(entry.digest.m256i_u64[0] == 1000);
        EXPECT_TRUE(entry.offset == 1000);
    }
    {
        TxHashMapEntry& entry = test_vm.getRef(1'000'000);
        EXPECT_TRUE(entry.digest.m256i_u64[0] == 1'000'000);
        EXPECT_TRUE(entry.offset == 1'000'000);
    }
}

TEST(TestSwapVirtualMemory, TestSwapVirtualMemory_IndexModeLinearAccess) {
    initFilesystem();
    registerAsynFileIO(NULL);

    struct TxHashMapEntry {
        m256i digest;
        unsigned long long offset;
    };
    SwapVirtualMemory<TxHashMapEntry, wcharToNumber(L"txdi"), wcharToNumber(L"data"), 64, 64, INDEX_MODE, 0> test_vm;
    test_vm.init();

    for (unsigned long long i = 0; i < 1024 * 128 * 64; i++) {
        PinScope _pinScope; // release this iteration's page pin (no work-unit boundary in tests)
        TxHashMapEntry& entry = test_vm.getRef(i);
        entry.digest = m256i::zero();
        entry.digest.m256i_u64[0] = i;
        entry.offset = i;
    }

    for (unsigned long long i = 0; i < 1024 * 128 * 64; i++) {
        PinScope _pinScope; // release this iteration's page pin (no work-unit boundary in tests)
        TxHashMapEntry& entry = test_vm.getRef(i);
        EXPECT_TRUE(entry.digest.m256i_u64[0] == i);
        EXPECT_TRUE(entry.offset == i);
    }
}

// Reproduces the epoch-transition pin-leak: a thread holds pin "notes" in its arena, the VM is
// reset() (as beginEpoch does), then the thread keeps working. Without the generation guard, the
// stale notes (a) suppress fresh pins (under-pin) and (b) make pinnedNow drift; with it, a reset
// cleanly invalidates the stale notes.
TEST(TestSwapVirtualMemory, TestSwapVirtualMemory_ResetInvalidatesStalePins) {
    initFilesystem();
    registerAsynFileIO(NULL);

    releaseThreadPins(); // start from a clean arena

    struct E { unsigned long long a, b; };
    SwapVirtualMemory<E, wcharToNumber(L"rsta"), wcharToNumber(L"data"), 64, 8, INDEX_MODE, 0> vm;
    vm.init();

    // Pin two distinct pages and DON'T release them (simulating a thread that holds notes across
    // the reset, like an in-flight HTTP request during an epoch transition).
    vm.getRef(0);    // page 0
    vm.getRef(64);   // page 1
    EXPECT_EQ(vm.getPinnedNow(), 2);

    // Epoch transition: reset wipes the cache + pin counters (but cannot touch this thread's arena).
    vm.reset();
    EXPECT_EQ(vm.getPinnedNow(), 0);

    // Fresh access to the SAME page must take a real pin again (stale note must not suppress it).
    vm.getRef(0);
    EXPECT_EQ(vm.getPinnedNow(), 1); // without the generation guard this would wrongly be 0

    // Releasing everything (the one fresh pin + the two stale notes) must land back at exactly 0,
    // not underflow or leave a phantom pinned slot.
    releaseThreadPins();
    EXPECT_EQ(vm.getPinnedNow(), 0);
}


TEST(TestSwapVirtualMemory, TestSwapVirtualMemory_OffsetModeLinearAccess) {
    initFilesystem();
    registerAsynFileIO(NULL);

    constexpr unsigned long long maxElementSize = (sizeof(Transaction) + SIGNATURE_SIZE + MAX_INPUT_SIZE);
    SwapVirtualMemory<Transaction, wcharToNumber(L"offs"), wcharToNumber(L"data"), 2, 64, OFFSET_MODE, SIGNATURE_SIZE + MAX_INPUT_SIZE> test_vm;
    test_vm.init();

    // Init
    {
        Transaction *tx = test_vm[0];
        tx->amount = 0;
        tx->inputSize = 0;
    }

    {
        Transaction *tx = test_vm[(maxElementSize)];
        tx->amount = maxElementSize;
        tx->inputSize = 0;
        // Should not use extra buffer
        EXPECT_TRUE(isAllBytesZero((void*)test_vm.getExtraBuffer(0), maxElementSize));
    }

    {
        EXPECT_TRUE(isAllBytesZero((void*)test_vm.getExtraBuffer(0), maxElementSize));
        // Will be added to extra buffer
        Transaction *tx = test_vm[(maxElementSize) + sizeof(Transaction)];
        tx->amount = (maxElementSize) + sizeof(Transaction);
        tx->inputSize = 4;
        EXPECT_FALSE(isAllBytesZero((void*)test_vm.getExtraBuffer(0), maxElementSize));

        // Check if the tx is from extra buffer
        EXPECT_TRUE(memcmp(test_vm.getExtraBuffer(0), (void*)tx, sizeof(Transaction)) == 0);
    }

    {
        Transaction *backupTx = new Transaction();
        memcpy(backupTx, (void*)test_vm.getExtraBuffer(0), sizeof(Transaction));
        // Will remove about tx from extra buffer and add below to extra buffer
        Transaction *tx = test_vm[(maxElementSize) + sizeof(Transaction) * 2 + 8];
        // Extra buffer should be reset
        EXPECT_TRUE(isAllBytesZero((void*)test_vm.getExtraBuffer(0), maxElementSize));
        // Check if the previous tx is written back correctly
        EXPECT_TRUE(memcmp(backupTx, (void*)test_vm[(maxElementSize) + sizeof(Transaction)], sizeof(Transaction)) == 0);
        tx->amount = (maxElementSize) + sizeof(Transaction) * 2 + 8;
        tx->inputSize = 0;
        EXPECT_FALSE(isAllBytesZero((void*)test_vm.getExtraBuffer(0), maxElementSize));
        // Check if the tx is from extra buffer
        EXPECT_TRUE(memcmp(test_vm.getExtraBuffer(0), (void*)tx, sizeof(Transaction)) == 0);
    }

    // Recheck
    {
        Transaction* tx = test_vm[0];
        EXPECT_TRUE(tx->amount == 0);
        EXPECT_TRUE(tx->inputSize == 0);
    }
    {
        Transaction* tx = test_vm[(maxElementSize)];
        EXPECT_TRUE(tx->amount == maxElementSize);
        EXPECT_TRUE(tx->inputSize == 0);
    }
    {
        Transaction* tx = test_vm[(maxElementSize) + sizeof(Transaction)];
        EXPECT_TRUE(tx->amount == (maxElementSize) + sizeof(Transaction));
        EXPECT_TRUE(tx->inputSize == 4);
    }
    {
        Transaction* tx = test_vm[(maxElementSize) + sizeof(Transaction) * 2 + 8];
        EXPECT_TRUE(tx->amount == (maxElementSize) + sizeof(Transaction) * 2 + 8);
        EXPECT_TRUE(tx->inputSize == 0);
    }
}

TEST(TestSwapVirtualMemory, TestSwapVirtualMemory_OffsetModeRandomAccess) {
    initFilesystem();
    registerAsynFileIO(NULL);

    SwapVirtualMemory<char, wcharToNumber(L"offs"), wcharToNumber(L"data"), 1024 * 1024, 64, OFFSET_MODE, 2> test_vm;
    test_vm.init();
    std::map<unsigned long long, char> valueMap;
    for (int i = 0; i < 1024 * 128 * 64; i++) {
        auto randomIndex = rand64() % (1024 * 128 * 64);
        char randomvalue = int(rand() % 256) - 127;
        char* entry = test_vm[randomIndex];
        *entry = randomvalue;
        valueMap[randomIndex] = randomvalue;
    }

    // Recheck
    for (auto& it : valueMap) {
        char* entry = test_vm[it.first];
        EXPECT_TRUE(*entry == it.second);
    }
}

TEST(TestSwapVirtualMemory, TestSwapVirtualMemory_TestCacheBuffer)
{
    initFilesystem();
    registerAsynFileIO(NULL);

    {
        SwapVirtualMemory<Transaction, wcharToNumber(L"cach"), wcharToNumber(L"data"), 1024 * 1024, 64, INDEX_MODE, 0> test_vm;
        test_vm.init();

        for (int i = 0; i < 64; i++)
        {
            Transaction *tx = test_vm.getCacheBuffer(i);
            EXPECT_TRUE(tx != nullptr);
            EXPECT_TRUE((uint64_t)tx == (uint64_t)test_vm.getCacheBuffer(0) + i * sizeof(Transaction) * 1024 * 1024);
        }
    }

    {
        SwapVirtualMemory<Transaction, wcharToNumber(L"cach"), wcharToNumber(L"data"), 1024 * 1024, 64, OFFSET_MODE, 64> test_vm;
        test_vm.init();
        EXPECT_TRUE(test_vm.getPageSize() == (1024 * 1024 * (sizeof(Transaction) + 64)));
        for (int i = 0; i < 64; i++)
        {
            Transaction *tx = test_vm.getCacheBuffer(i);
            EXPECT_TRUE(tx != nullptr);
            EXPECT_TRUE((uint64_t)tx == (uint64_t)test_vm.getCacheBuffer(0) + i * test_vm.getPageSize());
        }
    }
}

namespace {
struct E16 { unsigned long long a, b; }; // 16 bytes; pageCapacity 256 -> pageSize 4096 (one OS page)
#if defined(__linux__)
struct SwapCompressionTestScope
{
    bool previousCompression = gSwapCompressionEnabled;

    SwapCompressionTestScope()
    {
        gShadow.discard();
        gSwapCompressionEnabled = true;
    }
    ~SwapCompressionTestScope()
    {
        gShadow.discard();
        gSwapCompressionEnabled = previousCompression;
    }
};

void handleSwapDirtyTrackSignal(int signalNumber, siginfo_t* signalInfo, void*)
{
    if (signalNumber == SIGSEGV && signalInfo && SwapDirtyTrack::tryMarkDirty(signalInfo->si_addr))
        return;

    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}

struct SwapDirtyTrackTestScope
{
    bool previousDirtyTracking = gSwapDirtyTrackEnabled;
    struct sigaction previousSignalAction {};

    SwapDirtyTrackTestScope()
    {
        struct sigaction action {};
        action.sa_sigaction = handleSwapDirtyTrackSignal;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &previousSignalAction);
        gSwapDirtyTrackEnabled = true;
    }

    ~SwapDirtyTrackTestScope()
    {
        gSwapDirtyTrackEnabled = previousDirtyTracking;
        sigaction(SIGSEGV, &previousSignalAction, nullptr);
    }
};
#endif
}

// Every page must survive eviction+reload even though the working set (64 pages) far
// exceeds the cache (4 slots). Small + fast, exercising the IO-outside-the-lock evict/load path.
TEST(TestSwapVirtualMemory, IndexEvictReloadIntegritySmall) {
    initFilesystem();
    registerAsynFileIO(NULL);
    SwapVirtualMemory<E16, wcharToNumber(L"evrl"), wcharToNumber(L"data"), 256, 4, INDEX_MODE, 0> vm;
    vm.init();
    const unsigned long long NPAGES = 64, CAP = 256;
    for (unsigned long long p = 0; p < NPAGES; p++) {
        PinScope _;
        E16& e = vm.getRef(p * CAP);
        e.a = p + 1; e.b = (p + 1) * 7;
    }
    for (unsigned long long p = 0; p < NPAGES; p++) {
        PinScope _;
        E16& e = vm.getRef(p * CAP);
        EXPECT_EQ(e.a, p + 1);
        EXPECT_EQ(e.b, (p + 1) * 7);
    }
}

// Child-promote reinit: resetPinsForChildPromote must clear the inherited locks/pins/LOADING slots
// without corrupting cache content, and drainInflightIO must report idle afterwards. Every page (cached
// or evicted) must still read back correctly, and the absence of an orphan LOADING slot is proven by
// the re-read completing (a leftover LOADING page would dedup-wait forever).
TEST(TestSwapVirtualMemory, ForkReinitResetPinsAndDrain) {
    initFilesystem();
    registerAsynFileIO(NULL);
    SwapVirtualMemory<E16, wcharToNumber(L"frst"), wcharToNumber(L"data"), 256, 4, INDEX_MODE, 0> vm;
    vm.init();
    const unsigned long long NPAGES = 64, CAP = 256;
    for (unsigned long long p = 0; p < NPAGES; p++) {
        PinScope _;
        E16& e = vm.getRef(p * CAP);
        e.a = p + 1; e.b = (p + 1) * 7;
    }

    vm.resetPinsForChildPromote();
    EXPECT_TRUE(vm.drainInflightIO(100));   // no in-flight IO after reset -> idle

    for (unsigned long long p = 0; p < NPAGES; p++) {
        PinScope _;
        E16& e = vm.getRef(p * CAP);
        EXPECT_EQ(e.a, p + 1);
        EXPECT_EQ(e.b, (p + 1) * 7);
    }
}

// Cache hits must keep working (no corruption, no deadlock) while another thread churns
// misses that evict+load under released memLock. A churner walks 200 pages (constant eviction) while
// four hitters hammer the resident sentinel page 0; page 0 is only ever written 0xABCD, so any other
// value read back signals a torn evict/reload.
TEST(TestSwapVirtualMemory, ConcurrentHitsDuringMiss) {
    initFilesystem();
    registerAsynFileIO(NULL);
    SwapVirtualMemory<E16, wcharToNumber(L"chdm"), wcharToNumber(L"data"), 256, 8, INDEX_MODE, 0> vm;
    vm.init();
    const unsigned long long CAP = 256;
    { PinScope _; vm.getRef(0).a = 0xABCD; }

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::thread churner([&]{
        unsigned long long p = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            PinScope _;
            vm.getRef(p * CAP).a = p;
            p = (p % 200) + 1;
        }
    });
    std::vector<std::thread> hitters;
    for (int t = 0; t < 4; t++) hitters.emplace_back([&]{
        for (int i = 0; i < 100000; i++) {
            PinScope _;
            if (vm.getRef(0).a != 0xABCD) errors++;
        }
    });
    for (auto& h : hitters) h.join();
    stop = true; churner.join();
    EXPECT_EQ(errors.load(), 0);
}

#if defined(__linux__)
TEST(TestSwapVirtualMemory, CompressedShadowCommitReloadsNewData)
{
    initFilesystem();
    registerAsynFileIO(NULL);
    SwapCompressionTestScope scope;
    SwapVirtualMemory<E16, wcharToNumber(L"cmit"), wcharToNumber(L"data"), 256, 4, INDEX_MODE, 0> vm;
    ASSERT_TRUE(vm.init());

    constexpr unsigned long long cap = 256;
    for (unsigned long long page = 0; page < 16; ++page)
    {
        PinScope pins;
        vm.getRef(page * cap).a = 1000 + page;
    }
    {
        PinScope pins;
        ASSERT_EQ(vm.getRef(0).a, 1000U);
        vm.getRef(0).a = 9000;
    }

    ASSERT_TRUE(gShadow.arm());
    for (unsigned long long page = 16; page < 40; ++page)
    {
        PinScope pins;
        vm.getRef(page * cap).a = 1000 + page;
    }
    gShadow.commit();

    {
        PinScope pins;
        EXPECT_EQ(vm.getRef(0).a, 9000U);
    }
}

TEST(TestSwapVirtualMemory, CompressedShadowPromotionReadsPristine)
{
    initFilesystem();
    registerAsynFileIO(NULL);
    SwapCompressionTestScope scope;
    SwapVirtualMemory<E16, wcharToNumber(L"prom"), wcharToNumber(L"data"), 256, 4, INDEX_MODE, 0> vm;
    ASSERT_TRUE(vm.init());

    constexpr unsigned long long cap = 256;
    for (unsigned long long page = 0; page < 16; ++page)
    {
        PinScope pins;
        vm.getRef(page * cap).a = 1000 + page;
    }
    {
        PinScope pins;
        ASSERT_EQ(vm.getRef(0).a, 1000U);
    }

    int pipeFds[2];
    ASSERT_EQ(pipe(pipeFds), 0);
    ASSERT_TRUE(gShadow.arm());

    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0)
    {
        close(pipeFds[1]);
        tickForkControl::readChildCommand(pipeFds[0], 64);
        close(pipeFds[0]);

        gShadow.reinitForChildPromote();
        const bool clean = gShadow.purgeOrphans();
        vm.resetPinsForChildPromote();

        for (unsigned long long page = 1; page < 16; ++page)
        {
            PinScope pins;
            (void)vm.getRef(page * cap).a;
        }
        unsigned long long restored = 0;
        {
            PinScope pins;
            restored = vm.getRef(0).a;
        }
        _exit(clean && restored == 1000 ? 0 : 1);
    }

    close(pipeFds[0]);
    {
        PinScope pins;
        vm.getRef(0).a = 9000;
    }
    for (unsigned long long page = 16; page < 40; ++page)
    {
        PinScope pins;
        vm.getRef(page * cap).a = 1000 + page;
    }

    bool compressedShadowPage = false;
    for (const auto& entry : std::filesystem::directory_iterator("promdata000/s"))
    {
        if (entry.is_regular_file() && entry.file_size() < sizeof(E16) * cap)
        {
            compressedShadowPage = true;
            break;
        }
    }
    EXPECT_TRUE(compressedShadowPage);

    const char tag = tickForkControl::promoteTag;
    const unsigned int targetTick = 40;
    EXPECT_EQ(write(pipeFds[1], &tag, 1), 1);
    EXPECT_EQ(write(pipeFds[1], &targetTick, sizeof(targetTick)), (ssize_t)sizeof(targetTick));
    close(pipeFds[1]);

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_FALSE(std::filesystem::exists("promdata000/s"));
}

// With dirty tracking on, a page only read since load is evicted without a writeback
// (clean), while a written page is written back and survives. Tests install the SIGSEGV fast path
// that the node installs in signalHandler (the test binary has no handler of its own).
TEST(TestSwapVirtualMemory, DirtyTrackSkipsCleanWriteback) {
    initFilesystem();
    registerAsynFileIO(NULL);
    SwapDirtyTrackTestScope dirtyTrackScope;

    {
        SwapVirtualMemory<E16, wcharToNumber(L"dtyt"), wcharToNumber(L"data"), 256, 4, INDEX_MODE, 0> vm;
        vm.init();
        const unsigned long long CAP = 256;
        for (unsigned long long p = 0; p < 16; p++) {
            PinScope _;
            E16& e = vm.getRef(p * CAP);
            if (p % 2 == 0) e.a = p + 100;                          // written -> dirty
            else { volatile unsigned long long r = e.a; (void)r; }  // read only -> clean
        }
        EXPECT_GT(vm.getCleanEvicts(), 0u);
        EXPECT_GT(vm.getDirtyEvicts(), 0u);
        for (unsigned long long p = 0; p < 16; p++) {
            PinScope _;
            E16& e = vm.getRef(p * CAP);
            if (p % 2 == 0) EXPECT_EQ(e.a, p + 100);  // dirty page written back + reloaded
            else EXPECT_EQ(e.a, 0u);                  // clean page never written -> reloads as zero
        }
    }
}

// A pool whose page is not a multiple of the OS page cannot be armed read-only, so with tracking on every
// occupied victim must still be written back: written data survives eviction and reload.
TEST(TestSwapVirtualMemory, DirtyTrackUnalignedPoolAlwaysWritesBack) {
    initFilesystem();
    registerAsynFileIO(NULL);
    SwapDirtyTrackTestScope dirtyTrackScope;
    {
        const unsigned long long CAP = 384;                       // 16 B * 384 = 6144 B page, not 4K-strided
        SwapVirtualMemory<E16, wcharToNumber(L"dtun"), wcharToNumber(L"data"), CAP, 4, INDEX_MODE, 0> vm;
        vm.init();
        for (unsigned long long p = 0; p < 16; p++) {
            PinScope _;
            vm.getRef(p * CAP).a = p + 100;
        }
        EXPECT_EQ(vm.getCleanEvicts(), 0u);
        EXPECT_GT(vm.getDirtyEvicts(), 0u);
        for (unsigned long long p = 0; p < 16; p++) {
            PinScope _;
            EXPECT_EQ(vm.getRef(p * CAP).a, p + 100);
        }
    }
}

TEST(TestSwapVirtualMemory, RestoredCachedPageSurvivesEviction)
{
    initFilesystem();
    registerAsynFileIO(NULL);
    SwapDirtyTrackTestScope dirtyTrackScope;

    constexpr unsigned long long pageCapacity = 256;
    SwapVirtualMemory<E16, wcharToNumber(L"rstr"), wcharToNumber(L"data"), pageCapacity, 4, INDEX_MODE, 0> vm;
    ASSERT_TRUE(vm.init());

    {
        PinScope pins;
        vm.getRef(pageCapacity).a = 0x12345678;
    }

    std::vector<unsigned char> snapshot(vm.getVmStateSize());
    ASSERT_EQ(vm.dumpVMState(snapshot.data()), snapshot.size());

    vm.reset();
    ASSERT_EQ(vm.loadVMState(snapshot.data()), snapshot.size());

    for (unsigned long long page = 2; page < 16; ++page)
    {
        PinScope pins;
        (void)vm.getRef(page * pageCapacity).a;
    }

    PinScope pins;
    EXPECT_EQ(vm.getRef(pageCapacity).a, 0x12345678U);
}
#endif

// A per-access PinScope keeps the pin count flat no matter how many pages the loop walks, so a
// full-epoch scan can never exhaust the cache on its own.
TEST(TestSwapVirtualMemory, PerAccessPinScopeNeverStarves) {
    initFilesystem();
    registerAsynFileIO(NULL);
    tlPinArena.count = 0; // drop notes left by scope-less tests on VMs that no longer exist
    SwapVirtualMemory<E16, wcharToNumber(L"pnsc"), wcharToNumber(L"data"), 256, 4, INDEX_MODE, 0> vm;
    vm.init();
    for (unsigned long long p = 0; p < 64; p++) {
        PinScope _;
        vm.getRef(p * 256).a = p;
        EXPECT_EQ(vm.getPinnedNow(), 1);
    }
    EXPECT_EQ(vm.getPinnedNow(), 0);
    EXPECT_EQ(vm.getAllPinnedWaits(), 0ULL);
}

// One thread pinning every slot is the self-deadlock condition: a peer's miss must wait (counted in
// allPinnedWaits) and proceed only once the holder's scope ends.
TEST(TestSwapVirtualMemory, AllPinnedWaitRecoversWhenPeerReleases) {
    initFilesystem();
    registerAsynFileIO(NULL);
    tlPinArena.count = 0;
    SwapVirtualMemory<E16, wcharToNumber(L"pnwt"), wcharToNumber(L"data"), 256, 4, INDEX_MODE, 0> vm;
    vm.init();
    std::atomic<bool> peerDone{ false };
    std::thread peer;
    {
        PinScope holder;
        for (unsigned long long p = 0; p < 5; p++)
            vm.getRef(p * 256).a = p;
        EXPECT_EQ(vm.getPinnedNow(), 5);
        EXPECT_EQ(vm.getPinsHeldByThisThread(), 5);
        peer = std::thread([&] {
            PinScope _;
            vm.getRef(5 * 256).a = 5;
            peerDone.store(true);
        });
        for (int i = 0; i < 2000 && vm.getAllPinnedWaits() == 0; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        EXPECT_EQ(vm.getAllPinnedWaits(), 1ULL);
        EXPECT_FALSE(peerDone.load());
    }
    peer.join();
    EXPECT_TRUE(peerDone.load());
    EXPECT_EQ(vm.getAllPinnedWaits(), 1ULL);
    EXPECT_EQ(vm.getPinnedNow(), 0);
}

// getPinsHeldByThisThread counts only this thread's live notes: reset() invalidates them and the scope
// end releases them, so the fatal-path diagnostic can tell self-deadlock from cross-thread pressure.
TEST(TestSwapVirtualMemory, PinsHeldByThisThreadTracksScopeAndReset) {
    initFilesystem();
    registerAsynFileIO(NULL);
    tlPinArena.count = 0;
    SwapVirtualMemory<E16, wcharToNumber(L"pnhd"), wcharToNumber(L"data"), 256, 4, INDEX_MODE, 0> vm;
    vm.init();
    {
        PinScope _;
        for (unsigned long long p = 0; p < 3; p++)
            vm.getRef(p * 256).a = p;
        EXPECT_EQ(vm.getPinsHeldByThisThread(), 3);
        EXPECT_EQ(vm.getPinnedNow(), 3);
        vm.reset();
        EXPECT_EQ(vm.getPinsHeldByThisThread(), 0);
        EXPECT_EQ(vm.getPinnedNow(), 0);
    }
    EXPECT_EQ(vm.getPinnedNow(), 0);
    EXPECT_EQ(tlPinArena.count, 0);
}

// A releaseThreadPins() inside a scope (beginEpoch does this on the tick processor) must not leave the
// pins the scope takes afterwards stranded below its savepoint.
TEST(TestSwapVirtualMemory, ScopeReleasesPinsTakenAfterDrain) {
    initFilesystem();
    registerAsynFileIO(NULL);
    tlPinArena.count = 0;
    SwapVirtualMemory<E16, wcharToNumber(L"pndr"), wcharToNumber(L"data"), 256, 4, INDEX_MODE, 0> vm;
    vm.init();
    vm.getRef(0).a = 1; // pinned outside any scope, like initializeFirstTick before the tick loop
    {
        PinScope _;
        releaseThreadPins();
        vm.getRef(256).a = 2;
        EXPECT_EQ(vm.getPinnedNow(), 1);
    }
    EXPECT_EQ(vm.getPinnedNow(), 0);
    EXPECT_EQ(tlPinArena.count, 0);
}

TEST(TestVirtualMemory, CacheGrowthFollowsMemoryFloorAndReleasesOldest) {
    if (availableMemoryBytes() < memoryHeadroom::baseMarginBytes + (1LL << 30))
    {
        return; // Needs available memory above the base margin; unknown on Windows.
    }
    initFilesystem();
    registerAsynFileIO(NULL);
    constexpr unsigned long long pageCapacity = 4096;
    constexpr unsigned long long pagesToWrite = 10;
    VirtualMemory<char, wcharToNumber(L"grow"), 0, pageCapacity, 6> vm;
    vm.setCacheGrowth(2);
    vm.init();
    unsigned long long written = 0;
    const auto appendPages = [&](unsigned long long pages) {
        for (unsigned long long i = 0; i < pages * pageCapacity; i++)
        {
            vm.append((char)(written % 251));
            written++;
        }
    };

    // Floor out of reach: only the guaranteed slots are used.
    memoryHeadroom::gForkNeedBytes.store(1LL << 50);
    appendPages(pagesToWrite / 2);
    EXPECT_EQ(vm.oldestReleasableCacheAccess(), 0u);
    EXPECT_EQ(vm.tryReleaseOldestCacheSlot(), 0u);

    // Floor within reach: the cache grows, and gives its pages back oldest first.
    memoryHeadroom::gForkNeedBytes.store(0);
    appendPages(pagesToWrite / 2);
    unsigned long long previousOldest = vm.oldestReleasableCacheAccess();
    EXPECT_NE(previousOldest, 0u);
    unsigned int released = 0;
    while (vm.oldestReleasableCacheAccess())
    {
        EXPECT_EQ(vm.tryReleaseOldestCacheSlot(), pageCapacity);
        const unsigned long long oldest = vm.oldestReleasableCacheAccess();
        if (oldest)
        {
            EXPECT_GE(oldest, previousOldest);
            previousOldest = oldest;
        }
        released++;
    }
    EXPECT_GT(released, 0u);
    EXPECT_LE(released, 4u);

    // Released pages are read back from disk.
    for (unsigned long long i = 0; i < pagesToWrite * pageCapacity; i += 97)
    {
        EXPECT_EQ(vm[i], (char)(i % 251));
    }
    vm.deinit();
    memoryHeadroom::gForkNeedBytes.store(memoryHeadroom::baseMarginBytes);
}
