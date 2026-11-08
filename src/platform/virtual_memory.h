#pragma once

#include "platform/m256.h"
#include "platform/concurrency.h"
#include "platform/time.h"
#include "platform/memory_util.h"
#include "platform/memory_headroom.h"
#include "platform/debugging.h"
#include "platform/file_io.h"
#include "extensions/disk_shadow.h"

#include "four_q.h"
#include "kangaroo_twelve.h"
#include <lib/platform_common/sleep.h>

#include "extensions/zipper.h"
#include "extensions/swapvm_dirty_track.h" // gSwapDirtyTrackEnabled + SIGSEGV fast path + pool registry

#ifdef __linux__
// Runtime toggle for SwapVM page compression (blosc2); on by default.
inline bool gSwapCompressionEnabled = true;
#endif

template <class T>
inline constexpr const T& max(const T& left, const T& right)
{
    return (left < right) ? right : left;
}

template <class T>
inline constexpr const T& min(const T& left, const T& right)
{
    return (left < right) ? left : right;
}

// bounded retry: 100ms, 200ms, 400ms, 800ms, 1600ms (~3.1s max)
static constexpr int SWAPVM_IO_MAX_ATTEMPTS = 5;
static constexpr unsigned int SWAPVM_IO_INITIAL_DELAY_MS = 100;

// ============================================================================
//  Thread-local page pinning (keeps swap-cache pages resident across use)
// ============================================================================
//
// SwapVirtualMemory hands out raw pointers/references into a cache page (getRef/getPtr).
// Such a pointer would dangle if its page were evicted while still in use (by another
// op on the same VM or by another thread). To keep the upstream `T*`/`T&` API unchanged,
// we don't wrap the pointer; instead each access *pins* its cache slot in a thread-local
// arena, and the pins are released in bulk at well-defined work-unit boundaries:
//   - request processor: after each handled message
//   - tick processor / main / contract loops: each iteration
//   - long streaming loops (epoch transition, save, consistency): per iteration via PinScope
//   - lite HTTP handlers: per request via PinScope
// While pinned, a slot is never chosen for eviction. The invariant the boundaries rely on:
// no tick-storage pointer is used across the boundary that releases its pin (these pointers
// are function-local — vended and consumed within one unit). Under-sizing numCachePage shows
// up as the "all pages pinned" wait + counters, i.e. loud, not silent corruption.
struct ThreadPinArena
{
    static constexpr int CAP = 512; // >> total cache slots across all swap VMs
    void* vm[CAP];
    void (*unpin[CAP])(void*, int, unsigned long long);
    int slot[CAP];
    unsigned long long gen[CAP]; // VM generation when this pin was taken; stale across reset()
    int count = 0;
    unsigned long long drainSerial = 0; // bumped by releaseThreadPins(); a PinScope that saw a drain releases to 0

    // Already pinned by this thread in the current generation? (repeated access to the same page
    // is a no-op). Entries from an older generation never match — reset() bumped the generation,
    // so a stale note must not suppress a fresh pin.
    bool contains(void* v, int s, unsigned long long g) const
    {
        for (int i = 0; i < count; i++)
            if (vm[i] == v && slot[i] == s && gen[i] == g)
                return true;
        return false;
    }
    // Pins this thread holds on one VM in its current generation (diagnostics).
    int heldOn(const void* v, unsigned long long g) const
    {
        int n = 0;
        for (int i = 0; i < count; i++)
            if (vm[i] == v && gen[i] == g)
                n++;
        return n;
    }
    void add(void* v, void (*u)(void*, int, unsigned long long), int s, unsigned long long g)
    {
        if (count < CAP)
        {
            vm[count] = v; unpin[count] = u; slot[count] = s; gen[count] = g; count++;
            return;
        }
        // Full: the slot stays pinned for good (fail-stop via the all-pinned wait) rather than unpinned under a live pointer.
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            logToConsole(L"WARNING: swap pin arena full, pin leaked (thread spans resets without a pin boundary)");
        }
    }
    // Release pins added since `savepoint` (default: all of this thread's pins). The unpin thunk
    // checks the generation, so notes left stale by a reset() are dropped harmlessly.
    void releaseDownTo(int savepoint)
    {
        while (count > savepoint)
        {
            --count;
            unpin[count](vm[count], slot[count], gen[count]);
        }
    }
};
inline thread_local ThreadPinArena tlPinArena;

// Release every pin held by the current thread. Call at work-unit boundaries.
inline void releaseThreadPins()
{
    tlPinArena.releaseDownTo(0);
    tlPinArena.drainSerial++;
}

// RAII savepoint: releases only the pins taken during its lifetime. Use reset() to release
// per loop iteration without disturbing pins held by an enclosing scope.
struct PinScope
{
    int savepoint;
    unsigned long long drainSerial;
    PinScope() : savepoint(tlPinArena.count), drainSerial(tlPinArena.drainSerial) {}
    // A full drain inside the scope already invalidated its pointers; release everything taken since.
    void reset() { tlPinArena.releaseDownTo(tlPinArena.drainSerial == drainSerial ? savepoint : 0); }
    ~PinScope() { reset(); }
};

// an util to use disk as RAM to reduce hardware requirement for qubic core node
// this VirtualMemory doesn't (yet) support amend operation. That means data stay persisted once they are written
// template variables meaning:
// prefixName is used for generating page file names on disk, it must be unique if there are multiple VirtualMemory instances
// pageCapacity is number of items (T) inside a page
// it stores (numCachePage) pages on RAM for faster loading (the strategy mimics CPU cache lines)
// this class can be used to debug illegal memory access issue
template <typename T, unsigned long long prefixName, unsigned long long pageDirectory, unsigned long long pageCapacity = 100000, unsigned long long numCachePage = 128>
class VirtualMemory
{
protected:
    unsigned long long pageSize = sizeof(T) * pageCapacity;

    // on RAM
    T* currentPage = NULL; // current page is cache[0]
    T* cache[numCachePage + 1];
    CHAR16* pageDir = NULL;

    unsigned long long cachePageId[numCachePage + 1];
    unsigned long long lastAccessedTimestamp[numCachePage + 1]; // in millisecond
    unsigned long long currentId; // total items in this array, aka: latest item index + 1
    unsigned long long currentPageId; // current page index that's written on

    // Reader/writer spinlock: memLock is the writer byte, memReaders counts shared holders. Cache-hit
    // fast paths take shared (lockShared); every mutator takes exclusive (acquireMemLock).
    volatile char memLock;
    volatile long memReaders = 0;

    // Exclusive: claim the writer byte, then drain shared readers. The claim pumps IO while waiting
    // (a writer ahead may be parked in asyncLoad() under the lock — the documented deadlock). The
    // drain does NOT: shared holders are hit-path/unpin only, never do IO, so they always finish on
    // their own — a plain spin suffices.
    void acquireMemLock()
    {
        acquireLockWithIOPump(memLock);
        while (memReaders)
            _mm_pause();
    }

    // Shared, writer-preferring. The seq_cst inc + re-read of memLock is the Dekker handshake vs
    // acquireMemLock's claim-then-drain: a writer never runs while a reader is live.
    void lockShared()
    {
        for (;;)
        {
            while (memLock)
                _mm_pause();
            _InterlockedIncrement(&memReaders);
            if (!memLock)
                return;
            _InterlockedDecrement(&memReaders);
        }
    }
    void unlockShared()
    {
        _InterlockedDecrement(&memReaders);
    }

    void generatePageName(CHAR16 pageName[64], unsigned long long page_id)
    {
        setMem(pageName, sizeof(pageName), 0);
        char input[32];
        setMem(input, sizeof(input), 0);
        unsigned long long prefix_name = prefixName;
        copyMem(input, &prefix_name, 8);
        copyMem(input + 8, &page_id, 8);
        unsigned char digest[32];
        KangarooTwelve(input, 32, digest, 32);
        getIdentity(digest, pageName, true);
        setMem(pageName + 8, 8, 0); // too long file name will cause crash on some system
        appendText(pageName, L".pg");
    }

    void writeCurrentPageToDisk()
    {
        CHAR16 pageName[64];
        generatePageName(pageName, currentPageId);
#if defined(NO_UEFI) && !defined(REAL_NODE)
        auto sz = save(pageName, pageSize, (unsigned char*)currentPage, pageDir);
#else
        auto sz = asyncSave(pageName, pageSize, (unsigned char*)currentPage, pageDir, true);
#endif

#if !defined(NDEBUG)
        if (sz != pageSize)
        {
            addDebugMessage(L"Failed to store virtualMemory to disk. Old data maybe lost");
        }
        else
        {
            CHAR16 debugMsg[128];
            unsigned long long tmp = prefixName;
            debugMsg[0] = L'[';
            copyMem(debugMsg + 1, &tmp, 8);
            debugMsg[5] = L']';
            debugMsg[6] = L' ';
            debugMsg[7] = 0;
            appendText(debugMsg, L"page ");
            appendNumber(debugMsg, currentPageId, true);
            appendText(debugMsg, L" is written into disk");
            addDebugMessage(debugMsg);
        }
#endif
    }

    // Opt-in cache growth limit (setCacheGrowth): slots above cacheMinSlots are taken only while the
    // machine has at least cacheGrowMinFreeBytes of available memory. 0 = no limit (default).
    unsigned long long cacheMinSlots = numCachePage;
    unsigned long long cacheGrowMinFreeBytes = 0;

    // return the slot to fill: a free one if allowed, else the most outdated cache page
    int getMostOutdatedCachePage()
    {
        // i = 1 because 0 is used for current page
        int min_index = 0;
        int free_index = 0;
        for (int i = 1; i <= numCachePage; i++)
        {
            if (lastAccessedTimestamp[i] == 0)
            {
                if (!free_index)
                {
                    free_index = i;
                }
                continue;
            }
            if (!min_index || (lastAccessedTimestamp[i] < lastAccessedTimestamp[min_index]) || (lastAccessedTimestamp[i] == lastAccessedTimestamp[min_index] && cachePageId[i] < cachePageId[min_index]))
            {
                min_index = i;
            }
        }
        if (free_index && (!min_index || !cacheGrowMinFreeBytes || (unsigned long long)free_index <= cacheMinSlots))
        {
            return free_index;
        }
        if (free_index)
        {
            const long long availableBytes = availableMemoryBytes();
            if (availableBytes >= 0 && (unsigned long long)availableBytes >= cacheGrowMinFreeBytes)
            {
                return free_index;
            }
        }
        return min_index;
    }

    void copyCurrentPageToCache()
    {
        int cache_slot_idx = getMostOutdatedCachePage();
        copyMem(cache[cache_slot_idx], currentPage, pageSize);
        lastAccessedTimestamp[cache_slot_idx] = now_ms();
        cachePageId[cache_slot_idx] = currentPageId;
#ifndef NDEBUG
        {
            CHAR16 debugMsg[128];
            unsigned long long tmp = prefixName;
            debugMsg[0] = L'[';
            copyMem(debugMsg + 1, &tmp, 8);
            debugMsg[5] = L']';
            debugMsg[6] = L' ';
            debugMsg[7] = 0;
            appendText(debugMsg, L"page ");
            appendNumber(debugMsg, currentPageId, true);
            appendText(debugMsg, L" is moved to cache slot ");
            appendNumber(debugMsg, cache_slot_idx, true);
            addDebugMessage(debugMsg);
        }
#endif
    }

    void cleanCurrentPage()
    {
        setMem(currentPage, pageSize, 0);
    }

    // return cache id given cache_page_id
    int findCachePage(unsigned long long requested_page_id)
    {
        // TODO: consider implementing a tree here for faster search
        for (int i = 0; i <= numCachePage; i++)
        {
            if (cachePageId[i] == requested_page_id)
            {
                lastAccessedTimestamp[i] = now_ms();
                return i;
            }
        }
        return -1;
    }

    // load a page from disk to cache
    // if page is already on cache, return the id
    // return cache index
    int loadPageToCache(unsigned long long pageId)
    {
        int cache_page_id = findCachePage(pageId);

        if (cache_page_id != -1)
        {
            return cache_page_id;
        }
        CHAR16 pageName[64];
        generatePageName(pageName, pageId);
        cache_page_id = getMostOutdatedCachePage();
#if defined(NO_UEFI) && !defined(REAL_NODE)
        auto sz = load(pageName, pageSize, (unsigned char*)cache[cache_page_id], pageDir);
        lastAccessedTimestamp[cache_page_id] = now_ms();
#else
#if !defined(NDEBUG)
        {
            CHAR16 debugMsg[128];
            setText(debugMsg, L"Trying to load OLD page: ");
            appendNumber(debugMsg, pageId, true);
            addDebugMessage(debugMsg);
        }
#endif
        auto sz = asyncLoad(pageName, pageSize, (unsigned char*)cache[cache_page_id], pageDir);
        if (sz != pageSize)
        {
#if !defined(NDEBUG)
            addDebugMessage(L"Failed to load virtualMemory from disk");
#endif
            return -1;
        }
        lastAccessedTimestamp[cache_page_id] = now_ms();
#endif
        cachePageId[cache_page_id] = pageId;
#if !defined(NDEBUG)
        {
            CHAR16 debugMsg[128];
            unsigned long long tmp = prefixName;
            debugMsg[0] = L'[';
            copyMem(debugMsg + 1, &tmp, 8);
            debugMsg[5] = L']';
            debugMsg[6] = L' ';
            debugMsg[7] = 0;
            appendText(debugMsg, L"Load complete. Page ");
            appendNumber(debugMsg, pageId, true);
            appendText(debugMsg, L" is loaded into slot ");
            appendNumber(debugMsg, cache_page_id, true);
            addDebugMessage(debugMsg);
        }
#endif
        return cache_page_id;
    }

    // only call after append
    // check if current page is full
    // if yes, write current page to disk and cache
    // then clean current page
    void tryPersistingPage()
    {
        if (currentId % pageCapacity == 0)
        {
            writeCurrentPageToDisk();
            copyCurrentPageToCache();
            cleanCurrentPage();
            cachePageId[0] = currentId / pageCapacity;
            currentPageId++;
        }
    }

    void reset()
    {
        if (cacheGrowMinFreeBytes)
        {
            // Hand the pool back instead of writing it, so cache slots cost RAM only once used.
            zeroPool(currentPage, pageSize * (numCachePage + 1));
        }
        else
        {
            setMem(currentPage, pageSize * (numCachePage + 1), 0);
        }
        setMem(cachePageId, sizeof(cachePageId), 0xff);
        setMem(lastAccessedTimestamp, sizeof(lastAccessedTimestamp), 0);
        cachePageId[0] = 0;
        currentId = 0;
        currentPageId = 0;
        memLock = 0;
        memReaders = 0;
    }

public:
    VirtualMemory()
    {
        memLock = 0;
    }

    // Call before init(). Keeps up to minSlots cache pages unconditionally and takes more only while
    // the machine has growMinFreeBytes of available memory; unused slots cost no RAM.
    void setCacheGrowth(unsigned long long minSlots, unsigned long long growMinFreeBytes)
    {
        cacheMinSlots = minSlots;
        cacheGrowMinFreeBytes = growMinFreeBytes;
    }

    // With setCacheGrowth: give the RAM of one cache slot above minSlots (the least recently used)
    // back to the system, e.g. when the machine runs low on memory. One slot per call: freeing them
    // all at once could jump past the growth threshold and let the cache refill right away.
    // Non-blocking: does nothing if a writer holds the lock (it may be loading a page from disk).
    // Returns the number of slots released (0 or 1).
    unsigned int tryReleaseCacheAboveMin()
    {
        if (!cacheGrowMinFreeBytes || currentPage == NULL)
        {
            return 0;
        }
        if (!TRY_ACQUIRE(memLock))
        {
            return 0;
        }
        while (memReaders)
            _mm_pause();
        unsigned long long stalest = 0;
        for (unsigned long long i = cacheMinSlots + 1; i <= numCachePage; i++)
        {
            if (lastAccessedTimestamp[i] != 0
                && (!stalest || lastAccessedTimestamp[i] < lastAccessedTimestamp[stalest]))
            {
                stalest = i;
            }
        }
        if (stalest)
        {
            zeroPool(cache[stalest], pageSize);
            lastAccessedTimestamp[stalest] = 0;
            cachePageId[stalest] = 0xffffffffffffffffULL;
        }
        RELEASE(memLock);
        return stalest ? 1 : 0;
    }

    bool init()
    {
        acquireMemLock();
        if (currentPage == NULL)
        {
            if (!allocPoolWithErrorLog(L"VirtualMemory.Page", pageSize * (numCachePage + 1), (void**)&currentPage, __LINE__))
            {
                return false;
            }
            cache[0] = currentPage;
            for (int i = 1; i <= numCachePage; i++)
            {
                char *tmp = (char*)cache[0];
                cache[i] = (T*)(tmp + i * pageSize);
            }
        }

        if (pageDir == NULL)
        {
            if (prefixName != 0 && pageDirectory != 0)
            {
                if (!allocPoolWithErrorLog(L"PageDir", 32, (void**)&pageDir, __LINE__))
                {
                    return false;
                }
                setMem(pageDir, 32, 0);
                unsigned long long tmp = prefixName;
                copyMem(pageDir, &tmp, 8);
                tmp = pageDirectory;
                copyMem(pageDir + 4, &tmp, 8);
                appendText(pageDir, L".");
            }
        }

        if (pageDir != NULL)
        {
#ifdef REAL_NODE
            addEpochToFileName(pageDir, 12, max(EPOCH, int(system.epoch)));
#else
			addEpochToFileName(pageDir, 12, 0);
#endif
            if (asyncCreateDir(pageDir) != 0)
            {
#if !defined(NDEBUG)
                CHAR16 dbg[128];
                setText(dbg, L"WARNING: Failed to create dir ");
                appendText(dbg, pageDir);
                addDebugMessage(dbg);
#endif
            }
#ifdef __linux__
            gShadow.registerDir(pageDir);
#endif
        }

        reset();
        RELEASE(memLock);
        return true;
    }
    void deinit()
    {
        if (currentPage != NULL)
        {
            freePool(currentPage);
            currentPage = NULL;
        }
        if (pageDir != NULL)
        {
            freePool(pageDir);
            pageDir = NULL;
        }
    }

    // getMany: 
    // this operation copies numItems * sizeof(T) bytes from [src+offset] to [dst] - note that src and dst are T* not char*
    // offset + numItems must be less than currentId
    // return number of items has been copied
    unsigned long long getMany(T* dst, unsigned long long offset, unsigned long long numItems)
    {
        acquireMemLock();
        ASSERT(offset + numItems - 1 < currentId);
        if (offset + numItems - 1 >= currentId)
        {
            RELEASE(memLock);
            return 0;
        }
        RELEASE(memLock);

        // processing
        unsigned long long c_bytes = 0;
        unsigned long long p_start = offset;
        unsigned long long p_end = offset + numItems;

        // visualizer:
        // [     PAGE N    ] [ PAGE N + 1] [ PAGE N + 2] [ PAGE N + 3] ... [ PAGE N + K-2 ] [ PAGE N + K-1 ] [ PAGE N + K ]
        //        ^[                        REQUESTED MEMORY REGION                               ]^
        //         [HEAD   ] [                            BODY                            ] [TAIL ]

        // HEAD
        unsigned long long hs = offset; // head start
        unsigned long long rhs = (hs / pageCapacity) * pageCapacity; // rounded hs
        unsigned long long r_page_id = rhs / pageCapacity;
        unsigned long long he = min(rhs + pageCapacity, offset + numItems); // head end
        unsigned long long n_item = he - hs; // copy [hs, he)
        acquireMemLock();
        int cache_page_idx = loadPageToCache(r_page_id);
        if (cache_page_idx == -1)
        {
#if !defined(NDEBUG)
            addDebugMessage(L"Invalid cache page index, return zeroes array");
#endif
            setMem(dst, numItems * sizeof(T), 0);
            RELEASE(memLock);
            return 0;
        }
        copyMem(dst, cache[cache_page_idx] + (hs % pageCapacity), n_item * sizeof(T));
        dst += n_item;
        c_bytes += n_item * sizeof(T);
        RELEASE(memLock);
        // BODY
        unsigned long long bs = he; //bodystart
        if (he < p_end && (p_end - he) >= pageCapacity) // need to copy fullpage in the "middle"
        {
            for (bs = he; bs + pageCapacity <= p_end; bs += pageCapacity)
            {
                r_page_id = bs / pageCapacity;
                acquireMemLock();
                cache_page_idx = loadPageToCache(r_page_id);
                if (cache_page_idx == -1)
                {
#if !defined(NDEBUG)
                    addDebugMessage(L"Invalid cache page index, return zeroes array");
#endif
                    // zero only remaining span; dst already advanced
                    setMem(dst, (p_end - bs) * sizeof(T), 0);
                    RELEASE(memLock);
                    return 0;
                }
                copyMem(dst, cache[cache_page_idx], pageSize);
                dst += pageCapacity;
                c_bytes += pageSize;
                RELEASE(memLock);
            }
        }
        // TAIL
        if (bs < p_end)
        {
            r_page_id = bs / pageCapacity;
            n_item = p_end - bs; // copy [bs, p_end)
            acquireMemLock();
            int cache_page_idx = loadPageToCache(r_page_id);
            if (cache_page_idx == -1)
            {
#if !defined(NDEBUG)
                addDebugMessage(L"Invalid cache page index, return zeroes array");
#endif
                // zero only remaining span; dst already advanced
                setMem(dst, (p_end - bs) * sizeof(T), 0);
                RELEASE(memLock);
                return 0;
            }
            copyMem(dst, cache[cache_page_idx], n_item * sizeof(T));
            dst += n_item;
            c_bytes += n_item * sizeof(T);
            RELEASE(memLock);
        }
        return c_bytes;
    }

    // appendMany: 
    // this operation append/copies multiple (T) to the memory pages
    // return number of items has been copied
    unsigned long long appendMany(T* src, unsigned long long numItems)
    {
        acquireMemLock();
        unsigned long long c_bytes = 0;
        unsigned long long p_start = currentId;
        unsigned long long p_end = currentId + numItems;

        // HEAD
        unsigned long long hs = currentId; // head start
        unsigned long long rhs = (hs / pageCapacity) * pageCapacity; // rounded hs
        unsigned long long r_page_id = rhs / pageCapacity;
        unsigned long long he = min(rhs + pageCapacity, p_end); // head end
        unsigned long long n_item = he - hs; // copy [hs, he)
        copyMem(currentPage + (currentId % pageCapacity), src, n_item * sizeof(T));
        currentId += n_item;
        src += n_item;
        c_bytes += n_item * sizeof(T);
        tryPersistingPage();
        // BODY
        unsigned long long bs = he; //bodystart
        if (currentId < p_end && (p_end - currentId) >= pageCapacity)
        {
            for (bs = he; bs + pageCapacity <= p_end; bs += pageCapacity)
            {
                copyMem(currentPage, src, pageSize);
                currentId += pageCapacity;
                src += pageCapacity;
                c_bytes += pageSize;
                tryPersistingPage();
            }
        }
        // TAIL
        if (bs < p_end)
        {
            n_item = p_end - bs; // copy [bs, p_end)
            copyMem(currentPage, src, n_item * sizeof(T));
            currentId += n_item;
            src += n_item;
            c_bytes += n_item * sizeof(T);
            tryPersistingPage();
        }
        RELEASE(memLock);
        return c_bytes;
    }

    // return array[index]
    // if index is not in current page it will try to find it in cache
    // if index is not in cache it will load the page to most outdated cache
    T get(unsigned long long index)
    {
        T result;
        acquireMemLock();
        if (index >= currentId) // out of bound
        {
            setMem(&result, sizeof(T), 0);
            RELEASE(memLock);
            return result;
        }
        unsigned long long requested_page_id = index / pageCapacity;
        int cache_page_idx = loadPageToCache(requested_page_id);
        if (cache_page_idx == -1)
        {
#if !defined(NDEBUG)
            addDebugMessage(L"Invalid cache page index, return zeroes array");
#endif
            setMem(&result, sizeof(T), 0);
            RELEASE(memLock);
            return result;
        }
        result = cache[cache_page_idx][index % pageCapacity];
        RELEASE(memLock);
        return result;
    }

    // return array[index]
    // if index is not in current page it will try to find it in cache
    // if index is not in cache it will load the page to most outdated cache
    void getOne(unsigned long long index, T* result)
    {
        acquireMemLock();
        if (index >= currentId) // out of bound
        {
            setMem(result, sizeof(T), 0);
            RELEASE(memLock);
            return;
        }
        unsigned long long requested_page_id = index / pageCapacity;
        int cache_page_idx = loadPageToCache(requested_page_id);
        if (cache_page_idx == -1)
        {
#if !defined(NDEBUG)
            addDebugMessage(L"Invalid cache page index, return zeroes array");
#endif
            setMem(result, sizeof(T), 0);
            RELEASE(memLock);
            return;
        }
        copyMem(result, &(cache[cache_page_idx][index % pageCapacity]), sizeof(T));
        RELEASE(memLock);
        return;
    }

    T operator[](unsigned long long index)
    {
        return get(index);
    }

    // append (single) data to latest
    // if current page is fully written it will:
    // (1) write current page to disk
    // (2) copy current page to cache
    // (3) clean current page for new data
    void append(const T& data)
    {
        ASSERT(currentPage != NULL);
        acquireMemLock();
        copyMem(&currentPage[currentId % pageCapacity], &data, sizeof(T));
        currentId++;
        tryPersistingPage();
        RELEASE(memLock);
    }

    unsigned long long size()
    {
        return currentId;
    }

    // delete a page on disk given pageId
    bool prune(unsigned long long pageId)
    {
        if (pageId > currentPageId)
        {
            return false;
        }
        CHAR16 pageName[64];
        generatePageName(pageName, pageId);
        acquireMemLock();
        bool success = (asyncRemoveFile(pageName, pageDir)) == 0;
        RELEASE(memLock);
        return success;
    }

    // delete pages data on disk given (fromId, toId)
    // Since we store the whole page on disk, pageId will be rounded up for fromId and rounded down for toId
    // fromPageId = (fromId + pageCapacity - 1) // pageCapacity
    // toPageId = (toId   - pageCapacity + 1) // pageCapacity
    // eg: pageCapacity is 50'000. To delete the second page, call prune(50000, 99999)
    bool pruneRange(long long fromId, long long toId)
    {
        long long fromPageId = (fromId + pageCapacity - 1) / pageCapacity;
        long long toPageId = (toId - pageCapacity + 1) / pageCapacity;
        if (fromPageId < 0 || toPageId < 0)
        {
            return false;
        }
        if (fromPageId > toPageId)
        {
            return false;
        }
        if (!isPageIdValid(fromPageId))
        {
            return false;
        }
        if (!isPageIdValid(toPageId))
        {
            return false;
        }
        bool success = true;
        for (long long i = fromPageId; i <= toPageId; i++)
        {
            bool ret = prune(i);
            success &= ret;
            if (!ret) return success;
        }
        return success;
    }

    // checking if an index is in valid range of this array
    bool isIndexValid(unsigned long long index)
    {
        return index < currentId;
    }

    // checking if a page id is in valid range of this array
    bool isPageIdValid(unsigned long long index)
    {
        return index <= currentPageId;
    }

    unsigned long long pageCap()
    {
        return pageCapacity;
    }

    unsigned long long getPageSize()
    {
        return pageCapacity * sizeof(T);
    }

    unsigned long long getVmStateSize()
    {
        return numCachePage * getPageSize();
    }

    const T* getCurrentPagePtr()
    {
        return currentPage;
    }

    unsigned long long dumpVMState(unsigned char* buffer)
    {
        acquireMemLock();
        unsigned long long ret = 0;
        copyMem(buffer, currentPage, pageSize);
        ret += pageSize;
        buffer += pageSize;

        *((unsigned long long*)buffer) = currentId;
        buffer += 8;
        ret += 8;

        *((unsigned long long*)buffer) = currentPageId;
        buffer += 8;
        ret += 8;
        RELEASE(memLock);
        return ret;
    }

    unsigned long long loadVMState(unsigned char* buffer)
    {
        acquireMemLock();
        unsigned long long ret = 0;
        copyMem(currentPage, buffer, pageSize);
        ret += pageSize;
        buffer += pageSize;

        currentId = *((unsigned long long*)buffer);
        buffer += 8;
        ret += 8;

        currentPageId = *((unsigned long long*)buffer);
        buffer += 8;
        ret += 8;

        cachePageId[0] = currentPageId;
        lastAccessedTimestamp[0] = now_ms();
        RELEASE(memLock);
        return ret;
    }
};

enum SwapMode
{
    INDEX_MODE = 0, // for stride access pattern (ideally for TickData, Ticks)
    OFFSET_MODE = 1 // for random access pattern using offset (ideally for Transaction)
};

// SwapVirtualMemory don't use append operations, it acts like a continuous chunk of memory that can be read and written randomly
// it will try to persist pages that are not written to disk when loading a page to cache (when there is no empty cache slot)
// NOTE: pages in cache may not be written to disk yet
// NOTE: DO NOT CREATE INSTANCE IN FUNCTION STACK, IT WILL CAUSE STACK OVERFLOW
template <typename T, unsigned long long prefixName, unsigned long long pageDirectory, unsigned long long pageCapacity = 100000, unsigned long long numCachePage = 128, SwapMode mode = INDEX_MODE, long long extraBytesPerElement = 0>
class SwapVirtualMemory : private VirtualMemory<T, prefixName, pageDirectory, pageCapacity, numCachePage>
{
    using VMBase = VirtualMemory<T, prefixName, pageDirectory, pageCapacity, numCachePage>;
    using VMBase::currentPage;
    using VMBase::currentPageId;
    using VMBase::pageSize;
    using VMBase::pageDir;
    using VMBase::cache;
    using VMBase::cachePageId;
    using VMBase::lastAccessedTimestamp;
    using VMBase::memLock;
    using VMBase::memReaders;
    using VMBase::acquireMemLock;
    using VMBase::lockShared;
    using VMBase::unlockShared;
    using VMBase::generatePageName;
    using VMBase::findCachePage;
    using VMBase::loadPageToCache;
    using VMBase::getMostOutdatedCachePage;

#if defined(TESTNET) && defined(TESTNET_LITE_RAM)
    static constexpr unsigned long long MAX_PAGE = 128 * 1024; // 128K pages — LITE testnet
#else
    static constexpr unsigned long long MAX_PAGE = 1024 * 1024; // max 1 million pages, can be adjusted later
#endif

private:
    bool* isPageWrittenToDisk; // if current page is written to disk
    static constexpr unsigned long long INVALID_PAGE_ID = -1;
    static constexpr unsigned long long isPageWrittenToDiskSize = sizeof(bool) * MAX_PAGE;

    // Threads referencing the page in this slot (via the pin arena); pinCount>0 is never evicted.
    // Atomic inc/dec: pin/unpin run concurrently under the shared lock.
    volatile long pinCount[numCachePage + 1] = {};

    // Monitoring. pinnedNow is atomic (concurrent pin/unpin); the rest best-effort (lock-free reads).
    volatile long pinnedNow = 0;            // slots currently pinned
    volatile long pinnedHighWater = 0;      // max slots pinned at once since start
    unsigned long long allPinnedWaits = 0;  // times eviction had to wait for a pin (should be 0)
    unsigned long long cacheHits = 0;       // page accesses served from cache
    unsigned long long cacheMisses = 0;     // page accesses that loaded from disk

    // Bumped by reset() (exclusive); pins from an older generation are stale (slot was reused), so
    // unpin and the dedup check ignore them.
    unsigned long long generation = 0;

    // In-flight load coordination: a miss does its disk IO with memLock RELEASED so cache hits don't
    // stall, reserving the victim by stamping cachePageId=LOADING_PAGE_ID (find/eviction skip it).
    // loadingTarget[] = page coming in (dedup duplicate loads); evictingPage[] = page being written
    // back (a misser of it waits, vs reading stale disk). Both transient: INVALID on init/reset.
    static constexpr unsigned long long LOADING_PAGE_ID = (unsigned long long)-2;
    unsigned long long loadingTarget[numCachePage + 1];
    unsigned long long evictingPage[numCachePage + 1];

    // Per-slot dirty byte (mprotect auto-dirty). Set by the SIGSEGV fast path on the first write after
    // a slot is armed RO; read at eviction to skip the writeback for clean slots. Only meaningful when
    // gSwapDirtyTrackEnabled (else every occupied victim is treated as dirty).
    volatile unsigned char dirtyFlags[numCachePage + 1] = {};
    unsigned long long cleanEvicts = 0; // victims evicted with no writeback (clean) -- the saving
    unsigned long long dirtyEvicts = 0; // victims that needed a writeback

    // Bytes between cache slots when the pool is page-aligned for dirty tracking (== pageSize; slots
    // stay contiguous). 0 => not page-aligned (non-Linux, or pageSize not a whole number of OS pages)
    // => dirty tracking is off for this VM and eviction writes back every occupied victim.
    unsigned long long cacheSlotStride = 0;

    // Pin a slot for this thread (under shared or exclusive). Idempotent per thread (arena dedup).
    void pinSlotForThread(int slot)
    {
        const unsigned long long g = generation;
        if (tlPinArena.contains(this, slot, g))
            return;
        if (_InterlockedIncrement(&pinCount[slot]) == 1)
        {
            const long now = _InterlockedIncrement(&pinnedNow);
            if (now > pinnedHighWater)
                pinnedHighWater = now;
        }
        tlPinArena.add(this, &SwapVirtualMemory::unpinThunk, slot, g);
    }
    void unpinSlotInternal(int slot, unsigned long long gen)
    {
        // Shared: concurrent with other unpins/pins (pinCount atomic), exclusive of reset/eviction —
        // which is why the generation read below can be plain.
        lockShared();
        // gen mismatch => pin predates a reset() that cleared the slot; drop it, don't hit a fresh pin.
        if (gen == generation && slot >= 0 && slot <= (int)numCachePage && pinCount[slot] > 0)
        {
            if (_InterlockedDecrement(&pinCount[slot]) == 0)
                _InterlockedDecrement(&pinnedNow);
        }
        unlockShared();
    }
    static void unpinThunk(void* self, int slot, unsigned long long gen)
    {
        ((SwapVirtualMemory*)self)->unpinSlotInternal(slot, gen);
    }

    // used for offset mode
    T* pageExtraBytesBuffer;
    bool* pageHasExtraBytes; // if this page has extra bytes
    unsigned long long* lastestPageExtraBytesOffsetAccessed; // used to track the lastest offset accessed for each page
    static constexpr unsigned long long maxBytesPerElement = sizeof(T) + extraBytesPerElement;
    static constexpr unsigned long long maxBytesPerPage = maxBytesPerElement * pageCapacity;

    static constexpr unsigned long long pageExtraBytesBufferSize = (maxBytesPerElement) * MAX_PAGE;
    static constexpr unsigned long long pageHasExtraBytesBufferSize = sizeof(bool) * MAX_PAGE;
    static constexpr unsigned long long lastestPageExtraBytesOffsetAccessedBufferSize = sizeof(unsigned long long) * MAX_PAGE;

    void writePageToDisk(unsigned long long pageId, int cache_idx)
    {
        CHAR16 pageName[64];
        generatePageName(pageName, pageId);
        unsigned char *pageBuffer = (unsigned char*)cache[cache_idx];

        // bounded retry on save() failure
        unsigned long long sz = 0;
        unsigned long long expectedSize = pageSize;
        unsigned char* saveBuffer = (unsigned char*)pageBuffer;
#ifdef __linux__
        std::vector<unsigned char> compressed;
        if (gSwapCompressionEnabled)
        {
            compressed = Zipper::zip(pageBuffer, pageSize, 4);
            saveBuffer = compressed.data();
            expectedSize = (unsigned long long)compressed.size();
        }
#endif
        unsigned int delayMs = SWAPVM_IO_INITIAL_DELAY_MS;
        for (int attempt = 0; attempt < SWAPVM_IO_MAX_ATTEMPTS; attempt++)
        {
            sz = save(pageName, expectedSize, saveBuffer, pageDir);
            if (sz == expectedSize)
                break;

            setText(message, L"swapVM writePageToDisk failed (attempt ");
            appendNumber(message, (unsigned long long)(attempt + 1), false);
            appendText(message, L"/");
            appendNumber(message, (unsigned long long)SWAPVM_IO_MAX_ATTEMPTS, false);
            appendText(message, L") page ");
            appendNumber(message, pageId, true);
            logToConsole(message);

            if (attempt + 1 < SWAPVM_IO_MAX_ATTEMPTS)
            {
                sleepMilliseconds(delayMs);
                delayMs *= 2;
            }
        }

        if (sz != expectedSize)
        {
            setText(message, L"Fatal: swapVM writePageToDisk exhausted retries | page ");
            appendNumber(message, pageId, true);
            appendText(message, L" | prefix ");
            appendText(message, pageDir);
            logToConsole(message);
            exit(1);
        }

#if !defined(NDEBUG)
        {
            CHAR16 debugMsg[128];
            unsigned long long tmp = prefixName;
            debugMsg[0] = L'[';
            copyMem(debugMsg + 1, &tmp, 8);
            debugMsg[5] = L']';
            debugMsg[6] = L' ';
            debugMsg[7] = 0;
            appendText(debugMsg, L"page ");
            appendNumber(debugMsg, currentPageId, true);
            appendText(debugMsg, L" is written into disk");
            addDebugMessage(debugMsg);
        }
#endif

        // only mark written on confirmed success
        isPageWrittenToDisk[pageId] = true;
    }

    // Slot currently loading pageId (cachePageId stamped LOADING_PAGE_ID), else -1. Same-page dedup.
    int findInflightLoad(unsigned long long pageId)
    {
        for (int i = 0; i <= numCachePage; i++)
            if (cachePageId[i] == LOADING_PAGE_ID && loadingTarget[i] == pageId)
                return i;
        return -1;
    }
    // Slot currently writing pageId back to disk, else -1. A loader of pageId must wait for this
    // (its in-RAM copy is the authoritative one until the writeback lands) instead of reading stale disk.
    int findInflightWriteback(unsigned long long pageId)
    {
        for (int i = 0; i <= numCachePage; i++)
            if (evictingPage[i] == pageId)
                return i;
        return -1;
    }
    // Any slot mid-IO (reserved LOADING while its disk IO runs unlocked).
    bool anyInflightIO()
    {
        for (int i = 0; i <= numCachePage; i++)
            if (cachePageId[i] == LOADING_PAGE_ID)
                return true;
        return false;
    }

    // Component B mprotect machinery. All slot-protection is a no-op unless gSwapDirtyTrackEnabled.
#if defined(__linux__)
    // Page-align the cache pool so each slot is its own mprotect region, slots still contiguous at
    // pageSize stride. Only when pageSize is a whole number of OS pages; else leave it to VMBase::init
    // (dirty tracking off, cacheSlotStride==0). Pre-setting currentPage makes VMBase::init skip its alloc.
    bool allocPageAlignedCachePool()
    {
        if (currentPage != NULL)
            return true;
        const unsigned long long ps = (unsigned long long)sysconf(_SC_PAGESIZE);
        if (pageSize % ps != 0)
            return true; // not page-strided -> per-slot mprotect can't isolate slots; leave to base alloc
        cacheSlotStride = pageSize;
        const unsigned long long poolBytes = pageSize * (numCachePage + 1);
        void* aligned = aligned_alloc((size_t)ps, (size_t)poolBytes);
        if (aligned == NULL)
            return false;
        setMem(aligned, poolBytes, 0);
        currentPage = (T*)aligned;
        for (int i = 0; i <= numCachePage; i++)
            cache[i] = (T*)((char*)aligned + (unsigned long long)i * pageSize);
        return true;
    }
    void registerDirtyTrackPool()
    {
        if (cacheSlotStride) // only register pools we actually page-aligned
            SwapDirtyTrack::registerPool((unsigned char**)&currentPage, cacheSlotStride, numCachePage + 1, dirtyFlags);
    }
    void mprotectSlot(int slot, int prot)
    {
        if (gSwapDirtyTrackEnabled && cacheSlotStride)
            mprotect((void*)cache[slot], cacheSlotStride, prot);
    }
#endif
    void unprotectSlotForLoad(int slot) // make the slot writable so an fread can land in it
    {
#if defined(__linux__)
        mprotectSlot(slot, PROT_READ | PROT_WRITE);
#endif
    }
    void armSlotAfterLoad(int slot) // mark clean + write-protect: the next write to it faults -> dirty
    {
#if defined(__linux__)
        if (gSwapDirtyTrackEnabled && cacheSlotStride)
        {
            dirtyFlags[slot] = 0;
            mprotectSlot(slot, PROT_READ);
        }
#endif
    }
    void unprotectWholePool() // drop write-protection on the whole pool + clear dirty (before reset's bulk setMem)
    {
#if defined(__linux__)
        if (gSwapDirtyTrackEnabled && cacheSlotStride && currentPage != NULL)
            mprotect((void*)cache[0], cacheSlotStride * (numCachePage + 1), PROT_READ | PROT_WRITE);
#endif
        setMem((void*)dirtyFlags, sizeof(dirtyFlags), 0);
    }

    // Read pageId's bytes into cache[slot] (decompress if enabled), or zero-fill if never written.
    // Returns false on exhausted IO retries. Runs with memLock RELEASED (slot is reserved LOADING).
    bool loadPageBytesIntoSlot(unsigned long long pageId, int slot)
    {
        CHAR16 pageName[64];
        generatePageName(pageName, pageId);
        unsigned long long sz = 0;
        if (isPageWrittenToDisk[pageId])
        {
            unsigned int delayMs = SWAPVM_IO_INITIAL_DELAY_MS;
            for (int attempt = 0; attempt < SWAPVM_IO_MAX_ATTEMPTS; attempt++)
            {
#ifdef __linux__
                if (gSwapCompressionEnabled)
                {
                    sz = 0;
                    long long compressedSize = getFileSize((CHAR16*)pageName, pageDir);
                    if (compressedSize > 0)
                    {
                        std::vector<unsigned char> tmp((size_t)compressedSize);
                        long long readBytes = load(pageName, (unsigned long long)compressedSize, tmp.data(), pageDir);
                        if (readBytes == compressedSize)
                        {
                            std::vector<unsigned char> decompressed = Zipper::unzip(tmp.data(), (size_t)compressedSize, 4);
                            if (decompressed.size() == pageSize)
                            {
                                copyMem(cache[slot], decompressed.data(), pageSize);
                                sz = pageSize;
                            }
                        }
                    }
                }
                else
#endif
                {
                    sz = load(pageName, pageSize, (unsigned char*)cache[slot], pageDir);
                }
                if (sz == pageSize)
                    break;

                // Loud exact cause (missing vs size-mismatch + full path) so a torn/missing .pg is hand-recoverable.
                long long onDiskSize = getFileSize((CHAR16*)pageName, pageDir);
                const CHAR16* diagnosticDir = liteShadowReadDir(pageDir, pageName);
                bool compOn = false;
#ifdef __linux__
                compOn = gSwapCompressionEnabled;
#endif
                setText(message, L"swapVM load FAILED page ");
                appendNumber(message, pageId, true);
                appendText(message, L" | file ");
                appendText(message, diagnosticDir);
                appendText(message, L"/");
                appendText(message, (CHAR16*)pageName);
                appendText(message, L" | wantPageBytes ");
                appendNumber(message, (unsigned long long)pageSize, false);
                appendText(message, L" | onDisk ");
                if (onDiskSize < 0)
                {
                    appendText(message, L"MISSING(not found)");
                }
                else
                {
                    appendNumber(message, (unsigned long long)onDiskSize, false);
                    if (!compOn && (unsigned long long)onDiskSize != pageSize)
                        appendText(message, L" SIZE_MISMATCH");
                }
                appendText(message, L" | readReturned ");
                appendNumber(message, (unsigned long long)sz, false);
                appendText(message, L" | compression ");
                appendText(message, compOn ? L"on" : L"off");
                appendText(message, L" | attempt ");
                appendNumber(message, (unsigned long long)(attempt + 1), false);
                appendText(message, L"/");
                appendNumber(message, (unsigned long long)SWAPVM_IO_MAX_ATTEMPTS, false);
                logToConsole(message);

                if (attempt + 1 < SWAPVM_IO_MAX_ATTEMPTS)
                {
                    sleepMilliseconds(delayMs);
                    delayMs *= 2;
                }
            }
        }
        else
        {
            sz = pageSize;
            setMem(cache[slot], pageSize, 0);
        }
        if (sz != pageSize)
        {
            setText(message, L"swapVM load GAVE UP (node will fatal) page ");
            appendNumber(message, pageId, true);
            appendText(message, L" file ");
            appendText(message, pageDir);
            appendText(message, L"/");
            appendText(message, (CHAR16*)pageName);
            logToConsole(message);
        }
        return sz == pageSize;
    }

    // Self-deadlock (this thread holds the pins) and cross-thread exhaustion need different fixes; name which.
    void appendPinDiag(CHAR16* msg)
    {
        appendText(msg, L" | pinned ");
        appendNumber(msg, (unsigned long long)pinnedNow, false);
        appendText(msg, L"/");
        appendNumber(msg, (unsigned long long)(numCachePage + 1), false);
        appendText(msg, L", this thread ");
        appendNumber(msg, (unsigned long long)getPinsHeldByThisThread(), false);
    }

    // Load pageId into a cache slot (returns its index). Entered/returned holding memLock; the evict
    // writeback + load run with it RELEASED (victim reserved via LOADING_PAGE_ID, skipped by
    // find/eviction) so concurrent cache hits don't stall behind this miss's ~16MB disk IO.
    int loadPageToCacheAndTryToPersist(unsigned long long pageId)
    {
        for (;;)
        {
            int cache_page_id = findCachePage(pageId);
            if (cache_page_id != -1)
            {
                cacheHits++;
                return cache_page_id;
            }

            // Another thread is already loading pageId, or writing it back: wait for it rather than
            // start a duplicate load / read a stale on-disk copy. Bounded by one IO, per-page.
            if (findInflightLoad(pageId) != -1 || findInflightWriteback(pageId) != -1)
            {
                RELEASE(memLock);
                sleepMilliseconds(1);
                acquireMemLock();
                continue;
            }

            cacheMisses++;

            int victim = getMostOutdatedCachePageExceptCurrentPage();
            if (victim == -1)
            {
                allPinnedWaits++;
                // Surface pin-exhaustion immediately (not only at the 5s fatal) so an operator can
                // tell "stuck on swap-VM pins" apart from normal slow processing.
                CHAR16 pinMsg[256];
                setText(pinMsg, L"WARNING: swapVM all cache pages pinned, waiting for a free slot (pin leak or numCachePage too small) | prefix ");
                appendText(pinMsg, pageDir);
                appendPinDiag(pinMsg);
                logToConsole(pinMsg);
            }
            int allPinnedWaitMs = 0;
            bool inflightAppeared = false;
            while (victim == -1)
            {
                RELEASE(memLock);
                sleepMilliseconds(1);
                acquireMemLock();
                int rechecked = findCachePage(pageId);
                if (rechecked != -1)
                {
                    cacheHits++;
                    return rechecked;
                }
                if (findInflightLoad(pageId) != -1 || findInflightWriteback(pageId) != -1)
                {
                    inflightAppeared = true; // another loader took pageId; fall back to the dedup wait
                    break;
                }
                victim = getMostOutdatedCachePageExceptCurrentPage();
                ++allPinnedWaitMs;
                if (allPinnedWaitMs % 1000 == 0 && allPinnedWaitMs < 5000) // heartbeat once per second while waiting
                {
                    CHAR16 pinMsg[160];
                    setText(pinMsg, L"swapVM still waiting for a free cache page (");
                    appendNumber(pinMsg, (unsigned long long)(allPinnedWaitMs / 1000), FALSE);
                    appendText(pinMsg, L"s) | prefix ");
                    appendText(pinMsg, pageDir);
                    logToConsole(pinMsg);
                }
                if (allPinnedWaitMs > 5000) // ~5s
                {
                    setText(message, L"Fatal: swapVM all cache pages pinned (numCachePage too small) | prefix ");
                    appendText(message, pageDir);
                    appendPinDiag(message);
                    logToConsole(message);
                    exit(1);
                }
            }
            if (inflightAppeared)
                continue;

            // Reserve the victim under the lock; do the disk IO with the lock RELEASED.
            const unsigned long long oldPage = cachePageId[victim];
            // Component B: skip the writeback for a slot never modified since load. Tracking off, non-Linux,
            // or a pool whose page is not OS-page strided (never armed) => write back every occupied victim (Component A).
            const bool slotDirty = !(gSwapDirtyTrackEnabled && cacheSlotStride) || dirtyFlags[victim];
            const bool needWriteback = (oldPage != INVALID_PAGE_ID) && slotDirty;
            if (oldPage != INVALID_PAGE_ID)
                (needWriteback ? dirtyEvicts : cleanEvicts)++;
            cachePageId[victim] = LOADING_PAGE_ID;                    // hide from find/eviction
            loadingTarget[victim] = pageId;
            evictingPage[victim] = needWriteback ? oldPage : INVALID_PAGE_ID;
            const unsigned long long savedGeneration = generation;

            RELEASE(memLock);
            if (needWriteback)
                writePageToDisk(oldPage, victim);
            unprotectSlotForLoad(victim);                            // RW so the fread can land
            const bool loadOk = loadPageBytesIntoSlot(pageId, victim);
            armSlotAfterLoad(victim);                                // clean + RO: next write faults
            acquireMemLock();

            // reset() during our IO bumped the generation and cleared the slot + in-flight state:
            // our reservation is void, the victim may already belong to someone else -> retry fresh,
            // touching nothing.
            if (generation != savedGeneration)
                continue;

            loadingTarget[victim] = INVALID_PAGE_ID;
            evictingPage[victim] = INVALID_PAGE_ID;
            if (!loadOk)
            {
                cachePageId[victim] = INVALID_PAGE_ID;
                return -1;
            }
            cachePageId[victim] = pageId;                            // publish: now findable
            lastAccessedTimestamp[victim] = now_ms();
            return victim;
        }
    }

    // return the most outdated cache page that is neither the current page nor pinned.
    // returns -1 if every eligible slot is pinned (caller waits for a pin to be released).
    int getMostOutdatedCachePageExceptCurrentPage()
    {
        int min_index = -1;
        for (int i = 0; i <= numCachePage; i++)
        {
            if (cachePageId[i] == currentPageId) // skip current page
                continue;
            if (cachePageId[i] == LOADING_PAGE_ID) // skip slots reserved for an in-flight load
                continue;
            if (pinCount[i] > 0)                 // skip pinned pages (still referenced)
                continue;
            if (lastAccessedTimestamp[i] == 0)
            {
                return i;
            }
            if (min_index == -1
                || (lastAccessedTimestamp[i] < lastAccessedTimestamp[min_index])
                || (lastAccessedTimestamp[i] == lastAccessedTimestamp[min_index] && cachePageId[i] < cachePageId[min_index]))
            {
                min_index = i;
            }
        }
        return min_index;
    }
public:
    SwapVirtualMemory()
    {
        VMBase();
    }

    ~SwapVirtualMemory()
    {
#if defined(__linux__)
        // Drop the dirty-track registry entry before &currentPage dies (only tests destroy SwapVMs;
        // the node's are static). Else the SIGSEGV fault path derefs a dangling stack pointer.
        SwapDirtyTrack::unregisterPool((unsigned char**)&currentPage);
#endif
    }

    void reset() {
        // Invalidate all pin bookkeeping atomically w.r.t. concurrent pinners. Bumping the
        // generation here is what makes other threads' stale pin notes (e.g. HTTP request
        // threads that pinned tickData and aren't drained at this point) harmless: their later
        // release sees gen != generation and is dropped, instead of mis-decrementing a fresh pin.
        acquireMemLock();
        // Wait out in-flight swap IO before teardown (HTTP threads run it unlocked and aren't parked):
        // an fwrite racing the pool-zero below tears the .pg and strands a stale written-flag.
        for (int drainMs = 0; anyInflightIO(); )
        {
            RELEASE(memLock);
            sleepMilliseconds(1);
            acquireMemLock();
            if (++drainMs > 5000)
            {
                setText(message, L"Fatal: swapVM reset drain timeout (in-flight IO leak) | prefix ");
                appendText(message, pageDir);
                logToConsole(message);
                exit(1);
            }
        }
        generation++;
        setMem((void*)pinCount, sizeof(pinCount), 0);
        setMem(loadingTarget, sizeof(loadingTarget), 0xff); // INVALID_PAGE_ID
        setMem(evictingPage, sizeof(evictingPage), 0xff);
        pinnedNow = 0; // cumulative stats (hits/misses/highWater/waits) are kept across resets
        RELEASE(memLock);
        unprotectWholePool(); // RW + clear dirty so the base's bulk pool-zero below cannot fault
        VMBase::reset();
        setMem(isPageWrittenToDisk, isPageWrittenToDiskSize, 0);
        if (mode == SwapMode::OFFSET_MODE) {
            setMem(pageExtraBytesBuffer, pageExtraBytesBufferSize, 0);
            setMem(pageHasExtraBytes, pageHasExtraBytesBufferSize, 0);
            setMem(lastestPageExtraBytesOffsetAccessed, lastestPageExtraBytesOffsetAccessedBufferSize, 0xff);
        }
        // Keep the cache pool across reset (only refresh pageDir's epoch): reset doesn't wait for
        // unparked HTTP readers, so freeing+realloc'ing it would dangle a ref they still hold
        // (VMBase::reset already zeroed it in place; init() keeps it since currentPage != NULL).
        if (pageDir != NULL)
        {
            freePool(pageDir);
            pageDir = NULL;
        }
        VMBase::init(); // pool kept (currentPage != NULL); pageDir re-alloc'd for the new epoch
#if defined(__linux__)
        armSlotAfterLoad(0); // re-arm slot 0: VMBase::init's reset set cachePageId[0]=0 again
#endif
    }

    bool init()
    requires (mode == SwapMode::OFFSET_MODE)
    {
        ASSERT(extraBytesPerElement >= 0);
        pageSize = maxBytesPerPage;
        if (!allocPoolWithErrorLog(L"SwapVM.IsPageWrittenToDisk", isPageWrittenToDiskSize, (void**)&isPageWrittenToDisk, __LINE__))
        {
            return false;
        }

        if (
            !allocPoolWithErrorLog(L"SwapVM.ExtraBytes", pageExtraBytesBufferSize, (void**)&pageExtraBytesBuffer, __LINE__) ||
            !allocPoolWithErrorLog(L"SwapVM.PageHasExtraBytes", pageHasExtraBytesBufferSize, (void**)&pageHasExtraBytes, __LINE__) ||
            !allocPoolWithErrorLog(L"SwapVM.LastestPageExtraBytesOffsetAccessed", lastestPageExtraBytesOffsetAccessedBufferSize, (void**)&lastestPageExtraBytesOffsetAccessed, __LINE__))
        {
            return false;
        }
        setMem(loadingTarget, sizeof(loadingTarget), 0xff); // INVALID_PAGE_ID
        setMem(evictingPage, sizeof(evictingPage), 0xff);
#if defined(__linux__)
        if (!allocPageAlignedCachePool())
            return false;
#endif
        bool ok = VMBase::init();
#if defined(__linux__)
        registerDirtyTrackPool();
        armSlotAfterLoad(0); // slot 0 holds page 0 at init (cachePageId[0]=0) without taking the load/arm path
#endif
        return ok;
    }

    bool init()
    requires (mode == SwapMode::INDEX_MODE)
    {
        if (!allocPoolWithErrorLog(L"SwapVM.IsPageWrittenToDisk", isPageWrittenToDiskSize, (void**)&isPageWrittenToDisk, __LINE__))
        {
            return false;
        }

        setMem(loadingTarget, sizeof(loadingTarget), 0xff); // INVALID_PAGE_ID
        setMem(evictingPage, sizeof(evictingPage), 0xff);
#if defined(__linux__)
        if (!allocPageAlignedCachePool())
            return false;
#endif
        bool ok = VMBase::init();
#if defined(__linux__)
        registerDirtyTrackPool();
        armSlotAfterLoad(0); // slot 0 holds page 0 at init (cachePageId[0]=0) without taking the load/arm path
#endif
        return ok;
    }

    T& getRef(unsigned long long index)
    requires (mode == SwapMode::INDEX_MODE)
    {
        const unsigned long long requested_page_id = index / pageCapacity;

        // Fast path: shared lock, cache hit only. Identity/contents are stable under shared (eviction
        // is exclusive), so the pin keeps the returned ref valid after unlockShared().
        lockShared();
        int cache_page_idx = findCachePage(requested_page_id);
        if (cache_page_idx != -1)
        {
            cacheHits++; // best-effort stat
            pinSlotForThread(cache_page_idx);
            lastAccessedTimestamp[cache_page_idx] = now_ms(); // refresh LRU on hit (racy write, harmless)
            T& resultRef = cache[cache_page_idx][index % pageCapacity];
            unlockShared();
            return resultRef;
        }
        unlockShared();

        // Slow path: miss -> exclusive (may evict + load).
        acquireMemLock();
        currentPageId = requested_page_id > currentPageId ? requested_page_id : currentPageId;
        cache_page_idx = loadPageToCacheAndTryToPersist(requested_page_id);
        if (cache_page_idx == -1)
        {
            setText(message, L"Fatal Error: Invalid cache page index | Line ");
            appendNumber(message, __LINE__, true);
            logToConsole(message);
            // Exit program
            exit(1);
        }
        pinSlotForThread(cache_page_idx); // keep page resident until this thread's next boundary
        T& resultRef = cache[cache_page_idx][index % pageCapacity];
        RELEASE(memLock);
        return resultRef;
    }

    // The returned pointer stays valid until the current thread's next pin-release boundary,
    // because getRef/getPtr pin the cache slot in the thread-local arena (see releaseThreadPins).
    T* getPtr(unsigned long long index)
    requires (mode == SwapMode::INDEX_MODE)
    {
        const unsigned long long requested_page_id = index / pageCapacity;

        // Fast path: shared lock, cache hit only (see getRef). result is a plain local — a static
        // would be read after unlock and could return another thread's pointer.
        lockShared();
        int cache_page_idx = findCachePage(requested_page_id);
        if (cache_page_idx != -1)
        {
            cacheHits++; // best-effort stat
            pinSlotForThread(cache_page_idx);
            lastAccessedTimestamp[cache_page_idx] = now_ms(); // refresh LRU on hit (racy write, harmless)
            T* result = &cache[cache_page_idx][index % pageCapacity];
            unlockShared();
            return result;
        }
        unlockShared();

        // Slow path: miss -> exclusive (may evict + load).
        acquireMemLock();
        currentPageId = requested_page_id > currentPageId ? requested_page_id : currentPageId;
        cache_page_idx = loadPageToCacheAndTryToPersist(requested_page_id);
        if (cache_page_idx == -1)
        {
            setText(message, L"Fatal Error: Invalid cache page index | Line ");
            appendNumber(message, __LINE__, true);
            logToConsole(message);

            setText(message, L"Requested Page ID: ");
            appendNumber(message, requested_page_id, true);
            appendText(message, L"| Max Page ID: ");
            appendNumber(message, currentPageId, true);
            logToConsole(message);

            setText(message, L"SwapVm Prefix: ");
            appendText(message, pageDir);
            logToConsole(message);
            // Exit program
            exit(1);
        }
        pinSlotForThread(cache_page_idx); // keep page resident until this thread's next boundary
        T* result = &cache[cache_page_idx][index % pageCapacity];
        RELEASE(memLock);
        return result;
    }

    // Monitoring getters. Read lock-free on purpose (stats only — a stale read is harmless).
    // pinnedHighWater approaching getNumCachePage() means raise numCachePage.
    int getPinnedNow() const { return pinnedNow; }
    int getPinnedHighWater() const { return pinnedHighWater; }
    int getPinsHeldByThisThread() const { return tlPinArena.heldOn(this, generation); }
    unsigned long long getAllPinnedWaits() const { return allPinnedWaits; }
    unsigned long long getCacheHits() const { return cacheHits; }
    unsigned long long getCacheMisses() const { return cacheMisses; }
    unsigned long long getCleanEvicts() const { return cleanEvicts; } // writeback skipped (clean victim)
    unsigned long long getDirtyEvicts() const { return dirtyEvicts; }
    static constexpr unsigned long long getNumCachePage() { return numCachePage; }

    // Clear inherited locks and dead parent-thread pins before the promoted child starts workers.
    void resetPinsForChildPromote()
    {
        memLock = 0;
        memReaders = 0;
        // pinCount is volatile, so clear it element by element.
        for (int i = 0; i <= numCachePage; i++)
            pinCount[i] = 0;

        // Orphan loading slots have no surviving worker and would block later dedup waits.
        for (int i = 0; i <= numCachePage; i++)
        {
            if (cachePageId[i] == LOADING_PAGE_ID)
                cachePageId[i] = INVALID_PAGE_ID;
        }
        setMem(loadingTarget, sizeof(loadingTarget), 0xff);   // INVALID_PAGE_ID
        setMem(evictingPage, sizeof(evictingPage), 0xff);
        pinnedNow = 0;
    }

    // Wait for unlocked miss I/O before fork or commit after callers have blocked new I/O sources.
    // A timeout leaves recovery to resetPinsForChildPromote().
    bool drainInflightIO(int timeoutMs)
    {
        for (int elapsedMs = 0; elapsedMs <= timeoutMs; elapsedMs++)
        {
            acquireMemLock();
            const bool hasInflightIO = anyInflightIO();
            RELEASE(memLock);
            if (!hasInflightIO)
                return true;
            sleepMilliseconds(1);
        }
        return false;
    }

    T* operator[](unsigned long long offset)
    requires (mode == SwapMode::OFFSET_MODE)
    {
        const unsigned long long pageId = offset / maxBytesPerPage;
        const unsigned long long offsetInPage = offset % maxBytesPerPage;

        // Fast path: shared, in-page hit only. Straddle (mutates per-page state) + miss go exclusive.
        if (maxBytesPerPage - offsetInPage >= maxBytesPerElement)
        {
            lockShared();
            int idx = findCachePage(pageId);
            if (idx != -1)
            {
                cacheHits++; // best-effort stat
                pinSlotForThread(idx);
                lastAccessedTimestamp[idx] = now_ms(); // refresh LRU on hit (racy write, harmless)
                T* hit = (T*)((unsigned char*)cache[idx] + offsetInPage);
                unlockShared();
                return hit;
            }
            unlockShared();
        }

        // Slow path: miss or page-straddle -> exclusive.
        T* result = nullptr;
        long long lastElementLength = 0;
        acquireMemLock();
        currentPageId = pageId > currentPageId ? pageId : currentPageId;
        int cache_page_idx = loadPageToCacheAndTryToPersist(pageId);
        if (cache_page_idx == -1)
        {
            setText(message, L"Fatal Error: Invalid cache page index | Line ");
            appendNumber(message, __LINE__, true);
            logToConsole(message);
            // Exit program
            exit(1);
        }

        // To avoid cross page access, if the remaining bytes in this page is less than maxBytesPerElement,
        // we will tmp use extra buffer to store the element
        if (maxBytesPerPage - offsetInPage < maxBytesPerElement) {
            T* thisPageExtraBuffer = (T*)((unsigned char *)pageExtraBytesBuffer + pageId * maxBytesPerElement);
            if (pageHasExtraBytes[pageId]) {
                lastElementLength = offset - lastestPageExtraBytesOffsetAccessed[pageId];
                // if lastElementLength < 0, means the element already in cache, just return the element in cache
                if (lastElementLength < 0) {
                    goto normal_access;
                } else if (lastElementLength == 0) { // if lastElementLength == 0, means the element is already in extra buffer, just return the extra buffer
                    RELEASE(memLock);
                    return thisPageExtraBuffer;
                }
                // if lastElementLength > 0 (new element need to be inserted), copy the last element in extra buffer to cache
                copyMem((unsigned char *)cache[cache_page_idx] + (lastestPageExtraBytesOffsetAccessed[pageId] % maxBytesPerPage), thisPageExtraBuffer, lastElementLength);
                lastestPageExtraBytesOffsetAccessed[pageId] = offset;
                // reset the extra buffer
                setMem(thisPageExtraBuffer, maxBytesPerElement, 0);
                RELEASE(memLock);
                return thisPageExtraBuffer;
            } else {
                lastestPageExtraBytesOffsetAccessed[pageId] = offset;
                pageHasExtraBytes[pageId] = true;
                RELEASE(memLock);
                return thisPageExtraBuffer;
            }
        }

        normal_access:
        pinSlotForThread(cache_page_idx);
        unsigned char *pageBuffer = (unsigned char*)cache[cache_page_idx];
        result = (T*)(pageBuffer + offsetInPage);
        RELEASE(memLock);
        return result;
    }

    T* operator[](unsigned long long index)
    requires (mode == SwapMode::INDEX_MODE)
    {
        return getPtr(index);
    }

    T& operator()(unsigned long long index)
    requires (mode == SwapMode::INDEX_MODE)
    {
        return getRef(index);
    }

    unsigned long long getVmStateSize()
    {
        unsigned long long totalOffsetModeExtraSize = 0;
        if (mode == SwapMode::OFFSET_MODE)
        {
            totalOffsetModeExtraSize = pageExtraBytesBufferSize + pageHasExtraBytesBufferSize + lastestPageExtraBytesOffsetAccessedBufferSize;
        }
        return pageSize * (numCachePage + 1) + isPageWrittenToDiskSize  + sizeof(cachePageId) + sizeof(lastAccessedTimestamp) + 8 + totalOffsetModeExtraSize;
    }

    unsigned long long dumpVMState(unsigned char* buffer)
    {
        acquireMemLock();
        // Drain in-flight IO so a slot isn't snapshotted mid-fread; a slot still LOADING after the
        // bounded wait is captured as LOADING and normalized to INVALID on load (consistent fallback).
        for (int drainMs = 0; anyInflightIO() && drainMs < 1000; drainMs++)
        {
            RELEASE(memLock);
            sleepMilliseconds(1);
            acquireMemLock();
        }
        unsigned long long ret = 0;
        for (int i = 0; i <= numCachePage; i++)
        {
            copyMem(buffer, cache[i], pageSize);
            buffer += pageSize;
        }
        ret += (numCachePage+1) * pageSize;

        copyMem(buffer, isPageWrittenToDisk, isPageWrittenToDiskSize);
        ret += isPageWrittenToDiskSize;
        buffer += isPageWrittenToDiskSize;

        if (mode == SwapMode::OFFSET_MODE)
        {
            copyMem(buffer, pageExtraBytesBuffer, pageExtraBytesBufferSize);
            buffer += pageExtraBytesBufferSize;
            ret += pageExtraBytesBufferSize;

            copyMem(buffer, pageHasExtraBytes, pageHasExtraBytesBufferSize);
            buffer += pageHasExtraBytesBufferSize;
            ret += pageHasExtraBytesBufferSize;

            copyMem(buffer, lastestPageExtraBytesOffsetAccessed, lastestPageExtraBytesOffsetAccessedBufferSize);
            buffer += lastestPageExtraBytesOffsetAccessedBufferSize;
            ret += lastestPageExtraBytesOffsetAccessedBufferSize;
        }

        copyMem(buffer, lastAccessedTimestamp, sizeof(lastAccessedTimestamp));
        buffer += sizeof(lastAccessedTimestamp);
        ret += sizeof(lastAccessedTimestamp);

        copyMem(buffer, cachePageId, sizeof(cachePageId));
        ret += sizeof(cachePageId);
        buffer += sizeof(cachePageId);

        *((unsigned long long*)buffer) = currentPageId;
        buffer += 8;
        ret += 8;
        RELEASE(memLock);
        return ret;
    }

    unsigned long long loadVMState(unsigned char* buffer)
    {
        acquireMemLock();
        unprotectWholePool();   // the bulk restore writes into (possibly armed) slots must not fault
        unsigned long long ret = 0;
        for (int i = 0; i <= numCachePage; i++)
        {
            copyMem(cache[i], buffer, pageSize);
            buffer += pageSize;
        }
        ret += (numCachePage+1) * pageSize;

        copyMem(isPageWrittenToDisk, buffer, isPageWrittenToDiskSize);
        buffer += isPageWrittenToDiskSize;
        ret += isPageWrittenToDiskSize;

        if (mode == SwapMode::OFFSET_MODE)
        {
            copyMem(pageExtraBytesBuffer, buffer, pageExtraBytesBufferSize);
            buffer += pageExtraBytesBufferSize;
            ret += pageExtraBytesBufferSize;

            copyMem(pageHasExtraBytes, buffer, pageHasExtraBytesBufferSize);
            buffer += pageHasExtraBytesBufferSize;
            ret += pageHasExtraBytesBufferSize;

            copyMem(lastestPageExtraBytesOffsetAccessed, buffer, lastestPageExtraBytesOffsetAccessedBufferSize);
            buffer += lastestPageExtraBytesOffsetAccessedBufferSize;
            ret += lastestPageExtraBytesOffsetAccessedBufferSize;
        }

        copyMem(lastAccessedTimestamp, buffer, sizeof(lastAccessedTimestamp));
        buffer += sizeof(lastAccessedTimestamp);
        ret += sizeof(lastAccessedTimestamp);

        copyMem(cachePageId, buffer, sizeof(cachePageId));
        buffer += sizeof(cachePageId);
        ret += sizeof(cachePageId);

        // In-flight markers are transient and unserialized; a snapshot may have caught a slot mid-load.
        // Treat any such slot as empty and clear the in-flight arrays.
        for (int i = 0; i <= numCachePage; i++)
            if (cachePageId[i] == LOADING_PAGE_ID)
                cachePageId[i] = INVALID_PAGE_ID;
        setMem(loadingTarget, sizeof(loadingTarget), 0xff);
        setMem(evictingPage, sizeof(evictingPage), 0xff);

        // Snapshot cache bytes may be newer than or absent from their .pg backing.
        for (int i = 0; i <= numCachePage; i++)
            if (cachePageId[i] != INVALID_PAGE_ID)
                dirtyFlags[i] = 1;

        currentPageId = *((unsigned long long*)buffer);
        buffer += 8;
        ret += 8;

        RELEASE(memLock);
        return ret;
    }

    T* getPageBuffer(unsigned long long pageId) {
        auto cacheIndex = findCachePage(pageId);
        if (cacheIndex == -1) {
            return nullptr;
        }
        return cache[cacheIndex];
    }

    T* getCacheBuffer(unsigned long long cacheId) {
        if (cacheId > numCachePage) {
            return nullptr;
        }
        return cache[cacheId];
    }

    T* getExtraBuffer(unsigned long long pageId) {
        return &pageExtraBytesBuffer[pageId];
    }

    unsigned long long getPageSize()
    {
        return pageSize;
    }

    const T* getCurrentPagePtr()
    {
        return currentPage;
    }
};
