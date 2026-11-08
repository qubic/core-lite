#pragma once

// Available system memory (MemAvailable in /proc/meminfo), for code that must leave headroom on
// smaller machines. Returns -1 when unknown (not Linux, or /proc/meminfo unreadable): callers then
// keep their default behaviour.

#include <atomic>
#include <cstring>
#include <cstdlib>

#if defined(__linux__)

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

static inline long long availableMemoryBytes()
{
    FILE* meminfo = fopen("/proc/meminfo", "r");
    if (!meminfo)
    {
        return -1;
    }
    char line[128];
    long long availableKb = -1;
    while (fgets(line, sizeof(line), meminfo))
    {
        if (strncmp(line, "MemAvailable:", 13) == 0)
        {
            availableKb = strtoll(line + 13, nullptr, 10);
            break;
        }
    }
    fclose(meminfo);
    return availableKb < 0 ? -1 : availableKb * 1024;
}

#else

static inline long long availableMemoryBytes()
{
    return -1;
}

#endif

// Value of Private_Dirty in the text of /proc/<pid>/smaps_rollup, in bytes. -1 if absent.
static inline long long parseSmapsRollupPrivateDirtyBytes(const char* text)
{
    const char* field = strstr(text, "Private_Dirty:");
    if (!field)
    {
        return -1;
    }
    return strtoll(field + strlen("Private_Dirty:"), nullptr, 10) * 1024;
}

#if defined(__linux__)

// Private dirty memory of this process, or -1 if unknown. fork() makes every private page shared,
// so during a fork window this is what the parent has copied or allocated since the fork.
// The kernel walks the page tables: this takes seconds on a large node and must stay off the tick
// and BSP threads. Raw syscalls on a stack buffer, so that it holds no libc lock across a fork.
static inline long long processPrivateDirtyBytes()
{
    const int fd = open("/proc/self/smaps_rollup", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return -1;
    }
    char text[4096];
    size_t length = 0;
    while (length < sizeof(text) - 1)
    {
        const ssize_t got = read(fd, text + length, sizeof(text) - 1 - length);
        if (got <= 0)
        {
            break;
        }
        length += (size_t)got;
    }
    close(fd);
    text[length] = 0;
    return parseSmapsRollupPrivateDirtyBytes(text);
}

#else

static inline long long processPrivateDirtyBytes()
{
    return -1;
}

#endif

// Memory guards shared by the fork window and the log cache. The fork window publishes how much
// memory it still needs; the cache stays above that plus a margin, so it gives memory back before
// a window has to close early.
#ifndef LOW_MEMORY_BASE_MARGIN_GB
#define LOW_MEMORY_BASE_MARGIN_GB 4
#endif
#ifndef FORK_MEMORY_SAFETY_PERCENT
#define FORK_MEMORY_SAFETY_PERCENT 125
#endif

namespace memoryHeadroom
{
    inline constexpr long long baseMarginBytes = (long long)LOW_MEMORY_BASE_MARGIN_GB << 30;
    // A fork window closes below this much on top of its need, so the cache (full margin) goes first.
    inline constexpr long long forkReserveBytes = baseMarginBytes / 2;
    inline constexpr unsigned int forkSafetyPercent = FORK_MEMORY_SAFETY_PERCENT;

    // Memory the fork window still needs. Until a window has been measured, one margin is assumed.
    inline std::atomic<long long> gForkNeedBytes{ baseMarginBytes };
    // Cumulative bytes of cache pages taken from the system, so that a window can leave them out of
    // what it measured: they are the cache's cost, already checked when they were taken.
    inline std::atomic<unsigned long long> gCacheGrownBytes{ 0 };

    inline long long cacheFloorBytes()
    {
        return gForkNeedBytes.load(std::memory_order_relaxed) + baseMarginBytes;
    }

    inline long long forkFloorBytes()
    {
        return gForkNeedBytes.load(std::memory_order_relaxed) + forkReserveBytes;
    }

    // Memory for ticks more ticks at bytesPerTick, with the safety factor. No measurement yet (0):
    // one margin, which gives the cache and the window the floors they had when these were fixed.
    inline long long forkNeedBytes(unsigned long long bytesPerTick, unsigned int ticks)
    {
        if (!bytesPerTick)
        {
            return baseMarginBytes;
        }
        return (long long)(bytesPerTick / 100 * forkSafetyPercent * ticks);
    }

    // How many ticks of a window fit in the memory available now, between 1 and maxTicks. Closing a
    // window at its first tick because a full one would not fit would fork on every tick.
    inline unsigned int windowTicksThatFit(long long availableBytes, unsigned long long bytesPerTick, unsigned int maxTicks)
    {
        if (!bytesPerTick)
        {
            return maxTicks;
        }
        const long long room = availableBytes - forkReserveBytes;
        if (room <= 0)
        {
            return 1;
        }
        const unsigned long long fit = (unsigned long long)room / (bytesPerTick / 100 * forkSafetyPercent + 1);
        if (fit < 1)
        {
            return 1;
        }
        return fit < maxTicks ? (unsigned int)fit : maxTicks;
    }

    // Peak per-tick memory of the last windows: the maximum and not the average, so that one heavy
    // window is remembered for a while.
    class WindowRateHistory
    {
    public:
        static constexpr unsigned int capacity = 16;

        // One entry per window; 0 (nothing measured, e.g. the window closed too early) ages out the
        // older ones, so that a stale high rate cannot keep windows short forever.
        void record(unsigned long long bytesPerTick)
        {
            rates[next] = bytesPerTick;
            next = (next + 1) % capacity;
        }

        unsigned long long maxBytesPerTick() const
        {
            unsigned long long highest = 0;
            for (unsigned int i = 0; i < capacity; i++)
            {
                if (rates[i] > highest)
                {
                    highest = rates[i];
                }
            }
            return highest;
        }

        void reset()
        {
            for (unsigned int i = 0; i < capacity; i++)
            {
                rates[i] = 0;
            }
            next = 0;
        }

    private:
        unsigned long long rates[capacity] = {};
        unsigned int next = 0;
    };
}
