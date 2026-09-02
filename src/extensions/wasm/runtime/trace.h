#pragma once

// Per-call trace ring and host-call capture for testnet development.
#ifdef LITE_WASM_SC

#include <atomic>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// One slot per dispatch, not per tick — a contract registering BEGIN_TICK/END_TICK spends two of these
// every tick whether or not anything happened, so the depth is what a reader can still scroll back to.
#ifndef WASM_TRACE_RING_CAPACITY
#define WASM_TRACE_RING_CAPACITY 8192u
#endif
// Changed bytes are reported in aligned windows of this size, so a reader can decode the value the
// change landed in rather than the bytes that happened to differ.
#ifndef WASM_TRACE_DIFF_WINDOW
#define WASM_TRACE_DIFF_WINDOW 256u
#endif

namespace Wasm::Runtime
{

struct HostCallTrace
{
    const char* name;
    std::string detail;
};

struct StateRegionTrace
{
    unsigned int offset;
    std::string before;
    std::string after;
};

struct LogTrace
{
    unsigned char type = 0;
    unsigned int size = 0;
    std::string hex;
};

// One CC_PRINT argument. size == 0 means the value came through by register instead of by pointer.
struct CheatEntry
{
    unsigned int id = 0;
    unsigned char part = 0;
    unsigned int size = 0;
    unsigned long long value = 0;
    std::string hex;
};

struct TraceEntry
{
    unsigned long long sequence = 0;
    unsigned int tick = 0;
    unsigned int contractIndex = 0;
    unsigned short inputType = 0;
    unsigned char kind = 0;
    bool ok = true;
    bool used = false;
    m256i invocator = m256i::zero();
    long long invocationReward = 0;
    unsigned int inputSize = 0;
    unsigned int outputSize = 0;
    unsigned int stateSize = 0;
    bool stateTruncated = false;
    std::vector<unsigned char> input;
    std::vector<unsigned char> output;
    unsigned long long executionNanoseconds = 0;
    std::string trap;
    std::vector<StateRegionTrace> stateDiff;
    std::vector<HostCallTrace> hostCalls;
    std::vector<LogTrace> logs;
    std::vector<CheatEntry> cheats;
};

// On by default so a debugger attached after the fact still finds the calls that mattered. This header
// is testnet-only (LITE_WASM_SC), so the choice cannot reach a production node.
static std::atomic<bool> traceActive{ true };

static inline bool traceEnabled()
{
    return traceActive.load(std::memory_order_relaxed);
}

static TraceEntry traceRing[WASM_TRACE_RING_CAPACITY];
static volatile long g_liteWasmTraceLock = 0;
static unsigned int traceWriteIndex = 0;
static unsigned long long traceSequence = 0;

#ifdef _MSC_VER
static inline void acquireTraceLock()
{
    while (_InterlockedExchange(&g_liteWasmTraceLock, 1))
    {
    }
}

static inline void releaseTraceLock()
{
    _InterlockedExchange(&g_liteWasmTraceLock, 0);
}
#else
static inline void acquireTraceLock()
{
    while (__sync_lock_test_and_set(&g_liteWasmTraceLock, 1))
    {
    }
}

static inline void releaseTraceLock()
{
    __sync_lock_release(&g_liteWasmTraceLock);
}
#endif

struct TraceLockScope
{
    TraceLockScope()
    {
        acquireTraceLock();
    }

    ~TraceLockScope()
    {
        releaseTraceLock();
    }

    TraceLockScope(const TraceLockScope&) = delete;
    TraceLockScope& operator=(const TraceLockScope&) = delete;
};

static inline void recordHostCall(TraceEntry* entry, const char* name, const std::string& detail)
{
    if (entry)
    {
        entry->hostCalls.push_back({
            name, detail,
        });
    }
}

static inline void commitTrace(TraceEntry& entry)
{
    TraceLockScope lock;

    entry.sequence = ++traceSequence;
    entry.used = true;
    traceRing[traceWriteIndex % WASM_TRACE_RING_CAPACITY] = entry;
    traceWriteIndex++;
}

static inline std::vector<TraceEntry> traceSnapshot(unsigned long long since, unsigned int limit)
{
    std::vector<TraceEntry> entries;

    {
        TraceLockScope lock;

        for (unsigned int index = 0; index < WASM_TRACE_RING_CAPACITY; index++)
        {
            if (traceRing[index].used && traceRing[index].sequence > since)
            {
                entries.push_back(traceRing[index]);
            }
        }
    }

    std::sort(entries.begin(), entries.end(), [](const TraceEntry& left, const TraceEntry& right)
        {
            return left.sequence < right.sequence;
        });

    if (entries.size() > limit)
    {
        entries.erase(entries.begin(), entries.end() - limit);
    }

    return entries;
}

static inline void clearTrace()
{
    TraceLockScope lock;

    for (unsigned int index = 0; index < WASM_TRACE_RING_CAPACITY; index++)
    {
        traceRing[index].used = false;
    }

    traceWriteIndex = 0;
}

static inline std::string hex(const void* bytes, unsigned int size)
{
    if (!bytes)
    {
        return "null";
    }

    static const char* digits = "0123456789abcdef";
    const unsigned char* input = (const unsigned char*)bytes;
    std::string result;

    result.reserve(size * 2);
    for (unsigned int index = 0; index < size; index++)
    {
        result += digits[input[index] >> 4];
        result += digits[input[index] & 15];
    }

    return result;
}

static inline void recordLog(TraceEntry* entry, unsigned char type, const void* bytes, unsigned int size)
{
    if (!entry)
    {
        return;
    }

    entry->logs.push_back(LogTrace{
        type, size, hex(bytes, size),
    });
}

// The development print channel. Deliberately separate from logs: it consumes no log id, reaches no
// qLogger, and is stripped from the contract before submission.
static inline void recordCheat(TraceEntry* entry, unsigned int id, unsigned char part, unsigned long long value, const void* bytes, unsigned int size)
{
    if (!entry)
    {
        return;
    }

    // A guest offset outside linear memory resolves to null; record the size but never read from it.
    const bool readable = bytes && size;

    entry->cheats.push_back(CheatEntry{
        id, part, size, value, readable ? hex(bytes, size) : std::string(),
    });
}

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
