#pragma once

#include <atomic>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#include <string.h>
#endif

// The main thread pins itself to a single CPU during startup (see Overload::initializeUefi), and on
// builds whose libc derives the processor count from the affinity mask rather than from sysfs, every
// later std::thread::hardware_concurrency() then answers 1 — which is enough to make the node refuse to
// start ("At least 4 healthy enabled processors are required"). Take the count once during static
// initialisation, before any affinity is set, and answer from it everywhere.
inline const unsigned int gTotalProcessorCount = std::thread::hardware_concurrency();

inline unsigned int totalProcessorCount()
{
    return gTotalProcessorCount;
}

// Runs work(first, last) over [0, count) in blocks on every processor and returns once all of it is
// done. For one-off startup passes: the threads live only for the call.
template<typename Work>
inline void runOnAllProcessors(unsigned long long count, unsigned long long blockSize, const Work& work)
{
    std::atomic<unsigned long long> nextBlock{ 0 };
    const auto worker = [&]()
    {
#if defined(__linux__)
        // A new thread inherits its creator's affinity, and the main thread is pinned to one CPU.
        cpu_set_t everyProcessor;
        memset(&everyProcessor, 0xFF, sizeof(everyProcessor));
        sched_setaffinity(0, sizeof(everyProcessor), &everyProcessor);
#endif
        for (unsigned long long first = nextBlock.fetch_add(blockSize); first < count; first = nextBlock.fetch_add(blockSize))
        {
            work(first, first + blockSize < count ? first + blockSize : count);
        }
    };

    std::vector<std::thread> threads;
    const unsigned int threadCount = totalProcessorCount() > 0 ? totalProcessorCount() : 1;
    for (unsigned int i = 0; i < threadCount; i++)
    {
        threads.emplace_back(worker);
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
}
