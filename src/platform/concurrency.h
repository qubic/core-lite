#pragma once

#include <lib/platform_common/qintrin.h>
#include <atomic>
#include <cstdio>

#if defined(__linux__) || defined(__APPLE__)
#define _byteswap_ulong(x) __builtin_bswap32(x)
#define _InterlockedExchange8(target, val) __atomic_exchange_n(target, val, __ATOMIC_SEQ_CST)
#define _InterlockedIncrement64(target) __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST)
#define _InterlockedAnd64(target, val) __atomic_fetch_and(target, val, __ATOMIC_SEQ_CST)
#define _InterlockedExchange(target, val) __atomic_exchange_n(target, val, __ATOMIC_SEQ_CST)
#define _InterlockedExchange64(target, val) __atomic_exchange_n(target, val, __ATOMIC_SEQ_CST)
static long long _InterlockedCompareExchange64(volatile long long *target, long long exchange, long long comparand) {
    __atomic_compare_exchange_n(target, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
static char _InterlockedCompareExchange8(volatile char *target, char exchange, char comparand) {
    __atomic_compare_exchange_n(target, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
static long _InterlockedCompareExchange(volatile long *target, long exchange, long comparand) {
    __atomic_compare_exchange_n(target, &comparand, exchange, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
#define _InterlockedExchangeAdd64(target, val) __atomic_fetch_add(target, val, __ATOMIC_SEQ_CST)
#define _interlockedadd64 _InterlockedExchangeAdd64
#define _InterlockedDecrement(target) __atomic_sub_fetch(target, 1, __ATOMIC_SEQ_CST)
#define _InterlockedIncrement(target) __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST)
#endif

// Track per-thread node-lock depth so bspForkPoint can reject forks with locks held elsewhere.
// Global slots keep short-lived thread records valid through thread-exit cleanup.
inline thread_local int tlLockSlot = -1;   // this thread's census slot; -1 until first acquire

namespace ForkCensus
{
    inline constexpr int MAX_THREADS = 2048;
    struct alignas(64) Slot
    {
        std::atomic<int> depth{ 0 };
        std::atomic<const char*> what{ nullptr };
        // Every LockGuard lock reports the same `what`, so only the address names the offender.
        std::atomic<const volatile void*> where{ nullptr };
        std::atomic<int> live{ 0 };   // 0 = free/reusable, 1 = owned by a live thread
    };
    inline Slot gSlots[MAX_THREADS];
    inline std::atomic<int> gCount{ 0 };   // high-water of ever-claimed slots (<= MAX_THREADS)
    // Latched if a thread ever can't get a slot (>MAX_THREADS live). Makes overflow fail-SAFE: the gate
    // then treats the census as unreliable and degrades to strict (never forks with an undercount).
    inline std::atomic<bool> gOverflow{ false };

    // Free this thread's slot at exit (slot memory is global, so the gate never reads dead storage).
    struct Unreg
    {
        ~Unreg()
        {
            if (tlLockSlot < 0)
                return;
            gSlots[tlLockSlot].depth.store(0, std::memory_order_relaxed);
            gSlots[tlLockSlot].what.store(nullptr, std::memory_order_relaxed);
            gSlots[tlLockSlot].live.store(0, std::memory_order_release);   // free for reuse
        }
    };
    inline thread_local Unreg tlUnreg;

    inline void claimSlot()
    {
        for (;;)
        {
            const int slotCount = gCount.load(std::memory_order_acquire);
            for (int slotIndex = 0; slotIndex < slotCount; slotIndex++)
            {
                int expectedFree = 0;
                if (gSlots[slotIndex].live.compare_exchange_strong(expectedFree, 1, std::memory_order_acq_rel))
                {
                    tlLockSlot = slotIndex;
                    (void)&tlUnreg;
                    return;
                }
            }
            const int newSlot = gCount.fetch_add(1, std::memory_order_acq_rel);
            if (newSlot >= MAX_THREADS)
            {
                gCount.fetch_sub(1, std::memory_order_acq_rel);
                if (!gOverflow.exchange(true, std::memory_order_acq_rel))
                {
                    fprintf(stderr, "[FORKCENSUS] slot overflow (>%d lock-holding threads) -> forks degrade to strict\n", MAX_THREADS);
                    fflush(stderr);
                }
                return;
            }
            int expectedFree = 0;
            if (gSlots[newSlot].live.compare_exchange_strong(expectedFree, 1, std::memory_order_acq_rel))
            {
                tlLockSlot = newSlot;
                (void)&tlUnreg;
                return;
            }
            // A reuse scan claimed the fresh slot before this thread could publish it.
        }
    }

    inline void enter(const char* what, const volatile void* where = nullptr)
    {
        if (tlLockSlot < 0)
            claimSlot();
        if (tlLockSlot < 0)
            return;   // registry full (>MAX_THREADS live): best-effort, this thread uncounted
        gSlots[tlLockSlot].what.store(what, std::memory_order_relaxed);
        gSlots[tlLockSlot].where.store(where, std::memory_order_relaxed);
        gSlots[tlLockSlot].depth.fetch_add(1, std::memory_order_relaxed);
    }
    inline void leave()
    {
        if (tlLockSlot >= 0)
            gSlots[tlLockSlot].depth.fetch_sub(1, std::memory_order_relaxed);
    }

    // Held depth across all slots but the caller's own (excludes the BSP's deliberate fork-time holds).
    inline int sumExceptSelf()
    {
        if (gOverflow.load(std::memory_order_acquire))
            return MAX_THREADS;   // unreliable -> force the gate to skip the fork
        const int selfSlot = tlLockSlot;
        int slotCount = gCount.load(std::memory_order_acquire);
        if (slotCount > MAX_THREADS)
            slotCount = MAX_THREADS;
        int heldDepth = 0;
        for (int slotIndex = 0; slotIndex < slotCount; slotIndex++)
        {
            if (slotIndex != selfSlot)
                heldDepth += gSlots[slotIndex].depth.load(std::memory_order_relaxed);
        }
        return heldDepth < 0 ? 0 : heldDepth;
    }
    inline const volatile void* offenderAddress()
    {
        const int selfSlot = tlLockSlot;
        int slotCount = gCount.load(std::memory_order_acquire);
        if (slotCount > MAX_THREADS)
            slotCount = MAX_THREADS;
        for (int slotIndex = 0; slotIndex < slotCount; slotIndex++)
        {
            if (slotIndex != selfSlot && gSlots[slotIndex].depth.load(std::memory_order_relaxed) > 0)
            {
                return gSlots[slotIndex].where.load(std::memory_order_relaxed);
            }
        }
        return nullptr;
    }

    inline const char* offenderName()
    {
        if (gOverflow.load(std::memory_order_acquire))
            return "fork-census-slot-overflow";
        const int selfSlot = tlLockSlot;
        int slotCount = gCount.load(std::memory_order_acquire);
        if (slotCount > MAX_THREADS)
            slotCount = MAX_THREADS;
        for (int slotIndex = 0; slotIndex < slotCount; slotIndex++)
        {
            if (slotIndex != selfSlot && gSlots[slotIndex].depth.load(std::memory_order_relaxed) > 0)
            {
                return gSlots[slotIndex].what.load(std::memory_order_relaxed);
            }
        }
        return nullptr;
    }

    // Promotion clears dead parent-thread slots before new workers start; otherwise they accumulate
    // into a permanent overflow that disables future forks.
    inline void resetForChildPromote()
    {
        for (int i = 0; i < MAX_THREADS; i++)
        {
            gSlots[i].depth.store(0, std::memory_order_relaxed);
            gSlots[i].what.store(nullptr, std::memory_order_relaxed);
            gSlots[i].where.store(nullptr, std::memory_order_relaxed);
            gSlots[i].live.store(0, std::memory_order_relaxed);
        }
        gCount.store(0, std::memory_order_release);
        gOverflow.store(false, std::memory_order_release);
        ::tlLockSlot = -1;   // surviving thread re-claims a fresh slot on its next ACQUIRE
    }
}

inline void forkCensusEnter(const char* what, const volatile void* where = nullptr)
{
    ForkCensus::enter(what, where);
}

inline void forkCensusLeave()
{
    ForkCensus::leave();
}

inline int forkCensusSumExcept()
{
    return ForkCensus::sumExceptSelf();
}

inline const char* forkCensusOffender()
{
    return ForkCensus::offenderName();
}

inline const volatile void* forkCensusOffenderAddress()
{
    return ForkCensus::offenderAddress();
}

inline void forkCensusResetForChildPromote()
{
    ForkCensus::resetForChildPromote();
}

// Gates the fork-eligibility enforcement in bspForkPoint (counting itself is always on, ~free).
// Disable with --no-fork-census.
inline bool gForkCensus = true;

// Acquire lock, may block
#define ACQUIRE_WITHOUT_DEBUG_LOGGING(lock) \
    do { \
        while (_InterlockedCompareExchange8(&lock, 1, 0)) \
            _mm_pause(); \
        forkCensusEnter(#lock " @ " __FILE__, &lock); \
    } while (0)

#ifdef NDEBUG

// Acquire lock, may block
#define ACQUIRE(lock) ACQUIRE_WITHOUT_DEBUG_LOGGING(lock)

#else

// Emit output if waiting long
class BusyWaitingTracker
{
    unsigned long long mStartTsc;
    unsigned long long mNextReportTscDelta;
    const char* mExpr;
    const char* mFile;
    unsigned int mLine;
    bool mTotalWaitTimeReport;
public:
    BusyWaitingTracker(const char* expr, const char* file, unsigned int line);
    ~BusyWaitingTracker();
    void pause();
};

// Acquire lock, may block and may log if it is blocked for a long time
#define ACQUIRE(lock) \
    do { \
        if (_InterlockedCompareExchange8(&lock, 1, 0)) { \
            BusyWaitingTracker bwt(#lock, __FILE__, __LINE__); \
            while (_InterlockedCompareExchange8(&lock, 1, 0)) \
                bwt.pause(); \
        } \
        forkCensusEnter(#lock " @ " __FILE__, &lock); \
    } while (0)

#endif

// Try to acquire lock and return if successful (without blocking)
#define TRY_ACQUIRE(lock) \
    (_InterlockedCompareExchange8(&lock, 1, 0) == 0 \
        ? (forkCensusEnter(#lock " @ " __FILE__, &lock), true) \
        : false)

// Release lock
#ifdef _MSC_VER
#define RELEASE(lock) \
    do { \
        forkCensusLeave(); \
        lock = 0; \
    } while (0)
#else
#define RELEASE(lock) \
    do { \
        forkCensusLeave(); \
        __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); \
    } while (0)
#endif

// Create an object of this class to lock until the end of the life-time of this object.
// Usually used on stack for making sure that the lock is released, no matter which way the function is left.
struct LockGuard
{
    LockGuard(volatile char& lock) : _lock(lock)
    {
        ACQUIRE(_lock);
    }

    ~LockGuard()
    {
        RELEASE(_lock);
    }

    volatile char& _lock;
};


#ifdef NDEBUG

// Begin waiting loop (with short expected waiting time). Outputs to debug.log if waiting long and NDEBUG isn't defined.
#define BEGIN_WAIT_WHILE(condition) \
    while (condition) {

// End waiting loop, corresponding to BEGIN_WAIT_WHILE().
#define END_WAIT_WHILE() _mm_pause(); }

#else

// Begin waiting loop (with short expected waiting time). Outputs to debug.log if waiting long and NDEBUG isn't defined.
#define BEGIN_WAIT_WHILE(condition) \
    if (condition) { \
        BusyWaitingTracker bwt(#condition, __FILE__, __LINE__); \
        while (condition) {

// End waiting loop, corresponding to BEGIN_WAIT_WHILE().
#define END_WAIT_WHILE() bwt.pause(); } }

#endif


// Waiting loop with short expected waiting time. Outputs to debug.log if waiting long and NDEBUG isn't defined.
#define WAIT_WHILE(condition) \
    BEGIN_WAIT_WHILE(condition) \
    END_WAIT_WHILE()

#define ATOMIC_STORE8(target, val) _InterlockedExchange8(&target, val)
// long in windows is 32bits
#ifdef _MSC_VER
static_assert(sizeof(long) == 4, "Size of long for _InterlockedExchange is 4 bytes");
#define ATOMIC_STORE32(target, val) _InterlockedExchange((volatile long*)&target, val)
#define ATOMIC_LOAD32(target) _InterlockedCompareExchange((volatile long*)&target, 0, 0)
#else
#define ATOMIC_STORE32(target, val) _InterlockedExchange((volatile int*)&target, val)
// A real load, not a CAS: routing this through the _InterlockedCompareExchange shim would issue
// an 8-byte operation on a 4-byte field, since long is 8 bytes here.
#define ATOMIC_LOAD32(target) __atomic_load_n((volatile unsigned int*)&(target), __ATOMIC_SEQ_CST)
#endif
#define ATOMIC_INC64(target) _InterlockedIncrement64(&target)
#define ATOMIC_AND64(target, val) _InterlockedAnd64(&target, val)
#define ATOMIC_STORE64(target, val) _InterlockedExchange64(&target, val)
#define ATOMIC_LOAD64(target) _InterlockedCompareExchange64(&target, 0, 0)
#define ATOMIC_ADD64(target, val) _InterlockedExchangeAdd64(&target, val)
#define ATOMIC_MAX64(target, val) atomicMax64(&target, val)
