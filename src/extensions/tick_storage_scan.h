#pragma once

#ifdef __linux__

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <signal.h>
#include <string>
#include <vector>
#include <unistd.h>

namespace tickStorageScan
{
    static constexpr unsigned long long detailLimit = 20;

#if defined(USE_SWAP) && TICK_STORAGE_AUTOSAVE_MODE
    static bool cleanupPending = false;
    static bool shadowArmed = false;
    static bool signalHandlerInstalled = false;
    static struct sigaction previousSegmentationFaultAction;
    static struct sigaction defaultSegmentationFaultAction;

    static void handleSegmentationFault(int signal, siginfo_t* info, void*)
    {
        if (info && SwapDirtyTrack::tryMarkDirty(info->si_addr))
            return;

        sigaction(SIGSEGV, &defaultSegmentationFaultAction, nullptr);
        raise(signal);
        _exit(1);
    }

    static bool installSignalHandler()
    {
        struct sigaction action;
        std::memset(&action, 0, sizeof(action));
        action.sa_sigaction = handleSegmentationFault;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);

        std::memset(&defaultSegmentationFaultAction, 0, sizeof(defaultSegmentationFaultAction));
        defaultSegmentationFaultAction.sa_handler = SIG_DFL;
        sigemptyset(&defaultSegmentationFaultAction.sa_mask);
        if (sigaction(SIGSEGV, &action, &previousSegmentationFaultAction) != 0)
            return false;

        signalHandlerInstalled = true;
        return true;
    }

    static bool discardShadow()
    {
        if (!shadowArmed)
            return true;

        gShadow.discard();
        const bool purged = gShadow.purgeOrphans();
        shadowArmed = false;
        return purged;
    }

    static void cleanup()
    {
        if (!cleanupPending)
            return;

        deInitFileSystem();
        discardShadow();

        if (signalHandlerInstalled)
        {
            sigaction(SIGSEGV, &previousSegmentationFaultAction, nullptr);
            signalHandlerInstalled = false;
        }

        setNonBlockingInput(false);
        cleanupPending = false;
    }
#endif

    // Skip ticks before this one, for re-checking a known bad range without walking the whole epoch.
    static unsigned int scanFromTick = 0;

    static bool requested(int argc, const char* argv[])
    {
        bool scanRequested = false;
        for (int i = 1; i < argc; i++)
        {
            if (std::strcmp(argv[i], "--scan-issue") == 0)
                scanRequested = true;
            else if (std::strcmp(argv[i], "--scan-from") == 0 && i + 1 < argc)
                scanFromTick = (unsigned int)std::strtoul(argv[++i], nullptr, 10);
        }
        return scanRequested;
    }

    struct IssueSummary
    {
        unsigned long long total = 0;
        std::map<std::string, unsigned long long> categoryTotals;

        void add(const char* category, unsigned int tick, long long slot, const char* format, ...)
        {
            total++;
            categoryTotals[category]++;

            if (total > detailLimit)
                return;

            std::fprintf(stderr, "\n[ISSUE] %s tick=%u", category, tick);
            if (slot >= 0)
                std::fprintf(stderr, " slot=%lld", slot);
            std::fprintf(stderr, ": ");

            va_list args;
            va_start(args, format);
            std::vfprintf(stderr, format, args);
            va_end(args);
            std::fprintf(stderr, "\n");
        }

        void print() const
        {
            if (total > detailLimit)
            {
                std::fprintf(stderr, "[SCAN] %llu additional issue details suppressed\n", total - detailLimit);
            }

            for (const auto& category : categoryTotals)
            {
                std::fprintf(stderr, "[SCAN] %-24s %llu\n", category.first.c_str(), category.second);
            }
        }
    };

    class Progress
    {
        using Clock = std::chrono::steady_clock;

        static constexpr int barWidth = 30;
        bool outputEnabled;
        bool terminal;
        Clock::time_point started = Clock::now();
        Clock::time_point lastUpdate = started;

    public:
        explicit Progress(bool enabled = true)
            : outputEnabled(enabled)
            , terminal(enabled && isatty(STDOUT_FILENO))
        {
        }

        void update(unsigned long long completedTicks, unsigned long long totalTicks, unsigned long long transactions, unsigned long long issues,
            bool force = false)
        {
            if (!outputEnabled)
                return;

            const Clock::time_point now = Clock::now();
            const double sinceUpdate = std::chrono::duration<double>(now - lastUpdate).count();
            const double updateInterval = terminal ? 0.25 : 5.0;
            if (!force && sinceUpdate < updateInterval)
                return;

            lastUpdate = now;
            const double elapsed = std::chrono::duration<double>(now - started).count();
            const double rate = elapsed > 0 ? completedTicks / elapsed : 0;
            const double fraction = totalTicks ? (double)completedTicks / totalTicks : 1;
            const unsigned long long remainingTicks = totalTicks - completedTicks;
            const unsigned long long etaSeconds = rate > 0 ? (unsigned long long)(remainingTicks / rate) : 0;

            if (terminal)
            {
                const int filled = (int)(fraction * barWidth);
                std::string bar((size_t)filled, '=');
                if (filled < barWidth)
                    bar += '>';
                bar.append((size_t)(barWidth - (int)bar.size()), ' ');

                std::fprintf(stdout, "\r[SCAN] [%s] %6.2f%% %llu/%llu ticks | %llu tx | %llu issues | %.1f tick/s | ETA %02llu:%02llu:%02llu",
                    bar.c_str(), fraction * 100, completedTicks, totalTicks, transactions, issues, rate,
                    etaSeconds / 3600, (etaSeconds / 60) % 60, etaSeconds % 60);
                if (force)
                    std::fprintf(stdout, "\n");
            }
            else
            {
                std::fprintf(stdout, "[SCAN] %.2f%% %llu/%llu ticks | %llu tx | %llu issues | %.1f tick/s | ETA %02llu:%02llu:%02llu\n",
                    fraction * 100, completedTicks, totalTicks, transactions, issues, rate, etaSeconds / 3600, (etaSeconds / 60) % 60, etaSeconds % 60);
            }
            std::fflush(stdout);
        }
    };

    struct TransactionReference
    {
        unsigned long long offset;
        unsigned int slot;
        m256i expectedDigest;
        bool hasExpectedDigest;
    };

    struct ScanResult
    {
        IssueSummary issues;
        unsigned long long transactionsChecked = 0;
    };

    static ScanResult scanLoadedRange(TickStorage& storage, unsigned short epoch, unsigned int initialTick, unsigned int endTick, Progress& progress)
    {
        ScanResult result;
        IssueSummary& issues = result.issues;
        const unsigned long long tickCount = (unsigned long long)endTick - initialTick;
        const unsigned long long transactionLimit = storage.nextTickTransactionOffset;
        const unsigned long long transactionCapacity = storage.tickTransactions.storageSpaceCurrentEpoch;
        const bool transactionLimitValid = transactionLimit >= FIRST_TICK_TRANSACTION_OFFSET && transactionLimit <= transactionCapacity;
        if (!transactionLimitValid)
        {
            issues.add("transaction-layout", initialTick, -1, "next offset %llu is outside [%llu, %llu]", transactionLimit,
                (unsigned long long)FIRST_TICK_TRANSACTION_OFFSET, transactionCapacity);
        }

        std::vector<TransactionReference> transactionReferences;
        transactionReferences.reserve(NUMBER_OF_TRANSACTIONS_PER_TICK);
        progress.update(0, tickCount, result.transactionsChecked, issues.total, true);

        for (unsigned int tick = initialTick; tick < endTick; tick++)
        {
            PinScope tickPinScope;
            transactionReferences.clear();

            const TickData& tickData = storage.tickData.getByTickInCurrentEpoch(tick);
            const bool hasTickData = tickData.epoch == epoch;
            const bool tickDataValid = hasTickData && tickData.tick == tick && tickData.computorIndex == tick % NUMBER_OF_COMPUTORS;
            if (hasTickData && !tickDataValid)
            {
                issues.add("tick-data", tick, -1, "epoch=%u storedTick=%u leader=%u expectedLeader=%u", tickData.epoch, tickData.tick, tickData.computorIndex,
                    tick % NUMBER_OF_COMPUTORS);
            }
            else if (!hasTickData && tickData.epoch != 0 && tickData.epoch != INVALIDATED_TICK_DATA)
            {
                issues.add("tick-data", tick, -1, "invalid epoch marker %u", tickData.epoch);
            }

            m256i expectedTickDataDigest = m256i::zero();
            if (tickDataValid)
            {
                KangarooTwelve(&tickData, sizeof(TickData), &expectedTickDataDigest, sizeof(expectedTickDataDigest));
            }

            const Tick* tickVotes = storage.ticks.getByTickInCurrentEpoch(tick);
            unsigned int matchingVotes = 0;
            for (unsigned int computor = 0; computor < NUMBER_OF_COMPUTORS; computor++)
            {
                const Tick& vote = tickVotes[computor];
                if (vote.epoch == 0)
                    continue;

                if (vote.epoch != epoch || vote.tick != tick || vote.computorIndex != computor)
                {
                    issues.add("tick-vote", tick, computor, "epoch=%u storedTick=%u computor=%u", vote.epoch, vote.tick, vote.computorIndex);
                    continue;
                }

                if (vote.transactionDigest == expectedTickDataDigest)
                    matchingVotes++;
            }
            if (matchingVotes < QUORUM)
            {
                issues.add("tick-quorum", tick, -1, "%u matching votes, need %u", matchingVotes, (unsigned int)QUORUM);
            }

            if (!tickDataValid)
            {
                progress.update((unsigned long long)tick - initialTick + 1, tickCount, result.transactionsChecked, issues.total);
                continue;
            }

            const unsigned long long* offsets = storage.tickTransactionOffsets.getByTickInCurrentEpoch(tick);
            for (unsigned int slot = 0; slot < NUMBER_OF_TRANSACTIONS_PER_TICK; slot++)
            {
                const unsigned long long offset = offsets[slot];
                const bool hasDigest = !isZero(tickData.transactionDigests[slot]);
                if (offset == 0)
                {
                    if (hasDigest)
                    {
                        issues.add("transaction-pair", tick, slot, "digest has no transaction offset");
                    }
                    continue;
                }

                TransactionReference reference;
                reference.offset = offset;
                reference.slot = slot;
                reference.expectedDigest = hasDigest ? tickData.transactionDigests[slot]
                    : m256i::zero();
                reference.hasExpectedDigest = hasDigest;
                transactionReferences.push_back(reference);

                if (!hasDigest)
                {
                    issues.add("transaction-pair", tick, slot, "offset %llu has no transaction digest", offset);
                }
            }

            std::sort(transactionReferences.begin(), transactionReferences.end(), [](const TransactionReference& left, const TransactionReference& right)
                {
                    return left.offset < right.offset;
                });

            unsigned long long previousOffset = 0;
            for (const TransactionReference& reference : transactionReferences)
            {
                result.transactionsChecked++;
                if (reference.offset == previousOffset)
                {
                    issues.add("transaction-offset", tick, reference.slot, "duplicate offset %llu", reference.offset);
                }
                previousOffset = reference.offset;

                if (!transactionLimitValid || reference.offset < FIRST_TICK_TRANSACTION_OFFSET || reference.offset >= transactionLimit
                    || transactionLimit - reference.offset < sizeof(Transaction))
                {
                    issues.add("transaction-offset", tick, reference.slot, "offset %llu is outside stored transaction bytes", reference.offset);
                    continue;
                }

                PinScope transactionPinScope;
                const Transaction* transaction = storage.tickTransactions(reference.offset);
                const unsigned int transactionSize = transaction->totalSize();
                if (!transaction->checkValidity() || transactionSize > MAX_TRANSACTION_SIZE || transactionSize > transactionLimit - reference.offset)
                {
                    issues.add("transaction-header", tick, reference.slot, "offset=%llu inputSize=%u totalSize=%u",
                        reference.offset, transaction->inputSize, transactionSize);
                    continue;
                }

                if (transaction->tick != tick)
                {
                    issues.add("transaction-tick", tick, reference.slot, "offset=%llu storedTick=%u", reference.offset, transaction->tick);
                }

                if (reference.hasExpectedDigest)
                {
                    m256i transactionDigest;
                    KangarooTwelve(transaction, transactionSize, &transactionDigest, sizeof(transactionDigest));
                    if (transactionDigest != reference.expectedDigest)
                    {
                        issues.add("transaction-digest", tick, reference.slot, "digest mismatch at offset %llu", reference.offset);
                    }
                }
            }

            progress.update((unsigned long long)tick - initialTick + 1, tickCount, result.transactionsChecked, issues.total);
        }

        return result;
    }

    static void scanLoggingState(const CHAR16* snapshotDirectory, unsigned int startTick, IssueSummary& issues, unsigned long long& logIds,
        unsigned long long& logBytes)
    {
#if ENABLED_LOGGING
        static CHAR16 loggingFileName[] = L"logEventState.db";
        if (getFileSize(loggingFileName, (CHAR16*)snapshotDirectory) <= 0)
        {
            std::fprintf(stderr, "[SCAN] logging: no logEventState.db (logging disabled), skipping\n");
            return;
        }

        if (!commonBuffers.init(1, qLogger::logStateBufferSize))
        {
            issues.add("logging", system.tick, -1, "cannot allocate logging scratchpad");
            return;
        }
        if (!logger.initLogging() || !logger.loadLastLoggingStates((CHAR16*)snapshotDirectory))
        {
            issues.add("logging", system.tick, -1, "logEventState.db load failed");
            logger.deinitLogging();
            commonBuffers.deinit();
            return;
        }

        // one index entry per log event
        const unsigned long long logCount = logger.logId;
        const unsigned long long indexCount = qLogger::mapLogIdToBufferIndex.size();
        if (indexCount != logCount)
            issues.add("logging-idmap", system.tick, -1, "logId=%llu but %llu index entries", logCount, indexCount);

        const unsigned long long tickRangeCount = qLogger::mapTxToLogId.size();
        const unsigned long long startOffset = startTick > logger.tickBegin ? (unsigned long long)startTick - logger.tickBegin
            : 0;
        const bool partialScan = startOffset > 0;

        // A partial scan cannot verify continuity back to logId 0, so it begins at the first
        // log event of the first non-empty tick blob in range.
        unsigned long long startLogId = 0;
        if (partialScan)
        {
            startLogId = logCount;
            for (unsigned long long o = startOffset; o < tickRangeCount && startLogId == logCount; o++)
            {
                qLogger::TickBlobInfo tbi;
                qLogger::mapTxToLogId.getOne(o, &tbi);
                for (int t = 0; t < LOG_TX_PER_TICK; t++)
                {
                    const long long from = tbi.fromLogId[t];
                    if (from >= 0 && (unsigned long long)from < startLogId)
                        startLogId = (unsigned long long)from;
                }
            }
        }

        // per-entry header + contiguous layout
        char header[LOG_HEADER_SIZE];
        unsigned long long previousEnd = 0, firstEntryStart = 0;
        bool haveEntryLayout = false;
        const unsigned long long checkLimit = logCount < indexCount ? logCount : indexCount;
        for (unsigned long long i = startLogId; i < checkLimit; i++)
        {
            auto bi = logger.logBuf.getBlobInfo(i);
            if (bi.startIndex < 0 || bi.length < 0)
            {
                issues.add("logging-index", system.tick, -1, "logId %llu missing index", i);
                continue;
            }
            if (!haveEntryLayout)
            {
                firstEntryStart = (unsigned long long)bi.startIndex;
                previousEnd = firstEntryStart;
                haveEntryLayout = true;
            }
            if (bi.startIndex != previousEnd)
                issues.add("logging-layout", system.tick, -1, "logId %llu gap/overlap at %lld (expected %llu)", i, bi.startIndex, previousEnd);
            const unsigned long long entryEnd = (unsigned long long)(bi.startIndex + bi.length);
            if (entryEnd > logger.logBufferTail)
                issues.add("logging-layout", system.tick, -1, "logId %llu overruns logBufferTail", i);

            setMem(header, sizeof(header), 0);
            logger.logBuf.getMany(header, (unsigned long long)bi.startIndex, LOG_HEADER_SIZE);
            if (logger.getLogId(header) != i)
                issues.add("logging-id", system.tick, -1, "logId %llu header says %llu", i, logger.getLogId(header));
            if ((long long)(LOG_HEADER_SIZE + logger.getLogSize(header)) != bi.length)
                issues.add("logging-size", system.tick, -1, "logId %llu size=%u vs length=%lld", i, logger.getLogSize(header), bi.length);
            const unsigned short epoch = *((unsigned short*)header);
            const unsigned int tick = *((unsigned int*)(header + 2));
            if (epoch != system.epoch || tick < logger.tickBegin || tick > logger.lastUpdatedTick)
                issues.add("logging-header", system.tick, -1, "logId %llu epoch=%u tick=%u", i, epoch, tick);
            previousEnd = entryEnd;
        }
        if (haveEntryLayout && previousEnd != logger.logBufferTail)
            issues.add("logging-tail", system.tick, -1, "entries cover %llu bytes but tail=%llu", previousEnd, logger.logBufferTail);

        // Each tick's slot ranges form one contiguous logId block, and blocks are consecutive.
        // Slot index order is not logId order: the special slots (SC_BEGIN_TICK, SC_NOTIFICATION)
        // log before lower-indexed tx slots, so the block is checked as a whole, not slot by slot.
        const unsigned long long expectedTickCount = logger.lastUpdatedTick >= logger.tickBegin
            ? (unsigned long long)logger.lastUpdatedTick - logger.tickBegin + 1 : 0;
        if (tickRangeCount != expectedTickCount)
            issues.add("logging-ticks", system.tick, -1, "%llu tick ranges vs expected %llu", tickRangeCount, expectedTickCount);
        unsigned long long expectedLogId = startLogId;
        for (unsigned long long o = startOffset; o < tickRangeCount; o++)
        {
            const unsigned int tick = logger.tickBegin + (unsigned int)o;
            qLogger::TickBlobInfo tbi;
            qLogger::mapTxToLogId.getOne(o, &tbi);

            unsigned long long blockBegin = 0, blockEnd = 0, coveredLogIds = 0;
            bool hasSlot = false;
            for (int t = 0; t < LOG_TX_PER_TICK; t++)
            {
                const long long from = tbi.fromLogId[t];
                const long long len = tbi.length[t];
                if (from < 0 && len < 0)
                    continue;

                if (from < 0 || len < 1 || (unsigned long long)from + len > logCount)
                {
                    issues.add("logging-txslot", tick, t, "fromLogId=%lld len=%lld outside [0, %llu)", from, len, logCount);
                    continue;
                }

                const unsigned long long slotEnd = (unsigned long long)from + len;
                if (!hasSlot || (unsigned long long)from < blockBegin)
                    blockBegin = (unsigned long long)from;
                if (!hasSlot || slotEnd > blockEnd)
                    blockEnd = slotEnd;
                coveredLogIds += (unsigned long long)len;
                hasSlot = true;
            }

            if (!hasSlot)
                continue;

            if (coveredLogIds != blockEnd - blockBegin)
                issues.add("logging-txrange", tick, -1, "slots cover %llu logIds in block [%llu, %llu)", coveredLogIds, blockBegin, blockEnd);
            if (blockBegin != expectedLogId)
                issues.add("logging-txrange", tick, -1, "block starts at %llu, expected %llu", blockBegin, expectedLogId);

            expectedLogId = blockEnd; // resync so one bad tick cannot cascade into the rest
        }
        if (tickRangeCount > startOffset && expectedLogId != logCount)
            issues.add("logging-txrange", logger.lastUpdatedTick, -1, "tick blocks cover %llu logIds vs %llu total", expectedLogId, logCount);

        logIds = logCount - startLogId;
        logBytes = haveEntryLayout ? previousEnd - firstEntryStart : 0;
        logger.deinitLogging();
        commonBuffers.deinit();
#endif
    }

    static int runScan()
    {
#if !defined(USE_SWAP) || !TICK_STORAGE_AUTOSAVE_MODE
        std::fprintf(stderr, "[SCAN] --scan-issue requires USE_SWAP and tick-storage snapshots\n");
        return 2;
#else
        Overload::initializeUefi();
        cleanupPending = true;
        std::atexit(cleanup);

        std::fprintf(stdout, "[SCAN] Offline mode: stop the node before scanning its runtime directory\n");
        enableAVX();
        if (!installSignalHandler())
        {
            std::fprintf(stderr, "[SCAN] cannot install SwapVM dirty-tracking handler\n");
            return 2;
        }
        gSwapDirtyTrackEnabled = true;
        initTime();
        if (!initFilesystem())
        {
            std::fprintf(stderr, "[SCAN] filesystem initialization failed\n");
            return 2;
        }

        CHAR16 snapshotDirectory[16];
        setText(snapshotDirectory, L"ep");
        appendNumber(snapshotDirectory, EPOCH, false);
        if (!checkDir(snapshotDirectory))
        {
            std::fprintf(stderr, "[SCAN] snapshot directory ep%u does not exist\n", (unsigned int)EPOCH);
            return 2;
        }

        static CHAR16 systemSnapshotFileName[] = L"system.snp";
        setMem(&system, sizeof(system), 0);
        const long long loadedSystemSize = load(systemSnapshotFileName, sizeof(system), (unsigned char*)&system, snapshotDirectory);
        if (loadedSystemSize != sizeof(system))
        {
            std::fprintf(stderr, "[SCAN] failed to load ep%u/system.snp\n", (unsigned int)EPOCH);
            return 2;
        }

        const bool systemRangeValid = system.epoch == EPOCH && system.tick >= system.initialTick;
        if (!systemRangeValid || (unsigned long long)system.tick - system.initialTick > MAX_NUMBER_OF_TICKS_PER_EPOCH)
        {
            std::fprintf(stderr, "[SCAN] invalid system snapshot: epoch=%u initialTick=%u tick=%u\n", system.epoch, system.initialTick, system.tick);
            return 2;
        }

        unsigned int startTick = system.initialTick;
        if (scanFromTick > startTick)
        {
            startTick = scanFromTick < system.tick ? scanFromTick : system.tick;
        }
        const unsigned long long tickCount = (unsigned long long)system.tick - startTick;
        std::fprintf(stdout, "[SCAN] epoch %u range: ticks %u..%u (initialTick=%u)\n", system.epoch, startTick, system.tick, system.initialTick);

        EFI_GUID mpServiceProtocolGuid = EFI_MP_SERVICES_PROTOCOL_GUID;
        const EFI_STATUS locateProtocolStatus = bs->LocateProtocol(&mpServiceProtocolGuid, nullptr, (void**)&mpServicesProtocol);
        if (locateProtocolStatus != EFI_SUCCESS || !mpServicesProtocol)
        {
            std::fprintf(stderr, "[SCAN] async file-I/O initialization failed\n");
            return 2;
        }
        const EFI_STATUS processorStatus = mpServicesProtocol->WhoAmI(mpServicesProtocol, &mainThreadProcessorID);
        if (processorStatus != EFI_SUCCESS)
        {
            std::fprintf(stderr, "[SCAN] async file-I/O initialization failed\n");
            return 2;
        }
        registerAsynFileIO(mpServicesProtocol);

        if (!ts.init())
        {
            std::fprintf(stderr, "[SCAN] TickStorage initialization failed\n");
            return 2;
        }
        ts.beginEpoch(system.initialTick);
        rebuildTxHashmap = false;
        if (ts.tryLoadFromFile(system.epoch, snapshotDirectory) != 0)
        {
            std::fprintf(stderr, "[SCAN] TickStorage snapshot load failed\n");
            return 2;
        }
        if (ts.getPreloadTick() != system.tick)
        {
            std::fprintf(stderr, "[SCAN] snapshot tick mismatch: system=%u TickStorage=%u\n", system.tick, ts.getPreloadTick());
            return 2;
        }

        if (!gShadow.arm())
        {
            std::fprintf(stderr, "[SCAN] cannot create the temporary SwapVM shadow\n");
            return 2;
        }
        shadowArmed = true;

        Progress progress;
        ScanResult result = scanLoadedRange(ts, system.epoch, startTick, system.tick, progress);
        IssueSummary& issues = result.issues;

        unsigned long long logIds = 0, logBytes = 0;
        scanLoggingState(snapshotDirectory, startTick, issues, logIds, logBytes);

        deInitFileSystem();
        if (gShadowPoisoned.load(std::memory_order_acquire))
        {
            issues.add("swap-shadow", system.tick, -1, "temporary shadow I/O failed");
        }
        if (!discardShadow())
        {
            issues.add("swap-shadow", system.tick, -1, "temporary shadow cleanup failed");
        }
        cleanup();

        progress.update(tickCount, tickCount, result.transactionsChecked, issues.total, true);
        issues.print();
        std::fprintf(stdout, "[SCAN] logging: %llu logIds, %llu bytes\n", logIds, logBytes);

        const int exitCode = issues.total ? 1 : 0;
        std::fprintf(stdout, "[SCAN] %s: %llu ticks, %llu transactions, %llu issues\n", exitCode == 0 ? "clean" : "failed",
            tickCount, result.transactionsChecked, issues.total);
        return exitCode;
#endif
    }

    static int scan()
    {
        const int exitCode = runScan();
        std::fflush(stdout);
        std::fflush(stderr);
        // Skip exit(): the statically linked OpenSSL crashes in OPENSSL_cleanup once its
        // provider framework has dlopen'd a second libcrypto from ossl-modules.
        _exit(exitCode);
    }
}

#endif
