#pragma once

// Incremental KangarooTwelve cache for smart-contract state digests. K12 hashes its input as a
// one-level tree of 8192-byte chunks (each interior chunk -> a 32-byte chaining value absorbed
// into a final node), so a byte change in one chunk only invalidates that chunk's CV. We cache
// the CVs per contract and, using mprotect-read-only + the SIGSEGV handler to learn which chunks
// changed since the last digest, re-hash only the dirty chunks and reuse the rest. The result is
// byte-identical to the one-shot KangarooTwelve: dirty leaves go through KangarooTwelveLeaf() and
// cached ones through KangarooTwelveStream::absorbChainingValue(), the same tree steps as the
// one-shot. All state stays in RAM. On by default on Linux; --no-k12-state-cache off.

#include "../kangaroo_twelve.h"   // KangarooTwelveStream/KangarooTwelveLeaf + K12_chunkSize/K12_capacityInBytes + copyMem/setMem

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#endif

namespace K12StateDigestCache
{
    inline constexpr unsigned int K12SDC_CHUNK = K12_chunkSize;   // 8192
    inline constexpr unsigned int K12SDC_CV    = K12_capacityInBytes;   // 32

    // Bit-exact with KangarooTwelve(state,size,out,32). cvCache holds 32 bytes per chunk; only
    // chunks with chunkDirty[k] set are re-hashed (and their CV re-cached), the rest reuse cvCache[k].
    // cvCache[0] is unused (chunk 0 goes into the final node, no CV); the partial tail is always
    // re-read. Pure, self-contained so it is unit-testable without the node TU.
    inline void incrementalDigest(const unsigned char* state, unsigned long long size,
                                  unsigned char* cvCache, const unsigned char* chunkDirty,
                                  unsigned char* out)
    {
        KangarooTwelveStream stream;
        stream.init();
        if (size <= K12SDC_CHUNK)
        {
            stream.update(state, size);   // single node: no tree
            stream.finalize(out);
            return;
        }

        stream.update(state, K12SDC_CHUNK);
        unsigned long long off = K12SDC_CHUNK, k = 1;
        for (; off + K12SDC_CHUNK <= size; off += K12SDC_CHUNK, ++k)
        {
            unsigned char* cv = cvCache + k * K12SDC_CV;
            if (chunkDirty[k])
            {
                KangarooTwelveLeaf(state + off, cv);
            }
            stream.absorbChainingValue(cv);
        }
        stream.update(state + off, size - off);
        stream.finalize(out);
    }

#if defined(__linux__)
    inline bool gK12StateDigestCacheEnabled = true;     // on by default; --no-k12-state-cache flips it
#else
    inline bool gK12StateDigestCacheEnabled = false;    // mprotect+SIGSEGV is Linux-only; one-shot elsewhere
#endif
    inline bool gK12StateDigestCacheVerify = false;     // --k12-state-cache-verify: also run one-shot, stall on mismatch

#if defined(__linux__) && defined(SINGLE_COMPILE_UNIT)

    inline unsigned long long pageSize() { return (unsigned long long)sysconf(_SC_PAGESIZE); }

    // Only multi-chunk states benefit, and a chunk must be a whole number of pages so a per-chunk
    // mprotect is exact (true for 8192/4096; the page-size guard falls back to one-shot otherwise).
    inline constexpr unsigned int K12SDC_MIN_CHUNKS = 16;   // ~128 KB
    inline bool shouldCache(unsigned long long size)
    {
        return gK12StateDigestCacheEnabled
            && size > (unsigned long long)K12SDC_CHUNK * K12SDC_MIN_CHUNKS
            && (K12SDC_CHUNK % pageSize()) == 0;
    }
    inline bool wantsPageAlignedAlloc(unsigned long long size) { return shouldCache(size); }

    struct Record
    {
        unsigned char*          cv         = nullptr;   // 32*numChunks, allocated once
        volatile unsigned char* chunkDirty = nullptr;   // numChunks bytes, 1 = dirty
        unsigned long long      size       = 0;
        unsigned long long      allocLen   = 0;         // numChunks*CHUNK (page multiple)
        unsigned int            numChunks  = 0;
        bool                    cached     = false;
        bool                    armed      = false;     // false until first computeDigest arms whole region RO
    };
    inline Record gCache[MAX_NUMBER_OF_CONTRACTS];

    // Compact registry the SIGSEGV fast path scans lock-free (mirrors SwapDirtyTrack::gPools).
    struct Region { unsigned char** basePtr; unsigned long long allocLen; volatile unsigned char* chunkDirty; };
    inline constexpr int MAX_REGIONS = (int)MAX_NUMBER_OF_CONTRACTS;
    inline Region gRegions[MAX_REGIONS];
    inline std::atomic<int> gRegionCount{0};

    inline bool isCached(unsigned int i) { return gCache[i].cached; }

    // Page-aligned state buffer for cached contracts (free()-compatible with freePool).
    inline bool allocStatePool(unsigned long long size, void** buffer)
    {
        const unsigned long long ps = pageSize();
        const unsigned long long numChunks = (size + K12SDC_CHUNK - 1) / K12SDC_CHUNK;
        const unsigned long long allocLen  = numChunks * K12SDC_CHUNK;   // multiple of CHUNK => multiple of page
        void* p = aligned_alloc((size_t)ps, (size_t)allocLen);
        if (!p)
            return false;
        setMem(p, allocLen, 0);
        *buffer = p;
        return true;
    }

    // Allocate derived structures + register for fault dispatch. Reads no contract bytes (CVs are
    // built lazily, all-dirty, at the first computeDigest). Single-threaded, before tick threads.
    inline void init()
    {
        if (!gK12StateDigestCacheEnabled)
            return;
        unsigned int cachedCount = 0;
        unsigned long long trackedBytes = 0;
        for (unsigned int i = 0; i < contractCount; i++)
        {
            const unsigned long long size = contractDescriptions[i].stateSize;
            if (gCache[i].cached || !shouldCache(size) || contractStates[i] == nullptr)
                continue;
            Record& r = gCache[i];
            r.size = size;
            r.numChunks = (unsigned int)((size + K12SDC_CHUNK - 1) / K12SDC_CHUNK);
            r.allocLen = (unsigned long long)r.numChunks * K12SDC_CHUNK;
            r.cv = (unsigned char*)malloc((size_t)r.numChunks * K12SDC_CV);
            r.chunkDirty = (volatile unsigned char*)malloc((size_t)r.numChunks);
            if (!r.cv || !r.chunkDirty)
            {
                free(r.cv);
                free((void*)r.chunkDirty);
                r.cv = nullptr; r.chunkDirty = nullptr;
                continue;
            }
            setMem(r.cv, (size_t)r.numChunks * K12SDC_CV, 0);
            setMem((void*)r.chunkDirty, r.numChunks, 1);   // first digest rebuilds every CV
            r.armed = false;
            r.cached = true;
            cachedCount++;
            trackedBytes += r.allocLen;
            const int idx = gRegionCount.fetch_add(1, std::memory_order_acq_rel);
            if (idx < MAX_REGIONS)
                gRegions[idx] = { (unsigned char**)&contractStates[i], r.allocLen, r.chunkDirty };
        }
        printf("K12 state-digest cache: incrementally hashing %u contract state(s), %llu MB tracked%s\n",
               cachedCount, trackedBytes >> 20, gK12StateDigestCacheVerify ? " (self-verify ON)" : "");
    }

    // SIGSEGV fast path (chained from signalHandler). Async-signal-safe: atomic load + byte store +
    // mprotect over an immutable registry. Maps the faulting address to its chunk, marks it dirty,
    // restores write access, returns true (kernel retries the store).
    inline bool tryMarkDirty(void* addr)
    {
        if (!gK12StateDigestCacheEnabled)
            return false;
        unsigned char* a = (unsigned char*)addr;
        const int n = gRegionCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; i++)
        {
            Region& rg = gRegions[i];
            unsigned char* base = *rg.basePtr;
            if (base == nullptr)
                continue;
            if (a >= base && a < base + rg.allocLen)
            {
                const unsigned int chunk = (unsigned int)(((unsigned long long)(a - base)) / K12SDC_CHUNK);
                rg.chunkDirty[chunk] = 1;
                mprotect(base + (unsigned long long)chunk * K12SDC_CHUNK, K12SDC_CHUNK, PROT_READ | PROT_WRITE);
                return true;
            }
        }
        return false;
    }

    // Runs under the caller's contractStateLock[i] read-lock (writers blocked, single-threaded) so
    // arm + dirty-clear is race-free vs faults. Computes the digest, then arms the region read-only
    // for the next tick: first call arms the whole region; later calls re-arm only the chunks
    // faulted this cycle.
    inline void computeDigest(unsigned int i, void* out)
    {
        Record& r = gCache[i];
        incrementalDigest(contractStates[i], r.size, r.cv, (const unsigned char*)r.chunkDirty, (unsigned char*)out);

        if (gK12StateDigestCacheVerify)
        {
            m256i ref;
            KangarooTwelve(contractStates[i], (unsigned int)r.size, &ref, 32);
            if (memcmp(&ref, out, 32) != 0)
                while (true) { fprintf(stderr, "FATAL: K12 state-cache digest != one-shot (verify), contract %u\n", i); fflush(stderr); sleep(1); }
        }

        unsigned char* base = contractStates[i];
        if (!r.armed)
        {
            mprotect(base, r.allocLen, PROT_READ);
            setMem((void*)r.chunkDirty, r.numChunks, 0);
            r.armed = true;
        }
        else
        {
            for (unsigned int k = 0; k < r.numChunks; k++)
                if (r.chunkDirty[k])
                {
                    mprotect(base + (unsigned long long)k * K12SDC_CHUNK, K12SDC_CHUNK, PROT_READ);
                    r.chunkDirty[k] = 0;
                }
        }
    }

    // Force a full canonical rebuild: drop protection + mark all chunks dirty. For a rollback that
    // restores contract bytes by a non-faulting path, and for the quorum-mismatch recovery.
    // Caller holds contractStateLock[i] (or runs single-threaded at the digest site).
    inline void invalidateContract(unsigned int i)
    {
        Record& r = gCache[i];
        if (!r.cached)
            return;
        mprotect(contractStates[i], r.allocLen, PROT_READ | PROT_WRITE);
        setMem((void*)r.chunkDirty, r.numChunks, 1);
        r.armed = false;
    }
    inline void invalidateAll() { for (unsigned int i = 0; i < contractCount; i++) invalidateContract(i); }

#else   // non-Linux or non-TU: inert stubs so the core hooks compile and the path stays one-shot

    inline bool isCached(unsigned int) { return false; }
    inline void init() {}
    inline void computeDigest(unsigned int, void*) {}
    inline bool tryMarkDirty(void*) { return false; }
    inline bool wantsPageAlignedAlloc(unsigned long long) { return false; }
    inline bool allocStatePool(unsigned long long, void**) { return false; }
    inline void invalidateContract(unsigned int) {}
    inline void invalidateAll() {}

#endif
}
