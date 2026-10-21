// Focused lifecycle checks for the in-RAM contract-state pager.

#define NO_UEFI

#include <gtest/gtest.h>

#include "platform/msvc_polyfill.h"
#include "platform/file_io.h"
#include "platform/m256.h"
#define CONTRACT_STATE_PAGER_CONTRACT_COUNT 1
#include <extensions/contract_state_pager.h>
#undef CONTRACT_STATE_PAGER_CONTRACT_COUNT

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{

#ifndef _WIN32
void managedFaultHandler(int signalNumber, siginfo_t* info, void*)
{
    if (info && ContractStatePager::handleFault(info->si_addr))
    {
        return;
    }

    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

void installManagedFaultHandler()
{
    static std::once_flag once;
    std::call_once(once, []()
    {
        struct sigaction action{};
        action.sa_sigaction = managedFaultHandler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        sigaddset(&action.sa_mask, SIGSEGV);
        sigaddset(&action.sa_mask, SIGBUS);
        ASSERT_EQ(sigaction(SIGSEGV, &action, nullptr), 0);
        ASSERT_EQ(sigaction(SIGBUS, &action, nullptr), 0);
    });
}
#else
void installManagedFaultHandler()
{
}
#endif

struct ManagedState
{
    unsigned char* data = nullptr;
    ContractStatePager* pager = nullptr;
    size_t previousLimit = ContractStatePager::MAX_RAM_USAGE;

    explicit ManagedState(size_t size, unsigned int contractIndex = 0)
        : contractIndex(contractIndex)
    {
        installManagedFaultHandler();
        EXPECT_TRUE(ContractStatePager::create(&data, size, contractIndex));
        pager = ContractStatePager::getPager(contractIndex);
        EXPECT_NE(pager, nullptr);
    }

    ~ManagedState()
    {
        ContractStatePager::MAX_RAM_USAGE = previousLimit;
        ContractStatePager::release(contractIndex);
    }

private:
    unsigned int contractIndex;
};

std::array<unsigned char, 32> canonicalHash(const unsigned char* state, size_t size)
{
    std::array<unsigned char, 32> hash{};
    KangarooTwelve(state, (unsigned int)size, hash.data(), (unsigned int)hash.size());
    return hash;
}

std::array<unsigned char, 32> pagerHash(ManagedState& state)
{
    std::array<unsigned char, 32> hash{};
    EXPECT_EQ(state.pager->getHashAndProtect(hash.data(), hash.size()), 0);
    return hash;
}

void trimAllResidentState()
{
    ContractStatePager::MAX_RAM_USAGE = 0;
    ContractStatePager::tryEvictBlocks();
    ContractStatePager::tryEvictBlocks();
}

} // namespace

TEST(ContractStatePagerTest, ZeroSparseRandomPartialEvictRestoreAndModify)
{
    const size_t size = 11 * K12_chunkSize + 37;
    ManagedState state(size);

    EXPECT_EQ(pagerHash(state), canonicalHash(state.data, size));

    state.data[3] = 0x7A;
    state.data[5 * K12_chunkSize + 19] = 0xC3;
    state.data[size - 1] = 0x11;
    unsigned int random = 0x12345678;
    for (size_t i = K12_chunkSize; i < 3 * K12_chunkSize; i++)
    {
        random = random * 1664525u + 1013904223u;
        state.data[i] = (unsigned char)(random >> 24);
    }

    const auto beforeEviction = pagerHash(state);
    EXPECT_EQ(beforeEviction, canonicalHash(state.data, size));
    const size_t residentBefore = ContractStatePager::getResidentBytes();

    trimAllResidentState();
    EXPECT_LT(ContractStatePager::getResidentBytes(), residentBefore);
    EXPECT_GT(ContractStatePager::getCompressedBytes(), 0u);
    EXPECT_EQ(pagerHash(state), beforeEviction);

    state.data[6 * K12_chunkSize + 5] ^= 0xFF;
    const auto modified = pagerHash(state);
    EXPECT_NE(modified, beforeEviction);
    EXPECT_EQ(modified, canonicalHash(state.data, size));

    trimAllResidentState();
    EXPECT_EQ(pagerHash(state), modified);
}

TEST(ContractStatePagerTest, ZeroBlocksNeedNoCompressedStorage)
{
    ManagedState state(6 * K12_chunkSize);
    EXPECT_EQ(pagerHash(state), canonicalHash(state.data, 6 * K12_chunkSize));

    trimAllResidentState();
    EXPECT_EQ(ContractStatePager::getCompressedBytes(), 0u);
}

TEST(ContractStatePagerTest, ConcurrentColdFaultsAreSerializedWithoutCorruption)
{
    const size_t size = 16 * K12_chunkSize;
    ManagedState state(size);

    // Every slot sits in the same pager block, so one thread's cold fault races all the others
    // whatever the page size is; spacing by K12_chunkSize would only collide on 16K-page hosts.
    const size_t threadCount = 8;
    const size_t slotStride = state.pager->getBlockSize() / threadCount;

    for (size_t iteration = 0; iteration < 200; iteration++)
    {
        for (size_t slot = 0; slot < threadCount; slot++)
        {
            state.data[slot * slotStride] = (unsigned char)(slot + 1);
        }
        pagerHash(state);
        trimAllResidentState();

        std::atomic<bool> valuesOk{true};
        std::vector<std::thread> threads;
        for (size_t slot = 0; slot < threadCount; slot++)
        {
            threads.emplace_back([&, slot]()
            {
                unsigned char* value = state.data + slot * slotStride;
                if (*value != (unsigned char)(slot + 1))
                {
                    valuesOk.store(false, std::memory_order_relaxed);
                }
                *value = (unsigned char)(*value + 40);
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }

        ASSERT_TRUE(valuesOk.load(std::memory_order_relaxed)) << "iteration " << iteration;
        for (size_t slot = 0; slot < threadCount; slot++)
        {
            ASSERT_EQ(state.data[slot * slotStride], (unsigned char)(slot + 41)) << "iteration " << iteration << ", slot " << slot;
        }
    }

    EXPECT_EQ(pagerHash(state), canonicalHash(state.data, size));
}

TEST(ContractStatePagerTest, EvictionReleasesNativePages)
{
    ManagedState state(4 * K12_chunkSize);
    std::memset(state.data, 0xA5, 4 * K12_chunkSize);
    pagerHash(state);
    trimAllResidentState();

#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info{};
    ASSERT_EQ(VirtualQuery(state.data, &info, sizeof(info)), sizeof(info));
    EXPECT_EQ(info.State, (DWORD)MEM_RESERVE);
#elif defined(__APPLE__)
    const size_t pageSize = (size_t)sysconf(_SC_PAGESIZE);
    std::vector<char> residency(state.pager->getBlockSize() / pageSize);
    ASSERT_EQ(mincore(state.data, state.pager->getBlockSize(), residency.data()), 0);
    for (char page : residency)
    {
        EXPECT_EQ(page & 1, 0);
    }
#else
    EXPECT_EQ(ContractStatePager::getResidentBytes(), 0u);
#endif
}
