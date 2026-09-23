#pragma once
// Routes state allocation, digesting, and eviction through the active backend.
// The native state pager is shared by Linux, macOS, and Windows.

// LITE_SC_NO_PAGER selects the resident fallback for testing.
#if (defined(__linux__) || defined(__APPLE__) || defined(_WIN32)) \
    && defined(LITE_WASM_SC) && !defined(LITE_SC_NO_PAGER)
#define LITE_SC_PAGER 1
#endif

// Non-pager test builds use the ordinary resident allocator.
#if !defined(LITE_SC_PAGER) && defined(TESTNET) && defined(LITE_WASM_SC)
#define LITE_SC_CONTRACT_LEVEL 1
#endif

#include <atomic>
#include <cstdlib>
#include "extensions/fork_census.h"

namespace Wasm::Runtime
{

// Without the pager each reserved slot commits its whole state up front, so keep that window small.
#if !defined(LITE_SC_PAGER) && defined(LITE_WASM_SC)
static_assert(WASM_RESERVED_SLOT_COUNT <= 8, "more than 8 reserved Wasm slots need the state pager");
#endif

inline bool g_wasmOwnedSlot[contractCount] = {};

// Per-slot write sequence: odd while writing, even when quiescent.
// Outside the state bytes, so digests and consensus are unaffected.
inline std::atomic<unsigned long long> g_stateSeq[contractCount] = {};

// Raised around a writing dispatch; read-only and nested frames do not raise it.
struct StateWriteSeqScope
{
    StateWriteSeqScope(bool enabled, unsigned int contractIndex) : index(contractIndex), engaged(enabled)
    {
        if (engaged)
        {
            g_stateSeq[index].fetch_add(1, std::memory_order_release); // odd: a write may be in flight
        }
    }

    ~StateWriteSeqScope()
    {
        if (engaged)
        {
            g_stateSeq[index].fetch_add(1, std::memory_order_release); // even: the bytes are quiescent again
        }
    }

    StateWriteSeqScope(const StateWriteSeqScope&) = delete;
    StateWriteSeqScope& operator=(const StateWriteSeqScope&) = delete;

private:
    const unsigned int index;
    const bool engaged;
};

inline bool statePagerActive(unsigned int contractIndex)
{
#ifdef LITE_SC_PAGER
    return ContractStatePager::getPager(contractIndex) != nullptr && !g_wasmOwnedSlot[contractIndex];
#else
    (void)contractIndex;
    return false;
#endif
}

inline bool allocateContractState(unsigned int contractIndex, unsigned long long size)
{
#if defined(LITE_SC_PAGER)
    return ContractStatePager::create(&contractStates[contractIndex], size, contractIndex);
#elif defined(LITE_SC_CONTRACT_LEVEL)
    contractStates[contractIndex] = (unsigned char*)qVirtualAlloc(size, /*commitMem=*/true);
    return contractStates[contractIndex] != nullptr;
#else
    return allocPoolWithErrorLog(L"contractStates", size, (void**)&contractStates[contractIndex], __LINE__);
#endif
}

inline void hashContractState(unsigned int contractIndex, unsigned char* output, unsigned long long effectiveSize)
{
    if (statePagerActive(contractIndex))
    {
#ifdef LITE_SC_PAGER
        ContractStatePager::getPager(contractIndex)->getHashAndProtect(output, 32);
#endif
    }
    else
    {
        KangarooTwelve(contractStates[contractIndex], (unsigned int)effectiveSize, output, 32);
    }
}

inline void evictContractState()
{
#ifdef LITE_SC_PAGER
    ContractStatePager::tryEvictBlocks();
#endif
}

inline bool handleManagedStateFault(void* address)
{
#ifdef LITE_SC_PAGER
    return ContractStatePager::handleFault(address);
#else
    (void)address;
    return false;
#endif
}

inline void setContractStateMemoryLimit(unsigned long long bytes)
{
#ifdef LITE_SC_PAGER
    ContractStatePager::MAX_RAM_USAGE = (size_t)bytes;
#else
    (void)bytes;
#endif
}

inline void transferContractStateToWasm(unsigned int contractIndex)
{
#if defined(LITE_SC_PAGER)
    ContractStatePager::release(contractIndex);
    g_wasmOwnedSlot[contractIndex] = true;
#elif defined(LITE_SC_CONTRACT_LEVEL)
    g_wasmOwnedSlot[contractIndex] = true;
#else
    freePool(contractStates[contractIndex]);
#endif
}

// Only the plain pool backend returns state through freePool.
inline void freeContractState(unsigned int contractIndex)
{
#if defined(LITE_SC_PAGER)
    ContractStatePager::release(contractIndex);
#elif defined(LITE_SC_CONTRACT_LEVEL)
    (void)contractIndex;
#else
    if (contractStates[contractIndex])
    {
        freePool(contractStates[contractIndex]);
    }
#endif
}

inline unsigned long long contractStateRamUsage()
{
#ifdef LITE_SC_PAGER
    return ContractStatePager::getTotalRamUsage();
#else
    return 0;
#endif
}

// an initial state staged over rpc in ordered chunks; an http thread fills it, the tick thread takes it once complete
struct StagedState
{
    unsigned char* bytes = nullptr;
    unsigned long long totalBytes = 0;
    unsigned long long receivedBytes = 0;
};

inline StagedState g_stagedStates[contractCount] = {};
inline SmartMutex g_stagedStatesLock{ "stagedStatesLock" };

// set when a native slot's staged state completes, so the tick thread only scans when there is work
inline std::atomic<bool> g_nativeStateStaged{false};

inline void clearStagedState(unsigned int contractIndex)
{
    std::lock_guard<SmartMutex> guard(g_stagedStatesLock);
    StagedState& staged = g_stagedStates[contractIndex];

    free(staged.bytes);
    staged = StagedState{};
}

// returns null on success, else why the chunk was refused; offset 0 restarts the slot's staging buffer
inline const char* stageStateChunk(
    unsigned int contractIndex, unsigned long long offset, unsigned long long totalBytes, const unsigned char* chunk, unsigned long long chunkBytes,
    unsigned long long& receivedBytes)
{
    std::lock_guard<SmartMutex> guard(g_stagedStatesLock);
    StagedState& staged = g_stagedStates[contractIndex];

    if (offset == 0)
    {
        free(staged.bytes);
        staged = StagedState{};
        staged.bytes = (unsigned char*)malloc((size_t)totalBytes);
        if (!staged.bytes)
        {
            return "out of memory";
        }

        staged.totalBytes = totalBytes;
    }

    receivedBytes = staged.receivedBytes;
    if (!staged.bytes || staged.totalBytes != totalBytes || offset != staged.receivedBytes)
    {
        return "chunk is out of order";
    }

    if (chunkBytes > totalBytes - offset)
    {
        return "chunk overruns the total";
    }

    copyMem(staged.bytes + offset, chunk, chunkBytes);
    staged.receivedBytes += chunkBytes;
    receivedBytes = staged.receivedBytes;

    if (staged.receivedBytes == staged.totalBytes && contractIndex < WASM_RESERVED_SLOT_BASE)
    {
        g_nativeStateStaged.store(true, std::memory_order_release);
    }

    return nullptr;
}

// hands a complete staged state to the caller, who frees it; a deploy also drops a half-staged one so it never reaches a later deploy
inline bool takeStagedState(unsigned int contractIndex, unsigned char*& bytes, unsigned long long& totalBytes, bool dropIncomplete)
{
    std::lock_guard<SmartMutex> guard(g_stagedStatesLock);
    StagedState& staged = g_stagedStates[contractIndex];
    const bool complete = staged.bytes && staged.receivedBytes == staged.totalBytes;

    if (!complete)
    {
        if (dropIncomplete)
        {
            free(staged.bytes);
            staged = StagedState{};
        }

        return false;
    }

    bytes = staged.bytes;
    totalBytes = staged.totalBytes;
    staged = StagedState{};
    return true;
}

} // namespace Wasm::Runtime
