#pragma once

// Tick fork-rollback (AUX wrong-solution path): fork on BSP, child keeps networking thread,
// re-spawns AP loops on promote. Linux-only (fork/pipe/_exit) and excluded from Wasm-contract
// builds (Wasm testnet never forks); #else inert stubs.

#include "extensions/tick_fork_barrier.h"

#if defined(__linux__) && !defined(LITE_WASM_SC)

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstdio>
#include <ctime>
#include <atomic>
#include <mutex>
#include <thread>
#include <new>
#include <sys/mman.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sched.h>
#include "platform/memory_headroom.h"
#include "extensions/tick_fork_control.h"

// Fork-path diagnostics: fprintf/stderr is fork-safe (no log-subsystem locks/buffers).
static inline void tickForkLog(const char* message)
{
    fprintf(stderr, "[FORK] %s (pid=%d tick=%u)\n", message, (int)getpid(), (unsigned)system.tick);
    fflush(stderr);
}

// Benchmark helpers (--fork-bench): monotonic ns + parent RSS from /proc/self/status.
static inline long long tickForkNowNs()
{
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (long long)timestamp.tv_sec * 1000000000LL + timestamp.tv_nsec;
}
// Monotonic ms for timeouts (the ns above is for bench precision).
static inline long long tickForkNowMs() { return tickForkNowNs() / 1'000'000LL; }
static inline long tickForkRssKb()
{
    FILE* statusFile = fopen("/proc/self/status", "r");
    if (!statusFile)
        return -1;
    char line[256];
    long rssKb = -1;
    while (fgets(line, sizeof(line), statusFile))
    {
        if (sscanf(line, "VmRSS: %ld kB", &rssKb) == 1)
            break;
    }
    fclose(statusFile);
    return rssKb;
}
// Set by maybeForkBeforeTick / bspForkPoint; consumed by verdict to report one fork's cost.
inline long long gForkWindowStartNs = 0;
inline long long gForkQuiesceNs = 0;     // BSP writer-quiescence duration
inline long long gForkSyscallNs = 0;     // fork() syscall duration (BSP)
inline long gForkRssBeforeKb = 0;        // parent RSS just before fork

namespace tickFork
{
    // Trusting a claimed score is only safe where a tick can be undone.
    inline constexpr bool gRollbackAvailable = true;

    inline std::atomic<bool> gForkRequest{ false };  // tickProcessor -> BSP: fork now
    inline std::atomic<pid_t> gChildPid{ -2 };       // BSP -> tickProcessor: child pid (>=0) / -1 fail / -2 idle
    inline int gPipe[2] = { -1, -1 };                // verdict channel: parent writes [1], child reads [0]
    inline std::atomic<bool> gIsForkChild{ false };  // set in the promoted child

    enum class WindowState
    {
        Idle,
        Checkpointing,
        Live,
        Retiring,
    };
    inline std::atomic<int> gWinState{ (int)WindowState::Idle };

    inline WindowState winState()
    {
        return (WindowState)gWinState.load(std::memory_order_acquire);
    }
    inline void setWinState(WindowState state)
    {
        gWinState.store((int)state, std::memory_order_release);
    }

    // Only ticks carrying a mining-solution tx can mismatch quorum. Ant counts too: AUX commits it on the claimed score.
    inline bool tickHasSolution(unsigned int tick)
    {
        TickData tickDataCopy;
        ts.tickData.acquireLock();
        const TickData* storedTickData = ts.tickData.getByTickIfNotEmpty(tick);
        if (storedTickData)
            copyMem(&tickDataCopy, storedTickData, sizeof(TickData));
        ts.tickData.releaseLock();
        if (!storedTickData)
            return false;

        auto offsets = ts.tickTransactionOffsets.getByTickInCurrentEpoch(tick);
        for (unsigned int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++)
        {
            if (isZero(tickDataCopy.transactionDigests[i]) || !offsets[i])
                continue;
            Transaction* transaction = ts.tickTransactions(offsets[i]);
            if (!transaction->checkValidity())
                continue;
            if (MiningSolutionTransaction::isSolutionTransaction(transaction) || AntColonyMiningSolutionTransaction::isSolutionTransaction(transaction))
                return true;
        }
        return false;
    }

    // Checkpoint-and-replay: k ticks share one fork (amortizes the O(RSS) fork). The checkpoint child
    // stays alive across the window; match keeps it, mismatch rewinds+replays strict, staleness commits.
    // Each checkpoint parks every request processor for a fork and a shadow commit (about 3 s on
    // mainnet), so a much shorter window cannot keep up with a fast network: 64 ticks ran at about
    // 6 ticks/s. A mismatch replays up to this many ticks.
    inline constexpr unsigned int gForkWindowKNominal = 256;
    // Set when a checkpoint is established, so parent and child always agree on the window.
    inline unsigned int gForkWindowK = gForkWindowKNominal;
    inline unsigned int gCheckpointTick = 0;
    // Every page the parent writes during a window is copied for the checkpoint child, so a long
    // window can exhaust RAM on smaller machines. The window is measured while it runs, sized to the
    // memory available when it opens, and closed (committed) early if it outgrows that.
    inline memoryHeadroom::WindowRateHistory gWindowRates;   // tick thread only
    inline std::atomic<unsigned long long> gWindowBytesPerTick{ 0 };  // latest reading of the open window
    inline std::atomic<unsigned long long> gWindowCacheGrownAtFork{ 0 };
    inline std::atomic<unsigned int> gWindowSerial{ 0 };     // tells the sampler that a window changed

    inline void publishForkMemoryNeed(unsigned int ticksLeft)
    {
        memoryHeadroom::gForkNeedBytes.store(memoryHeadroom::forkNeedBytes(gWindowRates.maxBytesPerTick(), ticksLeft),
                                             std::memory_order_relaxed);
    }

    // Reads the window's memory from its own thread: the read takes seconds on a large node. Two
    // readings per window, past the middle so that the rate is not inflated by the first writes,
    // and not near the end: a read running across the next fork() would delay it.
    inline void sampleOpenWindowMemory(unsigned int& sampledSerial, unsigned int& readingsDone)
    {
        if (winState() != WindowState::Live)
            return;
        const unsigned int serial = gWindowSerial.load(std::memory_order_acquire);
        if (serial != sampledSerial)
        {
            sampledSerial = serial;
            readingsDone = 0;
        }
        const unsigned int checkpointTick = gCheckpointTick;
        const unsigned int windowTicks = gForkWindowK;
        const unsigned int ticksDone = (unsigned)system.tick - checkpointTick;
        const unsigned int nextReadingTick = readingsDone == 0 ? windowTicks / 2 : windowTicks * 3 / 4;
        if (readingsDone >= 2 || !ticksDone || ticksDone < nextReadingTick || ticksDone >= windowTicks * 7 / 8)
            return;

        readingsDone++;
        const long long privateBytes = processPrivateDirtyBytes();
        // Discard a reading that overlapped the end of the window: without the child, all is private.
        if (privateBytes < 0 || winState() != WindowState::Live || gWindowSerial.load(std::memory_order_acquire) != serial)
            return;
        const unsigned long long cacheGrownBytes = memoryHeadroom::gCacheGrownBytes.load(std::memory_order_relaxed)
            - gWindowCacheGrownAtFork.load(std::memory_order_relaxed);
        const unsigned long long windowBytes = (unsigned long long)privateBytes > cacheGrownBytes ? (unsigned long long)privateBytes - cacheGrownBytes : 0;
        gWindowBytesPerTick.store(windowBytes / ticksDone, std::memory_order_relaxed);
    }

    inline void* windowMemorySamplerThread(void*)
    {
        // Spawned from the pinned tick thread: move off its core, and yield to node threads.
        cpu_set_t allCpus;
        CPU_ZERO(&allCpus);
        const long cpuCount = sysconf(_SC_NPROCESSORS_ONLN);
        for (long cpu = 0; cpu < cpuCount && cpu < CPU_SETSIZE; cpu++)
            CPU_SET(cpu, &allCpus);
        sched_setaffinity(0, sizeof(allCpus), &allCpus);
        setpriority(PRIO_PROCESS, 0, 10);

        unsigned int sampledSerial = 0;
        unsigned int readingsDone = 0;
        for (;;)
        {
            sampleOpenWindowMemory(sampledSerial, readingsDone);
            usleep(50'000);
        }
        return nullptr;
    }

    // fork() keeps only the calling thread: a promoted child starts its own sampler at its first window.
    inline void ensureWindowMemorySampler()
    {
        static pid_t samplerProcess = 0;
        const pid_t self = getpid();
        if (samplerProcess == self)
            return;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        if (pthread_create(&thread, &attributes, windowMemorySamplerThread, nullptr) == 0)
            samplerProcess = self;
        pthread_attr_destroy(&attributes);
    }

    inline void recordWindowMemory()
    {
        const unsigned long long bytesPerTick = gWindowBytesPerTick.exchange(0, std::memory_order_relaxed);
        gWindowRates.record(bytesPerTick);
        char message[160];
        snprintf(message, sizeof(message), "window memory: %llu MB/tick over %u ticks, peak of last %u windows %llu MB/tick",
                 bytesPerTick >> 20, gForkWindowK, memoryHeadroom::WindowRateHistory::capacity, gWindowRates.maxBytesPerTick() >> 20);
        tickForkLog(message);
    }

    // Last tick the parent started, in a page shared with the checkpoint child. If the parent dies
    // without a verdict (e.g. OOM-killed), the child replays strict only up to that tick instead of
    // the whole window.
    inline std::atomic<unsigned int>* gParentProgressTick = nullptr;

    inline void ensureParentProgressPage()
    {
        if (gParentProgressTick)
            return;
        void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (page != MAP_FAILED)
            gParentProgressTick = new (page) std::atomic<unsigned int>(0);
    }

    inline void noteParentProgress(unsigned int tick)
    {
        if (gParentProgressTick)
            gParentProgressTick->store(tick, std::memory_order_relaxed);
    }

    // In the checkpoint child after the parent died: last tick to replay strict.
    inline unsigned int crashReplayTargetTick(unsigned int checkpointTick)
    {
        const unsigned int windowEnd = checkpointTick + gForkWindowK;
        const unsigned int parentTick = gParentProgressTick ? gParentProgressTick->load(std::memory_order_relaxed) : 0;
        if (parentTick >= checkpointTick && parentTick < windowEnd)
            return parentTick;
        return windowEnd;
    }

    inline bool memoryLowForWindow()
    {
        const long long availableBytes = availableMemoryBytes();
        return availableBytes >= 0 && availableBytes < memoryHeadroom::forkFloorBytes();
    }
    // Deadline to hand work to the BSP and quiesce fork writers before a fork / shadow commit.
    inline constexpr int gForkQuiesceTimeoutMs = 5'000;

    inline bool waitForRequestProcessors(RequestProcessorBarrier& barrier, long long deadlineMs)
    {
        if (!barrier.request())
            return false;

        while (!barrier.allAcknowledged(requestProcessorIDs, nRequestProcessorIDs))
        {
            flushAsyncFileIOBuffer(0);
            if (tickForkNowMs() > deadlineMs)
                return false;
            std::this_thread::yield();
        }
        return true;
    }

    class BspRetireQuiescence
    {
    public:
        BspRetireQuiescence()
#if !defined(NO_RPC)
            : rpcLock(gRpcDispatchLock, std::defer_lock)
#endif
        {
        }

        bool acquire(int timeoutMs)
        {
            const long long deadlineMs = tickForkNowMs() + timeoutMs;
            if (!waitForRequestProcessors(requestProcessorBarrier, deadlineMs))
                return false;

#if !defined(NO_RPC)
            while (!rpcLock.try_lock())
            {
                flushAsyncFileIOBuffer(0);
                if (tickForkNowMs() >= deadlineMs)
                    return false;
                std::this_thread::yield();
            }
#endif
            return true;
        }

        void release()
        {
#if !defined(NO_RPC)
            if (rpcLock.owns_lock())
                rpcLock.unlock();
#endif
            requestProcessorBarrier.release();
        }

        ~BspRetireQuiescence()
        {
            release();
        }

        BspRetireQuiescence(const BspRetireQuiescence&) = delete;
        BspRetireQuiescence& operator=(const BspRetireQuiescence&) = delete;

    private:
        RequestProcessorBarrier requestProcessorBarrier;
#if !defined(NO_RPC)
        std::unique_lock<SmartSharedMutex> rpcLock;
#endif
    };

    class BspForkQuiescence
    {
    public:
        enum class AcquireResult
        {
            Acquired,
            ParkTimeout,
            LockTimeout,
        };

        BspForkQuiescence()
            : networkingLock(Overload::networkingLock, std::defer_lock)
#if !defined(NO_RPC)
            , rpcLock(gRpcDispatchLock, std::defer_lock)
#endif
            , eventMapLock(Overload::eventMapLock, std::defer_lock)
        {
        }

        AcquireResult acquire(int timeoutMs)
        {
            if (!waitForRequestProcessors(requestProcessorBarrier, tickForkNowMs() + timeoutMs))
            {
                return AcquireResult::ParkTimeout;
            }

            snapshotDeadlineMs = tickForkNowMs() + timeoutMs;
            for (;;)
            {
                int failedLockIndex;
#if !defined(NO_RPC)
                failedLockIndex = std::try_lock(networkingLock, rpcLock, eventMapLock);
#else
                failedLockIndex = std::try_lock(networkingLock, eventMapLock);
#endif
                if (failedLockIndex == -1)
                    return AcquireResult::Acquired;

                flushAsyncFileIOBuffer(0);
                if (tickForkNowMs() >= snapshotDeadlineMs)
                    return AcquireResult::LockTimeout;
                std::this_thread::yield();
            }
        }

        int remainingDrainMs() const
        {
            const long long remainingMs = snapshotDeadlineMs - tickForkNowMs();
            return remainingMs > 0 ? (int)remainingMs : 0;
        }

        void release()
        {
            if (eventMapLock.owns_lock())
                eventMapLock.unlock();
#if !defined(NO_RPC)
            if (rpcLock.owns_lock())
                rpcLock.unlock();
#endif
            if (networkingLock.owns_lock())
                networkingLock.unlock();
            requestProcessorBarrier.release();
        }

        // Promoted children reconstruct these mutexes instead of unlocking inherited state.
        void abandonInChild()
        {
            if (eventMapLock.owns_lock())
                (void)eventMapLock.release();
#if !defined(NO_RPC)
            if (rpcLock.owns_lock())
                (void)rpcLock.release();
#endif
            if (networkingLock.owns_lock())
                (void)networkingLock.release();
            requestProcessorBarrier.release();
        }

        ~BspForkQuiescence()
        {
            release();
        }

        BspForkQuiescence(const BspForkQuiescence&) = delete;
        BspForkQuiescence& operator=(const BspForkQuiescence&) = delete;

    private:
        RequestProcessorBarrier requestProcessorBarrier;
        std::unique_lock<SmartMutex> networkingLock;
#if !defined(NO_RPC)
        std::unique_lock<SmartSharedMutex> rpcLock;
#endif
        std::unique_lock<SmartMutex> eventMapLock;
        long long snapshotDeadlineMs = 0;
    };

    // Establish a fresh checkpoint at system.tick. The tick thread waits here while the BSP parks
    // writers, drains VM IO, arms the shadow, and forks.
    inline void establishCheckpoint()
    {
        const long long availableBytes = availableMemoryBytes();
        gForkWindowK = availableBytes < 0 ? gForkWindowKNominal
            : memoryHeadroom::windowTicksThatFit(availableBytes, gWindowRates.maxBytesPerTick(), gForkWindowKNominal);
        publishForkMemoryNeed(gForkWindowK);
        if (gForkBench)
        {
            gForkWindowStartNs = tickForkNowNs();
            gForkRssBeforeKb = tickForkRssKb();
        }
        tickForkLog("checkpoint -> request BSP fork");
        if (gForkWindowK != gForkWindowKNominal)
        {
            char message[96];
            snprintf(message, sizeof(message), "window shortened to %u ticks to fit available memory", gForkWindowK);
            tickForkLog(message);
        }
        ensureWindowMemorySampler();
        gWindowBytesPerTick.store(0, std::memory_order_relaxed);
        gWindowCacheGrownAtFork.store(memoryHeadroom::gCacheGrownBytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
        ensureParentProgressPage();
        noteParentProgress((unsigned)system.tick);
        setWinState(WindowState::Checkpointing);
        ForkStats::onForkRequested();

        // If the fork cannot be set up, score this tick strict so the optimistic pass cannot diverge
        // from quorum: no child is needed and the no-fork verdict won't stall.
        if (pipe(gPipe) != 0)
        {
            gReRunStrict = true;
            gReRunStrictUntilTick = (unsigned)system.tick;
            ForkStats::onForkSkipped(ForkStats::PIPE_FAIL, (unsigned)system.tick, "");
            tickForkLog("pipe() failed -> scoring this tick strict (no fork)");
            setWinState(WindowState::Idle);
            return;
        }
        // O_CLOEXEC: a fork+exec helper must not hold the write end open — child needs EOF on parent crash.
        fcntl(gPipe[0], F_SETFD, fcntl(gPipe[0], F_GETFD) | FD_CLOEXEC);
        fcntl(gPipe[1], F_SETFD, fcntl(gPipe[1], F_GETFD) | FD_CLOEXEC);
        gChildPid = -2;
        gForkRequest = true;
        // Before the BSP claims this request, a stalled main loop requires supervisor recovery.
        // After it claims the request, wait through the non-cancellable fork critical section.
        const long long forkDeadlineMs = tickForkNowMs() + 30'000;
        while (gChildPid == -2)
        {
            // The timeout may cancel only before the BSP claims the request. Once claimed, exiting
            // here could race a live fork just as cancelling a running shadow commit would.
            if (tickForkNowMs() > forkDeadlineMs && gForkRequest.load(std::memory_order_acquire))
            {
                tickForkLog("BSP fork handoff stalled -> fatal exit for supervisor restart");
                _exit(70);
            }
            std::this_thread::yield();
        }

        if (gChildPid < 0)
        {
            close(gPipe[0]);
            close(gPipe[1]);
            gPipe[0] = gPipe[1] = -1;
            gReRunStrict = true;
            gReRunStrictUntilTick = (unsigned)system.tick;
            tickForkLog("fork() failed -> scoring this tick strict (no fork)");
            setWinState(WindowState::Idle);
            return;
        }
        close(gPipe[0]);
        gCheckpointTick = (unsigned)system.tick;
        ForkStats::onForkOk();
        tickForkLog("parent: checkpoint forked, optimistic processTick ahead");
        gWindowSerial.fetch_add(1, std::memory_order_release);
        setWinState(WindowState::Live);
    }

    inline void reapCheckpointChild()
    {
        const pid_t child = gChildPid.load();
        if (child <= 0)
        {
            tickForkLog("checkpoint child missing before commit -> supervisor restart");
            _exit(70);
        }
        if (!tickForkControl::writeRetireCommand(gPipe[1]))
            kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR)
        {
        }
        close(gPipe[1]);
        gPipe[1] = -1;
        gChildPid = -2;
    }

    // Close out a window that completed with no mismatch. The waiting tick thread is already stopped;
    // the BSP owns the remaining writer barrier, VM drain, and commit.
    inline void retireCheckpoint()
    {
        if (winState() != WindowState::Live)
        {
            return;
        }
        tickForkLog("window complete -> commit shadow + reap checkpoint");
        setWinState(WindowState::Retiring);
        recordWindowMemory();
        if (!tickForkControl::gBspRetireHandoff.requestAndWait(gForkQuiesceTimeoutMs))
        {
            // Tell the child to promote — its state is at the checkpoint, it replays the window strict.
            const char tag = tickForkControl::promoteTag;
            const unsigned int targetTick = (unsigned)system.tick;
            write(gPipe[1], &tag, 1);
            write(gPipe[1], &targetTick, sizeof(targetTick));
            tickForkLog("FATAL: swap writers did not quiesce before commit -> child promoted");
            _exit(70);
        }
    }

    // A deliberate node shutdown must not look like a parent crash to the checkpoint child.
    // The BSP sets shutDownNode before releasing its writer barrier, then acknowledges this request.
    inline void retireCheckpointForShutdown()
    {
        if (winState() == WindowState::Live)
        {
            tickForkLog("graceful shutdown -> commit shadow + reap checkpoint");
            setWinState(WindowState::Retiring);
        }
        for (;;)
        {
            if (tickForkControl::gBspRetireHandoff.requestAndWait(gForkQuiesceTimeoutMs, true))
            {
                shutDownNode = 1;
                return;
            }
            tickForkLog("graceful shutdown retirement failed -> retrying without promoting child");
        }
    }

    // tickProcessor, before processTick(system.tick). Maintains the checkpoint window.
    inline void maybeForkBeforeTick(unsigned long long processorNumber)
    {
        (void)processorNumber;
        if (gReRunStrict)
            return;
        if (forceVerifySolutions)
            return;

        if (isMainMode())
        {
            if (winState() == WindowState::Live)
            {
                retireCheckpoint();
            }
            return;
        }

        if (isLastTickInEpoch())
        {
            if (winState() == WindowState::Live)
            {
                retireCheckpoint();
            }
            return;
        }

        if (gForkForceRollbackEvery && (unsigned)system.tick % gForkForceRollbackEvery == 0)
        {
            if (winState() != WindowState::Live)
            {
                establishCheckpoint();
            }
            return;
        }

        switch (winState())
        {
        case WindowState::Idle:
            publishForkMemoryNeed(gForkWindowKNominal);
            if (!gForkForceFork && !tickHasSolution(system.tick))
            {
                return;
            }
            establishCheckpoint();
            break;

        case WindowState::Live:
            if ((unsigned)system.tick - gCheckpointTick < gForkWindowK)
            {
                publishForkMemoryNeed(gForkWindowK - ((unsigned)system.tick - gCheckpointTick));
                if (!memoryLowForWindow())
                {
                    noteParentProgress((unsigned)system.tick);
                    return;
                }
                tickForkLog("low available memory -> close window early");
            }
            retireCheckpoint();
            if (gForkForceFork || tickHasSolution(system.tick))
            {
                establishCheckpoint();
            }
            break;

        case WindowState::Checkpointing:
        case WindowState::Retiring:
            break;
        }
    }

    // At the quorum compare. Returns true if the checkpoint window handled this tick.
    inline bool verdict(bool mismatch, const m256i& quorumSpectrumDigest, unsigned long long processorNumber)
    {
        (void)quorumSpectrumDigest;
        (void)processorNumber;
        if (winState() != WindowState::Live)
            return false;

        if (gForkForceMatch)
            mismatch = false;      // test: exercise the keep-checkpoint path
        if (gForkForceMismatch)
            mismatch = true;       // test: force rewind (parent _exit + child replays)
        if (gForkForceRollbackEvery && (unsigned)system.tick % gForkForceRollbackEvery == 0)
            mismatch = true;       // test: periodic forced rollback
        if (gShadowPoisoned.load(std::memory_order_acquire))
            mismatch = true;       // shadow I/O failed -> replay strict from pristine

        if (gForkBench)
        {
            const long long windowDurationNs = tickForkNowNs() - gForkWindowStartNs;
            const long rssAfterKb = tickForkRssKb();
            fprintf(stderr, "[FORK-BENCH] tick=%u %s ckpt=%u window=%.2fms quiesce=%.2fms fork()=%.3fms "
                "rss: before=%ldMB after=%ldMB cow_delta=%ldMB\n", (unsigned)system.tick, mismatch ? "MISMATCH" : "MATCH", gCheckpointTick,
                windowDurationNs / 1e6, gForkQuiesceNs / 1e6, gForkSyscallNs / 1e6, gForkRssBeforeKb / 1024, rssAfterKb / 1024,
                (rssAfterKb - gForkRssBeforeKb) / 1024);
            fflush(stderr);
        }

        ForkStats::onVerdict(mismatch);

        if (!mismatch)
        {
            // Keep the checkpoint for the rest of the window; committed + reaped at staleness.
            if (gForkBench)
            {
                tickForkLog("verdict MATCH: keep checkpoint (window)");
            }
            return true;
        }

        // Mismatch: leave shadowing active and exit. The child waits for pipe EOF before purging the
        // registered shadow directories, so a late compressed write can never fall through to real.
        tickForkLog("verdict MISMATCH: rewind to checkpoint + parent _exit");
        const unsigned int targetTick = (unsigned)system.tick;
        const char tag = tickForkControl::promoteTag;
        ssize_t writeSize = write(gPipe[1], &tag, 1);
        (void)writeSize;
        writeSize = write(gPipe[1], &targetTick, sizeof(targetTick));
        (void)writeSize;
        _exit(0);
    }
}

#else  // non-Linux or Wasm build: rollback disabled; inert stubs keep qubic.cpp building.

#include <atomic>
namespace tickFork
{
    // No checkpoint on this build, so every optimistic shortcut stays off.
    inline constexpr bool gRollbackAvailable = false;

    inline std::atomic<bool> gIsForkChild{ false };
    inline std::atomic<bool> gForkRequest{ false };
    inline void maybeForkBeforeTick(unsigned long long) {}
    inline bool verdict(bool mismatch, const m256i& quorumSpectrumDigest, unsigned long long processorNumber)
    {
        (void)mismatch;
        (void)quorumSpectrumDigest;
        (void)processorNumber;
        return false;
    }
}

#endif // __linux__ && !LITE_WASM_SC
