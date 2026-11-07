#pragma once

////////////////// Extensions \\\\\\\\\\\\

#include <cstring>

#if defined(_WIN32)
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <conio.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")   // timeBeginPeriod
#define MSG_DONTWAIT 0
#define MSG_NOSIGNAL 0
#elif defined(__linux__) || defined(__APPLE__)
#include <sched.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/mman.h>
#include <cstddef>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#endif

#include <atomic>
#include <chrono>
#include <new>
#include <memory>

// Fork quiesce: the BSP fork point holds networkingLock across fork() for a consistent map snapshot.
// Per-socket workers are cv-blocked when idle (no busy-spin), so they don't starve the fork.

static volatile bool listOfPeersIsStaticLiteNode = false;

#define ACQUIRE_NO_SPINNING(lock) while (_InterlockedCompareExchange8(&lock, 1, 0)) std::this_thread::sleep_for(std::chrono::milliseconds(1));

#undef CreateEvent
#define CreateEvent CreateEvent
#include "platform/console_logging.h"
#include "extensions/fork_census.h"   // SmartMutex (census-aware networkingLock/eventMapLock)
#include "platform/processor_count.h"

// Use a high-resolution Windows timer for the network retry loops.
#ifdef _MSC_VER
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static inline void preciseSleepMicros(long long microseconds)
{
    if (microseconds <= 0)
    {
        return;
    }

    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer)
    {
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -(microseconds * 10);
        if (SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}
#else
static inline void preciseSleepMicros(long long microseconds)
{
    if (microseconds > 0)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    }
}
#endif

//////////// Custom Data \\\\\\\\\\\

static std::string mySeed = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static m256i mySubseed;
static std::string myOperatorId(60, 0);
static m256i myPublicKey;
static std::string nodeAlias = "My Qubic Lite Node";

// Shared start anchor for uptime reporting on both the HTTP /tick-info
// handler and the P2P RequestLiteCheckin handler. Initialized at static
// init (before main), so it does not depend on which endpoint is hit first.
static const auto liteNodeStartTime = std::chrono::system_clock::now();


// The legacy Windows solution opts out because it does not link the RPC dependencies.
#if defined(__linux__) || defined(__APPLE__) || (defined(_WIN32) && !defined(NO_RPC))
#include <json/config.h>
#include <json/value.h>
#include <json/writer.h>

/////////// Custom Function \\\\\\\\\\\\

std::string getQubicVersionString()
{
    return std::to_string(VERSION_A) + "." +
           std::to_string(VERSION_B) + "." +
           std::to_string(VERSION_C);
}

Json::Value getCheckInData(const std::string& challenge = "")
{
    auto jsonToBytes = [](const Json::Value& json) -> std::vector<unsigned char> {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = ""; // No indentation for compact representation
        std::string jsonString = Json::writeString(writer, json);

        return std::vector<unsigned char>(jsonString.begin(), jsonString.end());
    };

    Json::Value checkinData;
    try
    {
        checkinData["type"] = "lite";
        checkinData["version"] = getQubicVersionString();
        checkinData["alias"] = nodeAlias;
        checkinData["operator"] = myOperatorId;
        // unix timestamp
        checkinData["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch()).count();
        checkinData["uptime"] = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now() - liteNodeStartTime).count();

        if (!challenge.empty())
        {
            checkinData["challenge"] = challenge;
        }

        uint8_t hash[32];
        auto checkinBytes = jsonToBytes(checkinData);
        KangarooTwelve(checkinBytes.data(), checkinBytes.size(), hash, 32);
        uint8_t signature[SIGNATURE_SIZE];
        sign(mySubseed.m256i_u8, myPublicKey.m256i_u8, hash, signature);

        checkinData["signature"] = byteToHex(signature, SIGNATURE_SIZE);
        checkinData["messageHex"] = byteToHex(checkinBytes.data(), checkinBytes.size());
    } catch (const std::exception& e)
    {
    }

    return checkinData;
}
#endif

static volatile bool forceDontCheckComputerDigest = false;

//////////// Go Behind Testnet Trick \\\\\\\\

static inline bool isTestnetGoBehindTrick = false;

//////////// Tick Delay Feature \\\\\\\\\\\\\

static inline unsigned long long tickDelay = 0;

// Wall-clock ms per tick, 0 = unpaced; see doc/long_run_local_testnet.md
#ifdef LONG_RUN_LOCAL_TESTNET
static inline unsigned long long tickDurationMs = 1000;
#else
static inline unsigned long long tickDurationMs = 0;
#endif

//////////// HTTP Server Port \\\\\\\\\\\\

static inline int httpPort = 41841;

// Security-tick skip removed: every tick verifies state (cheap with the K12 state-digest cache).
// A single tick can still be suppressed by the catch-up override.
bool isSystemAtSecurityTick()
{
    return !forceDontCheckComputerDigest;
}

bool isNextTickIsSecurityTick()
{
    return true;
}

uint32_t getCurrentCpuIndex() {
#if defined(_WIN32)
    return GetCurrentProcessorNumber();
#elif defined(__linux__)
    return static_cast<uint32_t>(sched_getcpu());
#else
    return 0; // not supported
#endif
}

#ifndef _MSC_VER

#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close

void setNonBlockingInput(bool enable) {
    static termios oldt;
    termios newt;

    if (enable) {
        // Save old settings
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;

        // Disable canonical mode and echo
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

        // Set stdin non-blocking
        fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    } else {
        // Restore old settings
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        fcntl(STDIN_FILENO, F_SETFL, 0);
    }
}

std::vector<unsigned char> readInput() {
    std::vector<unsigned char> buffer;
    unsigned char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        buffer.push_back(c);
    }
    return buffer;
}
#endif

struct VirtualMapping
{
    bool commitMem;
    unsigned long long size;
    // Explicit callers ask for huge pages (cheaper fork); demand-zero pools keep the system default.
    bool hugePageHint;
    // Scatter-access pools opt out of huge pages so a single written byte does not pin 2 MiB.
    bool noHugePages;
};
inline std::map<unsigned long long, VirtualMapping> commitMemMap;
// Autosave allocates on the main thread while the tick thread zeroes pools, so the table is locked.
inline SmartMutex commitMemMapLock{ "commitMemMapLock" };

// Mapping that contains [address, address + size), or end() if none does. Caller holds the lock.
inline std::map<unsigned long long, VirtualMapping>::iterator findVirtualMappingLocked(const void* address, const unsigned long long size)
{
    const unsigned long long begin = (unsigned long long)address;
    auto it = commitMemMap.upper_bound(begin);
    if (it == commitMemMap.begin())
    {
        return commitMemMap.end();
    }
    --it;
    if (begin + size > it->first + it->second.size)
    {
        return commitMemMap.end();
    }
    return it;
}

inline bool lookupVirtualMapping(const void* address, const unsigned long long size, VirtualMapping& out)
{
    std::lock_guard<SmartMutex> lk(commitMemMapLock);
    auto it = findVirtualMappingLocked(address, size);
    if (it == commitMemMap.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

inline void registerVirtualMapping(void* address, const unsigned long long size, bool commitMem, bool hugePageHint)
{
    std::lock_guard<SmartMutex> lk(commitMemMapLock);
    commitMemMap[(unsigned long long)address] = { commitMem, size, hugePageHint, false };
}

inline bool qVirtualContains(const void* address, const unsigned long long size)
{
    VirtualMapping mapping;
    return lookupVirtualMapping(address, size, mapping);
}

#ifdef _MSC_VER
inline void* qVirtualAlloc(const unsigned long long size, bool commitMem = false, bool hugePageHint) {
    void *addr = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | (commitMem ? MEM_COMMIT : 0), PAGE_READWRITE);
    if (addr != nullptr)
    {
        registerVirtualMapping(addr, size, commitMem, hugePageHint);
        return addr;
    }
    logToConsole(L"CRITIAL: VirtualAlloc failed in qVirtualAlloc");
    return nullptr;
}

inline void qVirtualAdviseNoHugePages(void* address, const unsigned long long size)
{
    std::lock_guard<SmartMutex> lk(commitMemMapLock);
    auto mapping = findVirtualMappingLocked(address, size);
    if (mapping != commitMemMap.end())
    {
        mapping->second.noHugePages = true;
    }
}

inline void* qVirtualCommit(void* address, const unsigned long long size) {
	return VirtualAlloc(address, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE);
}

inline unsigned long long qGetPageSize() {
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    return (unsigned long long)systemInfo.dwPageSize;
}

inline bool qVirtualFreeAndRecommit(void* address, const unsigned long long size) {
    static const unsigned long long pageSize = qGetPageSize();
    VirtualMapping mapping;
    if (!lookupVirtualMapping(address, size, mapping))
    {
        return false;
    }
    const bool commitMem = mapping.commitMem;

    // MEM_DECOMMIT works on whole pages, so a range that starts or ends mid-page would also drop
    // whatever shares those pages; zero the head and tail fragments in place instead.
    char* const rangeEnd = (char*)address + size;
    char* const head = (char*)address;
    char* const decommitStart = (char*)(((uintptr_t)head + pageSize - 1) & ~(pageSize - 1));
    char* const decommitEnd = (char*)((uintptr_t)rangeEnd & ~(pageSize - 1));
    if (decommitEnd <= decommitStart)
    {
        memset(head, 0, (size_t)size);
        return true;
    }
    memset(head, 0, (size_t)(decommitStart - head));

    const unsigned long long decommitSize = decommitEnd - decommitStart;
    VirtualFree(decommitStart, (SIZE_T)decommitSize, MEM_DECOMMIT);
    if (commitMem && VirtualAlloc(decommitStart, (SIZE_T)decommitSize, MEM_COMMIT, PAGE_READWRITE) != decommitStart)
    {
        return false;
    }

    const unsigned long long tailSize = rangeEnd - decommitEnd;
    if (tailSize)
    {
        if (!VirtualAlloc(decommitEnd, (SIZE_T)tailSize, MEM_COMMIT, PAGE_READWRITE))
        {
            return false;
        }
        memset(decommitEnd, 0, (size_t)tailSize);
    }

    return true;
}

// Emulate demand-zero overcommit by committing Windows pages on first access.
struct LazyCommitRegion
{
    uintptr_t base;
    uintptr_t end;
};
inline LazyCommitRegion g_lazyCommitRegions[4096 * 2];
inline volatile long g_lazyCommitRegionCount = 0;
inline volatile long g_lazyCommitVehInstalled = 0;
inline unsigned long g_lazyCommitPageSize = 4096;

inline bool inLazyCommitRegion(uintptr_t address)
{
    const long regionCount = g_lazyCommitRegionCount;
    for (long i = 0; i < regionCount; i++)
    {
        if (address >= g_lazyCommitRegions[i].base && address < g_lazyCommitRegions[i].end)
        {
            return true;
        }
    }
    return false;
}

// Commit reserved pages on first access and let genuine allocation failures propagate.
static LONG WINAPI lazyCommitVeh(EXCEPTION_POINTERS* exception)
{
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const uintptr_t faultAddress = (uintptr_t)exception->ExceptionRecord->ExceptionInformation[1];
    if (!inLazyCommitRegion(faultAddress))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const uintptr_t page = faultAddress & ~((uintptr_t)g_lazyCommitPageSize - 1);
    if (VirtualAlloc((void*)page, g_lazyCommitPageSize, MEM_COMMIT, PAGE_READWRITE))
    {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Use lazy allocation only for pages written from user mode, not kernel I/O buffers.
inline void* qVirtualAllocLazy(const unsigned long long size)
{
    if (!_InterlockedCompareExchange(&g_lazyCommitVehInstalled, 1, 0))
    {
        SYSTEM_INFO systemInfo;
        GetSystemInfo(&systemInfo);
        g_lazyCommitPageSize = systemInfo.dwPageSize ? systemInfo.dwPageSize : 4096;
        AddVectoredExceptionHandler(1 /*first*/, lazyCommitVeh);
    }

    void* address = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_READWRITE);
    if (!address)
    {
        logToConsole(L"CRITIAL: VirtualAlloc(MEM_RESERVE) failed in qVirtualAllocLazy");
        return nullptr;
    }

    const long regionIndex = g_lazyCommitRegionCount;
    if (regionIndex < (long)(sizeof(g_lazyCommitRegions) / sizeof(g_lazyCommitRegions[0])))
    {
        g_lazyCommitRegions[regionIndex].base = (uintptr_t)address;
        g_lazyCommitRegions[regionIndex].end = (uintptr_t)address + size;
        _ReadWriteBarrier();
        g_lazyCommitRegionCount = regionIndex + 1;
    }
    else
    {
        VirtualAlloc(address, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE);
    }

    registerVirtualMapping(address, size, true, true);
    return address;
}
#else
inline void qVirtualAdviseHugePages(void* address, const unsigned long long size, bool hugePageHint, bool noHugePages)
{
    // THP hint: fewer PTEs -> cheaper fork() of the large-RSS node. Unprivileged; no-op if THP off.
#if defined(__linux__)
    if (noHugePages)
    {
        madvise(address, size, MADV_NOHUGEPAGE);   // Linux-only (absent on macOS/Darwin)
    }
    else if (hugePageHint)
    {
        madvise(address, size, MADV_HUGEPAGE);
    }
#else
    (void)address; (void)size; (void)hugePageHint; (void)noHugePages;
#endif
}

inline void* qVirtualAlloc(const unsigned long long size, bool commitMem = false, bool hugePageHint) {
    int prot = commitMem ? (PROT_READ | PROT_WRITE) : PROT_NONE;
    void* addr = mmap(nullptr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr != MAP_FAILED)
    {
        qVirtualAdviseHugePages(addr, size, hugePageHint, false);
        registerVirtualMapping(addr, size, commitMem, hugePageHint);
        return addr;
    }

    logToConsole(L"CRITIAL: mmap failed in qVirtualAlloc");
    return nullptr;
}

inline void qVirtualAdviseNoHugePages(void* address, const unsigned long long size)
{
    std::lock_guard<SmartMutex> lk(commitMemMapLock);
    auto mapping = findVirtualMappingLocked(address, size);
    if (mapping != commitMemMap.end())
    {
        mapping->second.noHugePages = true;
        qVirtualAdviseHugePages(address, size, false, true);
    }
}

inline void* qVirtualCommit(void* address, const unsigned long long size) {
    static long ps = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)address & ~(ps - 1);
    uintptr_t end   = (uintptr_t)address + size;
    size_t aligned_len = end - start;
    aligned_len = (aligned_len + ps - 1) & ~(ps - 1);
    if (mprotect((void*)start, aligned_len, PROT_READ | PROT_WRITE) == 0)
    {
        return address;
    }

    logToConsole(L"CRITIAL: mprotect failed in qVirtualCommit");
    return nullptr;
}

inline bool qVirtualFreeAndRecommit(void* address, const unsigned long long size) {
    static const unsigned long long pageSize = (unsigned long long)sysconf(_SC_PAGESIZE);
    VirtualMapping mapping;
    if (!lookupVirtualMapping(address, size, mapping))
    {
        return false;
    }
    const bool commitMem = mapping.commitMem;
    const int prot = commitMem ? (PROT_READ | PROT_WRITE) : PROT_NONE;

    // MAP_FIXED works on whole pages, so a range that starts or ends mid-page would also wipe
    // whatever shares those pages; zero the head and tail fragments in place instead.
    char* const rangeEnd = (char*)address + size;
    char* const head = (char*)address;
    char* const remapStart = (char*)(((uintptr_t)head + pageSize - 1) & ~(pageSize - 1));
    char* const remapEnd = (char*)((uintptr_t)rangeEnd & ~(pageSize - 1));
    if (remapEnd <= remapStart)
    {
        memset(head, 0, size);
        return true;
    }
    memset(head, 0, remapStart - head);

    const unsigned long long remapSize = remapEnd - remapStart;
    if (mmap(remapStart, remapSize, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != remapStart)
    {
        return false;
    }
    // The fresh mapping carries no advice; restoring it keeps adjacent VMAs mergeable.
    qVirtualAdviseHugePages(remapStart, remapSize, mapping.hugePageHint, mapping.noHugePages);

    const unsigned long long tailSize = rangeEnd - remapEnd;
    if (tailSize)
    {
        if (mprotect(remapEnd, tailSize, PROT_READ | PROT_WRITE) != 0)
        {
            return false;
        }
        memset(remapEnd, 0, tailSize);
    }

    return true;
}

#endif

inline void freePoolOrVirtual(void* pointer)
{
    freePool(pointer);
}

void updateTime() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::gmtime(&t);
    utcTime.Year = tm->tm_year + 1900;
    utcTime.Month = tm->tm_mon + 1;
    utcTime.Day = tm->tm_mday;
    utcTime.Hour = tm->tm_hour;
    utcTime.Minute = tm->tm_min;
    utcTime.Second = tm->tm_sec;
    utcTime.Nanosecond = 0;
    utcTime.TimeZone = 0;
    utcTime.Daylight = 0;
}

unsigned long long now_ms()
{
    return ms((unsigned char)utcTime.Year % 100, utcTime.Month, utcTime.Day, utcTime.Hour, utcTime.Minute, utcTime.Second, utcTime.Nanosecond / 1000000);
}

void setMem(void* buffer, unsigned long long size, unsigned char value)
{
    memset(buffer, value, size);
}

void copyMem(void* destination, const void* source, unsigned long long length)
{
    // Must match UEFI EFI_BOOT_SERVICES.CopyMem semantics, which explicitly support
    // overlapping source/destination. The original (bare-metal) code relies on this —
    // e.g. processReceivedData() compacts its receive buffer in place with overlapping
    // ranges. memcpy() is UB on overlap (glibc corrupts large copies), which left zeroed
    // bytes mid-stream and made the parser read size()==0 and force-forget valid peers.
    memmove(destination, source, length);
}

bool allocatePool(unsigned long long size, void** buffer)
{
    void* ptr = malloc(size);
    if (ptr)
    {
        *buffer = ptr;
        return true;
    }
    return false;
}

// Pools come from either the heap or qVirtualAlloc; the mapping table tells which.
void freePool(void* buffer)
{
    if (!buffer)
    {
        return;
    }
    unsigned long long mappingSize = 0;
    {
        std::lock_guard<SmartMutex> lk(commitMemMapLock);
        auto mapping = commitMemMap.find((unsigned long long)buffer);
        if (mapping != commitMemMap.end())
        {
            mappingSize = mapping->second.size;
            commitMemMap.erase(mapping);
        }
    }
    if (mappingSize)
    {
#ifdef _MSC_VER
        VirtualFree(buffer, 0, MEM_RELEASE);
#else
        munmap(buffer, mappingSize);
#endif
        return;
    }
    free(buffer);
}

inline void closeEvent(EFI_EVENT Event)
{
    bs->CloseEvent(Event);
}

inline EFI_STATUS createEvent(unsigned int Type, EFI_TPL NotifyTpl, void* NotifyFunction, void* NotifyContext, EFI_EVENT* Event)
{
    return bs->CreateEvent(Type, NotifyTpl, NotifyFunction, NotifyContext, Event);
}

enum class ConnectStatus
{
    Connected,
    Connecting,
    Disconnected,
    Error
};

struct Overload {

    struct PerSocketIo {
        std::mutex mtx;   // SMARTMUTEX-EXEMPT: per-socket IO lock, dropped via tcpDataMap.clear() in resetForChildPromote; never held over node state
        std::condition_variable cv;
        EFI_TCP4_IO_TOKEN* pendingToken = nullptr;
        bool hasPending = false;
        SOCKET socket = INVALID_SOCKET;
        std::atomic<bool> stop{false};
    };
    // recvWorkerLoop poll slice: bounds the stop re-check after DestroyChild.
    static constexpr int RECV_POLL_SLICE_MS = 10;

    struct TcpData {
        EFI_TCP4_CONFIG_DATA configData;
        SOCKET socket;
        bool isOutgoing;
        volatile char receiveLock;
        volatile char sendLock;
        ConnectStatus connectStatus;
        std::shared_ptr<PerSocketIo> sendIo;
        std::shared_ptr<PerSocketIo> recvIo;
    };

    struct EventData {
        EFI_EVENT event;
        void* context;
        void* notifyFunction;
    };

    inline static std::vector<std::thread> threads;
    inline static std::unordered_map<unsigned long long, SOCKET> incomingSocketMap;
    // shared_ptr: detached connect/accept threads capture the element by value so it survives a
    // concurrent DestroyChild() erase (was a use-after-free on epoch-end teardown).
    inline static std::unordered_map<unsigned long long, std::shared_ptr<TcpData>> tcpDataMap;
    inline static std::unordered_map<unsigned long long, EventData> eventDataMap;
    inline static std::unordered_map<unsigned long long, bool> isReceiveThreadSetupMap;
    inline static std::unordered_map<unsigned long long, bool> isSendThreadSetupMap;

    inline static SmartMutex networkingLock{ "networkingLock" };   // census-aware: a non-AP holder at fork trips the gate
    inline static SmartMutex eventMapLock{ "eventMapLock" };       // guards eventDataMap (CreateEvent/CloseEvent on main vs callback lookup on AP worker threads)

    // Only the calling thread survives fork. Keep the inherited listening socket, but drop
    // per-peer sockets and stale worker references so reconnects spawn fresh workers lazily.
    static void resetForChildPromote()
    {
        const unsigned long long listenKey = (unsigned long long)peerTcp4Protocol;
        std::shared_ptr<TcpData> listenData;
        if (auto it = tcpDataMap.find(listenKey); it != tcpDataMap.end())
            listenData = it->second;

        for (auto &kv : tcpDataMap)
            if (kv.first != listenKey && kv.second && kv.second->socket != INVALID_SOCKET)
                closesocket(kv.second->socket);
        for (auto &kv : incomingSocketMap)
            if (kv.second != INVALID_SOCKET)
                closesocket(kv.second);
        tcpDataMap.clear();
        incomingSocketMap.clear();
        eventDataMap.clear();
        isReceiveThreadSetupMap.clear();
        isSendThreadSetupMap.clear();
        // The kept listen socket's inherited per-socket worker refs are stale (threads gone); null
        // them so the next op lazy-spawns fresh workers.
        if (listenData)
        {
            listenData->sendIo.reset();
            listenData->recvIo.reset();
            tcpDataMap.emplace(listenKey, listenData);
        }

        new (&networkingLock) SmartMutex("networkingLock");
        new (&eventMapLock) SmartMutex("eventMapLock");
    }

    // Stop a socket's workers; each worker's shared_ptr keeps its state alive until exit.
    static void signalPerSocketWorkers(TcpData& tcpData)
    {
        auto signalWorker = [](std::shared_ptr<PerSocketIo>& worker)
        {
            if (!worker)
                return;

            {
                std::lock_guard<std::mutex> lock(worker->mtx);
                worker->stop.store(true, std::memory_order_release);
            }
            worker->cv.notify_all();
        };
        signalWorker(tcpData.sendIo);
        signalWorker(tcpData.recvIo);
    }

#ifndef _MSC_VER
    // std::thread cannot size its stack, and the linker flag that gives the Linux build 8 MB has no ld64
    // equivalent reaching a secondary thread — Darwin hands one 512 KB. processTick's own frame is about
    // 144 KB before the Wasm interpreter runs on top of it, so the size is asked for outright.
    static constexpr size_t PROCESSOR_THREAD_STACK_SIZE = 8u * 1024u * 1024u;

    struct ProcessorThreadArgs
    {
        void* data;
        unsigned long long processorNumber;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    static void* processorThreadEntry(void* argument) {
        ProcessorThreadArgs* args = (ProcessorThreadArgs*)argument;
        CustomStack* me = reinterpret_cast<CustomStack*>(args->data);
        me->setupFuncToCall(me->setupDataToPass, args->processorNumber);
        args->finished->store(true);
        delete args;
        return nullptr;
    }
#endif

    // Directly call the setup function without using custom stack.
    static void startThread(EFI_AP_PROCEDURE procedure, void* data, unsigned long long ProcessorNumber, EFI_EVENT WaitEvent, unsigned long long TimeoutInMicroseconds) {
        (void)procedure;
        // Shared with the thread because the timeout path below detaches and then reads this flag.
        auto isThreadFinished = std::make_shared<std::atomic<bool>>(false);

        #ifdef _MSC_VER
        std::thread thread([isThreadFinished, data, ProcessorNumber]() {
            CustomStack* me = reinterpret_cast<CustomStack*>(data);
            me->setupFuncToCall(me->setupDataToPass, ProcessorNumber);
            isThreadFinished->store(true);
            });
        HANDLE hThread = (HANDLE)thread.native_handle();
        SetThreadAffinityMask(hThread, 1ULL << ProcessorNumber);
        #else
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, PROCESSOR_THREAD_STACK_SIZE);

        pthread_t thread{};
        ProcessorThreadArgs* args = new ProcessorThreadArgs{ data, ProcessorNumber, isThreadFinished };
        const int startError = pthread_create(&thread, &attributes, processorThreadEntry, args);
        pthread_attr_destroy(&attributes);
        if (startError != 0) {
            delete args;
            logToConsole(L"Error calling pthread_create");
            return;
        }
        #ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(ProcessorNumber, &cpuset);
        int rc = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            logToConsole(L"Error calling pthread_setaffinity_np");
        }
        #endif // macOS: no cpu affinity API (scheduler handles placement)
        #endif

        if (TimeoutInMicroseconds > 0) {
            #ifdef _MSC_VER
            thread.detach();
            #else
            pthread_detach(thread);
            #endif
        }
        else {
            #ifdef _MSC_VER
            thread.join(); // Wait for the thread to finish if no timeout is specified
            #else
            pthread_join(thread, nullptr);
            #endif
            isThreadFinished->store(true); // Mark the thread as finished
        }

        if (TimeoutInMicroseconds > 0) {
            while (!isThreadFinished->load() && TimeoutInMicroseconds > 0) {
                // Sleep for a short duration to avoid busy waiting
                preciseSleepMicros(100);
                TimeoutInMicroseconds -= 100;
            }

            if (!isThreadFinished->load()) {
                #ifdef _MSC_VER
                TerminateThread(hThread, 0); // Forcefully terminate the thread if it doesn't finish
                #else
                pthread_cancel(thread);
                #endif
            }
        }

        // call the event call back
        if (WaitEvent) {
            void* notifyFn = nullptr;
            void* ctx = nullptr;
            bool found = false;
            {
                // Copy under lock, call the callback outside it (the callback may re-enter Create/CloseEvent).
                std::lock_guard<SmartMutex> lk(eventMapLock);
                auto it = eventDataMap.find((unsigned long long)WaitEvent);
                if (it != eventDataMap.end()) {
                    notifyFn = it->second.notifyFunction;
                    ctx = it->second.context;
                    found = true;
                }
            }
            if (found) {
                if (notifyFn) {
                    void (*notifyFunction)(void*, void*) = reinterpret_cast<void (*)(void*, void*)>(notifyFn);
                    notifyFunction(WaitEvent, ctx);
                }
            }
            else {
                logToConsole(L"Event callback not found for the given event.");
            }
        }
    }

    ////////////// RuntimeServices Implementation //////////////

    static EFI_STATUS GetTime(OUT EFI_TIME* Time, OUT EFI_TIME_CAPABILITIES* Capabilities OPTIONAL) {
        logToConsole(L"GetTime IS NOT IMPLEMENTED");
        return EFI_UNSUPPORTED;
    }

    static EFI_STATUS SetTime(IN EFI_TIME* Time) {
        if (Time == nullptr) {
            return EFI_INVALID_PARAMETER;
        }
        utcTime = *Time;
        return EFI_SUCCESS;
    }

    ////////////// BootServices Implementation //////////////

    static EFI_STATUS Stall(IN unsigned long long Microseconds) {
        // Simulate a stall by doing nothing for the specified time
        preciseSleepMicros((long long)Microseconds);
        return EFI_SUCCESS;
    }

    static EFI_STATUS SetWatchdogTimer(IN unsigned long long Timeout, IN unsigned long long WatchdogCode, IN unsigned long long DataSize, IN CHAR16* WatchdogData OPTIONAL) {
        //logToConsole(L"SetWatchdogTimer IS NOT IMPLEMENTED");
        return EFI_UNSUPPORTED;
    }

    static EFI_STATUS CloseProtocol(IN EFI_HANDLE Handle, IN EFI_GUID* Protocol, IN EFI_HANDLE AgentHandle, IN EFI_HANDLE ControllerHandle) {
        return EFI_SUCCESS;
    }

    static EFI_STATUS LocateProtocol(IN EFI_GUID* Protocol, IN void* Registration OPTIONAL, OUT void** Interface) {
        EFI_GUID mpServiceProtocolGuid = EFI_MP_SERVICES_PROTOCOL_GUID;
        if (memcmp(Protocol, &(tcp4ServiceBindingProtocolGuid), sizeof(EFI_GUID)) == 0) {
            ////// TCP4 Service Binding Protocol Implementation ///////
            *Interface = new EFI_SERVICE_BINDING_PROTOCOL;
            tcp4ServiceBindingProtocol->CreateChild = Overload::CreateChild;
            tcp4ServiceBindingProtocol->DestroyChild = Overload::DestroyChild;
            return EFI_SUCCESS;
        }
        else if (memcmp(Protocol, &(mpServiceProtocolGuid), sizeof(EFI_GUID)) == 0) {
            ///// MP Services Protocol Implementation /////
            *Interface = new EFI_MP_SERVICES_PROTOCOL;
            mpServicesProtocol->GetNumberOfProcessors = Overload::GetNumberOfProcessors;
            mpServicesProtocol->WhoAmI = Overload::WhoAmI;
            mpServicesProtocol->GetProcessorInfo = Overload::GetProcessorInfo;
            mpServicesProtocol->StartupThisAP = Overload::StartupThisAP;
            return EFI_SUCCESS;
        }

        logToConsole(L"LocateProtocol IS NOT IMPLEMENTED");
        return EFI_UNSUPPORTED;
    }

    static EFI_STATUS WaitForEvent(IN unsigned long long NumberOfEvents, IN EFI_EVENT* Event, OUT unsigned long long* Index) {
        logToConsole(L"WaitForEvent IS NOT IMPLEMENTED");
        return EFI_UNSUPPORTED;
    }

    static EFI_STATUS OpenProtocol(IN EFI_HANDLE Handle, IN EFI_GUID* Protocol, OUT void** Interface OPTIONAL, IN EFI_HANDLE AgentHandle, IN EFI_HANDLE ControllerHandle, IN unsigned int Attributes) {
        std::lock_guard<SmartMutex> lock(networkingLock);

        if (Handle == nullptr || Protocol == nullptr || Interface == nullptr) {
            return EFI_INVALID_PARAMETER;
        }

        if (memcmp(Protocol, &tcp4ProtocolGuid, sizeof(EFI_GUID)) == 0) {
            *Interface = new EFI_TCP4_PROTOCOL;
            // Check if this is a incomming socket and set socket instance if it is
            if (incomingSocketMap.contains((unsigned long long)Handle)) {
                tcpDataMap[(unsigned long long) * Interface] = std::make_shared<TcpData>();
                TcpData& tcpData = *tcpDataMap[(unsigned long long) * Interface];
                tcpData.socket = incomingSocketMap[(unsigned long long)Handle];

                incomingSocketMap.erase((unsigned long long)Handle);
            }

            // Map handle to the tcp4Protocol so we can get tcp4Protocol from the handle
            *(unsigned long long*)Handle = (unsigned long long) * Interface;

            EFI_TCP4_PROTOCOL* tcp4Protocol = reinterpret_cast<EFI_TCP4_PROTOCOL*>(*Interface);
            tcp4Protocol->GetModeData = Overload::GetModeData;
            tcp4Protocol->Poll = Overload::Poll;
            tcp4Protocol->Transmit = Overload::Transmit;
            tcp4Protocol->Receive = Overload::Receive;
            tcp4Protocol->Close = Overload::Close;
            tcp4Protocol->Cancel = Overload::Cancel;
            tcp4Protocol->Configure = Overload::Configure;
            tcp4Protocol->Accept = Overload::Accept;
            tcp4Protocol->Connect = Overload::Connect;
            tcp4Protocol->Routes = Overload::Routes;
            return EFI_SUCCESS;
        }

        logToConsole(L"OpenProtocol IS NOT IMPLEMENTED");
        return EFI_UNSUPPORTED;
    }

    static EFI_STATUS LocateHandleBuffer(IN EFI_LOCATE_SEARCH_TYPE SearchType, IN EFI_GUID* Protocol OPTIONAL, IN void* SearchKey OPTIONAL, OUT unsigned long long* NoHandles, OUT EFI_HANDLE** Buffer) {
        logToConsole(L"LocateHandleBuffer IS NOT IMPLEMENTED");
        return EFI_UNSUPPORTED;
    }

    static EFI_STATUS CreateEvent(IN unsigned int Type, IN EFI_TPL NotifyTpl, IN void* NotifyFunction, OPTIONAL IN void* NotifyContext, OPTIONAL OUT EFI_EVENT* Event) {
        if (Type == EVT_NOTIFY_SIGNAL && (NotifyTpl == TPL_CALLBACK || NotifyTpl == TPL_NOTIFY)) {
            *Event = new unsigned long long;
            EventData eventData;
            eventData.event = *Event;
            eventData.context = NotifyContext;
            eventData.notifyFunction = NotifyFunction;
            {
                std::lock_guard<SmartMutex> lk(eventMapLock);
                eventDataMap[(unsigned long long) * Event] = eventData;
            }
            return EFI_SUCCESS;
        }

        // used for case bs->CreateEvent(0, TPL_CALLBACK, NULL, NULL, &closeToken.CompletionToken.Event); in closePeer()
        if (Type == 0)
        {
            *Event = new unsigned long long;
            EventData eventData;
            eventData.event = *Event;
            eventData.context = NotifyContext;
            eventData.notifyFunction = NotifyFunction;
            {
                std::lock_guard<SmartMutex> lk(eventMapLock);
                eventDataMap[(unsigned long long)*Event] = eventData;
            }
            return EFI_SUCCESS;
        }

        logToConsole(L"Create Event IS NOT IMPLEMENTED");
        return EFI_UNSUPPORTED;
    }

    static EFI_STATUS CloseEvent(IN EFI_EVENT Event) {
        std::lock_guard<SmartMutex> lk(eventMapLock);
        auto it = eventDataMap.find((unsigned long long)Event);
        if (it != eventDataMap.end()) {
            delete (unsigned long long*)Event; // Free the allocated memory for the event
            eventDataMap.erase(it); // Remove from the map
            return EFI_SUCCESS;
        }

        logToConsole(L"No event found in map");
        return EFI_NOT_FOUND;
    }

    static EFI_STATUS CheckEvent(IN EFI_EVENT Event) {
        return EFI_SUCCESS;
    }

    ////////////// SystemTable Implementation //////////////

    static EFI_STATUS ClearScreen(IN void* This) {
        return EFI_SUCCESS;
    }

    static EFI_STATUS ReadKeyStroke(IN void* This, OUT EFI_INPUT_KEY* Key) {
        Key->ScanCode = 0;
        Key->UnicodeChar = 0;
#ifdef _MSC_VER
        if (_kbhit()) {               // check if key was pressed
            int ch = _getch();        // now it's safe to read
            if (ch == 27) {
                Key->ScanCode = 0x17;
            };

            if (ch == 0 || ch == 224) {
                int code = _getch();

                // check f2->f12
                switch (code) {
                case 60:  Key->ScanCode = 0x0C; break;
                case 61:  Key->ScanCode = 0x0D; break;
                case 62:  Key->ScanCode = 0x0E; break;
                case 63:  Key->ScanCode = 0x0F; break;
                case 64:  Key->ScanCode = 0x10; break;
                case 65:  Key->ScanCode = 0x11; break;
                case 66:  Key->ScanCode = 0x12; break;
                case 67:  Key->ScanCode = 0x13; break;
                case 68:  Key->ScanCode = 0x14; break;
                case 133: Key->ScanCode = 0x15; break;
                case 134: Key->ScanCode = 0x16; break;
                }
            }
            else {
                if (ch == 'p') {
                    Key->ScanCode = 0x48;
                }
            }

            return EFI_SUCCESS;
        }
#else
        static std::map<std::vector<unsigned char>, std::string> keyMap = {
            {{27,79,80}, "F1"}, {{27,79,81}, "F2"},
            {{27,79,82}, "F3"}, {{27,79,83}, "F4"},
            {{27,91,49,53,126}, "F5"}, {{27,91,49,55,126}, "F6"},
            {{27,91,49,56,126}, "F7"}, {{27,91,49,57,126}, "F8"},
            {{27,91,50,48,126}, "F9"}, {{27,91,50,49,126}, "F10"},
            {{27,91,50,51,126}, "F11"}, {{27,91,50,52,126}, "F12"}
        };

        std::vector<unsigned char> input = readInput();
        if (!input.empty()) {
            // Try to match against known sequences
            if (keyMap.count(input)) {
                // Map f2->f12 to EFI_INPUT_KEY
                std::string keyName = keyMap[input];

                if (keyName == "F2")  Key->ScanCode = 0x0C;
                else if (keyName == "F3")  Key->ScanCode = 0x0D;
                else if (keyName == "F4")  Key->ScanCode = 0x0E;
                else if (keyName == "F5")  Key->ScanCode = 0x0F;
                else if (keyName == "F6")  Key->ScanCode = 0x10;
                else if (keyName == "F7")  Key->ScanCode = 0x11;
                else if (keyName == "F8")  Key->ScanCode = 0x12;
                else if (keyName == "F9")  Key->ScanCode = 0x13;
                else if (keyName == "F10") Key->ScanCode = 0x14;
                else if (keyName == "F11") Key->ScanCode = 0x15;
                else if (keyName == "F12") Key->ScanCode = 0x16;
            } else {
                if (input.size() == 1)
                {
                    // map 'p' to fake pause key
                    if (input[0] == 'p')
                    {
                        Key->ScanCode = 0x48;
                    } else if (input[0] == 27) {
                        Key->ScanCode = 0x17;
                    } else {
                        Key->UnicodeChar = input[0];
                    }
                }
            }

            return EFI_SUCCESS;
        }
#endif

        return EFI_NOT_READY;
    }

    ////////////// MP Services Protocol Implementation //////////////

    static EFI_STATUS GetNumberOfProcessors(IN void* This, OUT unsigned long long* NumberOfProcessors, OUT unsigned long long* NumberOfEnabledProcessors) {
        // Counted before the main thread pinned itself — hardware_concurrency() answers 1 after that on
        // builds whose libc reads the affinity mask.
        *NumberOfProcessors = (unsigned long long)totalProcessorCount();
        *NumberOfEnabledProcessors = *NumberOfProcessors; // Assume all processors are enabled
        return EFI_SUCCESS;
    }

    static EFI_STATUS WhoAmI(IN void* This, OUT unsigned long long* ProcessorNumber) {
        *ProcessorNumber = getCurrentCpuIndex();
        return EFI_SUCCESS;
    }

    static EFI_STATUS GetProcessorInfo(IN void* This, IN unsigned long long ProcessorNumber, OUT EFI_PROCESSOR_INFORMATION* ProcessorInfoBuffer) {
        ProcessorInfoBuffer->StatusFlag = PROCESSOR_ENABLED_BIT | PROCESSOR_HEALTH_STATUS_BIT; // Assume the processor is enabled and healthy
        ProcessorInfoBuffer->Location = { 0, 0 }; // Location is not used in this implementation
        ProcessorInfoBuffer->ProcessorId = ProcessorNumber; // Use the processor number as the ID
        return EFI_SUCCESS;
    }

    // Using the custom stack here breaks the runtime, which expects the OS-provided one — startThread asks
    // the OS for a stack large enough instead, so the custom stack can be bypassed.
    static EFI_STATUS StartupThisAP(IN void* This, IN EFI_AP_PROCEDURE Procedure, IN unsigned long long ProcessorNumber, IN EFI_EVENT WaitEvent OPTIONAL, IN unsigned long long TimeoutInMicroseconds, IN void* ProcedureArgument OPTIONAL, OUT BOOLEAN* Finished OPTIONAL) {
        std::thread thread(startThread, Procedure, ProcedureArgument, ProcessorNumber, WaitEvent, TimeoutInMicroseconds);
        thread.detach();
        return EFI_SUCCESS;
    }

    ////////////// TCP4 Service Binding Protocol Implementation //////////////

    static EFI_STATUS CreateChild(IN void* This, OUT EFI_HANDLE* ChildHandle) {
        // Preserve 8 bytes to hold the address of tcp4Protocol
        void* _8Bytes = new unsigned long long;
        *ChildHandle = _8Bytes;
        return EFI_SUCCESS;
    }

    static EFI_STATUS DestroyChild(IN void* This, IN EFI_HANDLE ChildHandle)
    {
        const unsigned long long key = *(unsigned long long*)ChildHandle;
        if (tcpDataMap.contains(key))
        {
            TcpData& tcpData = *tcpDataMap[key];
            signalPerSocketWorkers(tcpData);
            if (tcpData.socket != INVALID_SOCKET)
            {
                closesocket(tcpData.socket);
                tcpData.socket = INVALID_SOCKET;
            }
            tcpDataMap.erase(key);
            isReceiveThreadSetupMap.erase(key);
            isSendThreadSetupMap.erase(key);
        }
        freePool(ChildHandle);
        return EFI_SUCCESS;
    }

    ////////////// TCP4 Protocol Implementation //////////////

    static EFI_STATUS Routes(IN void* This, IN BOOLEAN DeleteRoute, IN EFI_IPv4_ADDRESS* SubnetAddress, IN EFI_IPv4_ADDRESS* SubnetMask, IN EFI_IPv4_ADDRESS* GatewayAddress) {
        logToConsole(L"Routes IS NOT IMPLEMENTED");
        return EFI_UNSUPPORTED;
    }

    static EFI_STATUS Close(IN void* This, IN EFI_TCP4_CLOSE_TOKEN* CloseToken) {
        return EFI_SUCCESS;
    }

    static EFI_STATUS Cancel(IN void* This, IN EFI_TCP4_COMPLETION_TOKEN* Token OPTIONAL) {
        logToConsole(L"Cancel IS NOT IMPLEMENTED");
        return EFI_UNSUPPORTED;
    }

    static EFI_STATUS Poll(IN void* This) {
        return EFI_SUCCESS;
    }

    static EFI_STATUS Transmit(IN void* This, IN EFI_TCP4_IO_TOKEN* Token) {
        std::shared_ptr<TcpData> tcpData;
        unsigned long long key = (unsigned long long)This;
        if (tcpDataMap.contains(key)) {
            tcpData = tcpDataMap[key];
        }
        else {
            logToConsole(L"No Tcp Data For This Connect!");
            return EFI_UNSUPPORTED;
        }

        if (tcpData->socket == INVALID_SOCKET) {
            logToConsole(L"No Available Socket Connect!");
            return EFI_UNSUPPORTED;
        }

        if (tcpData->connectStatus == ConnectStatus::Disconnected || tcpData->connectStatus == ConnectStatus::Error) {
            Token->CompletionToken.Status = EFI_ABORTED;
            return EFI_ABORTED;
        }

        // Lazy-spawn one dedicated send worker per socket (only the main thread calls Transmit per peer).
        if (!tcpData->sendIo) {
            tcpData->sendIo = std::make_shared<PerSocketIo>();
            tcpData->sendIo->socket = tcpData->socket;
            auto io = tcpData->sendIo;
            std::thread([io]() { sendWorkerLoop(io); }).detach();
        }

        {
            std::lock_guard<std::mutex> lk(tcpData->sendIo->mtx);
            tcpData->sendIo->pendingToken = Token;
            tcpData->sendIo->hasPending = true;
        }
        tcpData->sendIo->cv.notify_one();

        return EFI_SUCCESS;
    }

    static EFI_STATUS Receive(IN void* This, IN EFI_TCP4_IO_TOKEN* Token) {
        std::shared_ptr<TcpData> tcpData;
        unsigned long long key = (unsigned long long)This;
        if (tcpDataMap.contains(key)) {
            tcpData = tcpDataMap[key];
        }
        else {
            logToConsole(L"No Tcp Data For This Connect!");
            return EFI_ABORTED;
        }

        if (tcpData->socket == INVALID_SOCKET) {
            logToConsole(L"No Available Socket Connect!");
            return EFI_ABORTED;
        }

        if (tcpData->connectStatus == ConnectStatus::Disconnected) {
            Token->CompletionToken.Status = EFI_ABORTED;
            return EFI_CONNECTION_FIN;
        } else if (tcpData->connectStatus == ConnectStatus::Error) {
            Token->CompletionToken.Status = EFI_ABORTED;
            return EFI_ABORTED;
        }

        if (!tcpData->recvIo) {
            tcpData->recvIo = std::make_shared<PerSocketIo>();
            tcpData->recvIo->socket = tcpData->socket;
            auto io = tcpData->recvIo;
            std::thread([io]() { recvWorkerLoop(io); }).detach();
        }

        {
            std::lock_guard<std::mutex> lk(tcpData->recvIo->mtx);
            tcpData->recvIo->pendingToken = Token;
            tcpData->recvIo->hasPending = true;
        }
        tcpData->recvIo->cv.notify_one();

        return EFI_SUCCESS;
    }

    static EFI_STATUS GetModeData(IN void* This, OUT EFI_TCP4_CONNECTION_STATE* Tcp4State OPTIONAL, OUT EFI_TCP4_CONFIG_DATA* Tcp4ConfigData OPTIONAL, OUT EFI_IP4_MODE_DATA* Ip4ModeData OPTIONAL, OUT EFI_MANAGED_NETWORK_CONFIG_DATA* MnpConfigData OPTIONAL, OUT EFI_SIMPLE_NETWORK_MODE* SnpModeData OPTIONAL) {
        if (Ip4ModeData) {
            Ip4ModeData->IsConfigured = true;
            Ip4ModeData->IsStarted = true;
            Ip4ModeData->RouteCount = 0;
            Ip4ModeData->RouteTable = new EFI_IP4_ROUTE_TABLE;
            memset(Ip4ModeData->RouteTable->GatewayAddress.Addr, 0, sizeof(IPv4Address));
            memset(Ip4ModeData->RouteTable->SubnetMask.Addr, 0, sizeof(IPv4Address));
            memset(Ip4ModeData->RouteTable->SubnetAddress.Addr, 0, sizeof(IPv4Address));
        }

        if (Tcp4State) {
			if (tcpDataMap.contains((unsigned long long)This)) {
				TcpData& tcpData = *tcpDataMap[(unsigned long long)This];
				if (tcpData.socket == INVALID_SOCKET) {
					*Tcp4State = Tcp4StateClosed;
				}
				else {
#ifdef _MSC_VER
                    WSAPOLLFD pfd{};
                    pfd.fd = tcpData.socket;
                    pfd.events = POLLIN | POLLERR | POLLHUP;
                    int ret = WSAPoll(&pfd, 1, 0);
#else
				    pollfd pfd{};
				    pfd.fd = tcpData.socket;
				    pfd.events = POLLIN | POLLERR | POLLHUP;
				    int ret = poll(&pfd, 1, 0);
#endif
                    // On POLLHUP with data still readable, stay Established so recv() drains it first (clean close on EOF).
                    const bool closed = (pfd.revents & POLLERR) || ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN));
                    if (ret > 0 && closed) {
                        *Tcp4State = Tcp4StateClosed;
                        tcpData.connectStatus = ConnectStatus::Error;
                    }
                    else
                    {
                        *Tcp4State = Tcp4StateEstablished;
                    }
				}
			}
			else {
				*Tcp4State = Tcp4StateClosed;
			}
        }

        if (Tcp4ConfigData) {
        }

        if (MnpConfigData) {
            logToConsole(L"GetModeData for MnpConfigData is not implemented");
            return EFI_ABORTED;
        }

        if (SnpModeData) {
            logToConsole(L"GetModeData for SnpModeData is not implemented");
            return EFI_ABORTED;
        }

        return EFI_SUCCESS;
    }

    static EFI_STATUS Configure(IN void* This, IN EFI_TCP4_CONFIG_DATA* TcpConfigData OPTIONAL) {
        static bool isGlobalSocketInitialized = false;
        if (!TcpConfigData) {
            // Teardown via Configure(NULL): shutdown socket so in-flight send/recv abort and
            // isTransmitting can clear (else slot stuck isClosing). fd closed in DestroyChild.
            auto it = tcpDataMap.find((unsigned long long)This);
            if (it != tcpDataMap.end() && it->second) {
                it->second->connectStatus = ConnectStatus::Disconnected;
                if (it->second->socket != INVALID_SOCKET) {
#ifdef _MSC_VER
                    shutdown(it->second->socket, SD_BOTH);
#else
                    shutdown(it->second->socket, SHUT_RDWR);
#endif
                }
            }
            return EFI_SUCCESS;
        }

        TcpData data;
        data.configData = *TcpConfigData;
        data.isOutgoing = *((unsigned int*)TcpConfigData->AccessPoint.RemoteAddress.Addr) == 0;
        data.socket = INVALID_SOCKET;
        data.receiveLock = 0;
        data.sendLock = 0;
        data.connectStatus = ConnectStatus::Disconnected;
        // Global set up for accepting new connections
        if ((unsigned long long)This == (unsigned long long)peerTcp4Protocol && !isGlobalSocketInitialized) {
            #ifdef _MSC_VER
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                logToConsole(L"WSAStartup failed!!");
                return EFI_ABORTED;
            }
            #endif

            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) {
                logToConsole(L"Socket creation failed!!");
                return EFI_ABORTED;
            }

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(TcpConfigData->AccessPoint.StationPort);
            addr.sin_addr.s_addr = INADDR_ANY;

            int opt = 1;
            int buf_size = 16 * 1024 * 1024; // 16MB
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(buf_size));
            setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&buf_size, sizeof(buf_size));
            if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
                logToConsole(L"Failed to bind socket!");
                closesocket(sock);

                return EFI_ABORTED;
            }

            if (listen(sock, SOMAXCONN) == SOCKET_ERROR) {
                logToConsole(L"Failed to listen socket!");
                closesocket(sock);

                return EFI_ABORTED;
            }

            logToConsole(L"Socket binded");
            data.socket = sock;
			isGlobalSocketInitialized = true;
        }

        unsigned long long key = (unsigned long long)This;
        if (tcpDataMap.contains(key))
            return EFI_ACCESS_DENIED;

        tcpDataMap.emplace(key, std::make_shared<TcpData>(data));
        return EFI_SUCCESS;
    }

    // Note: Only global tcp4Protocol call this function, peers don't call
    static EFI_STATUS Accept(IN void* This, IN EFI_TCP4_LISTEN_TOKEN* ListenToken, IN void* peer) {
        std::shared_ptr<TcpData> tcpData;
        unsigned long long key = (unsigned long long)This;
        if (tcpDataMap.contains(key)) {
            tcpData = tcpDataMap[key];
        }
        else {
            logToConsole(L"No Tcp Data For Global Tcp Connect!");
            return EFI_UNSUPPORTED;
        }

        // accept in a thread
        std::thread acceptThread([tcpData, ListenToken, peer]() {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(tcpData->configData.AccessPoint.StationPort);
            addr.sin_addr.s_addr = INADDR_ANY;
            #ifdef _MSC_VER
            int addrlen = sizeof(addr);
            #else
            socklen_t addrlen = sizeof(addr);
            #endif
            tcpData->connectStatus = ConnectStatus::Connecting;
            SOCKET clientSocket = accept(tcpData->socket, (sockaddr*)&addr, &addrlen);

            int buf_size = 16 * 1024 * 1024; // 16MB
            setsockopt(clientSocket, SOL_SOCKET, SO_RCVBUF, (char*)&buf_size, sizeof(buf_size));
            setsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, (char*)&buf_size, sizeof(buf_size));
#ifdef _MSC_VER
            // Disable Nagle for small per-vote messages on Windows loopback.
            int nodelay = 1;
            setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
#endif

            bool isLocal = false;
            if (peer)
            {
                // get ipv4 of the client
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(addr.sin_addr), ipStr, INET_ADDRSTRLEN);
                IPv4Address ip;
                ip.fromString(ipStr);
                ((Peer*)peer)->address = ip;
                isLocal = (ip == IPv4Address::getLocalIp());
            }

            if (listOfPeersIsStaticLiteNode && !isLocal)
            {
                logToConsole(L"Static network mode, rejected a incomming connection");
                ListenToken->CompletionToken.Status = EFI_ABORTED;
                return;
            }
            if (clientSocket == INVALID_SOCKET) {
                logToConsole(L"Obtained tcpData failed");
                ListenToken->CompletionToken.Status = EFI_ABORTED;
                return;
            }

#ifdef _MSC_VER
            u_long mode = 1;
            ioctlsocket(clientSocket, FIONBIO, &mode);
#endif

            CreateChild(NULL, &ListenToken->NewChildHandle);
            // Save the socket until peerConnectionNewlyEstablished initializes the protocol.
            {
                std::lock_guard<SmartMutex> lock(networkingLock);
                incomingSocketMap[(unsigned long long)ListenToken->NewChildHandle] = clientSocket;
            }
            ListenToken->CompletionToken.Status = EFI_SUCCESS;
            tcpData->connectStatus = ConnectStatus::Connected;
            });
        acceptThread.detach();
        return EFI_SUCCESS;
    }

    static EFI_STATUS Connect(IN void* This, IN EFI_TCP4_CONNECTION_TOKEN* ConnectionToken) {
        std::shared_ptr<TcpData> tcpData;
        unsigned long long key = (unsigned long long)This;
        if (tcpDataMap.contains(key)) {
            tcpData = tcpDataMap[key];
        }
        else {
            logToConsole(L"No Tcp Data For This Connect!");
            return EFI_UNSUPPORTED;
        }
        // Non-blocking socket so connect() returns immediately (EINPROGRESS) and we can bound the wait.
#ifdef _MSC_VER
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock != INVALID_SOCKET) { u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb); }
#else
        // SOCK_NONBLOCK is a Linux extension; set the flag separately so BSD/macOS builds too.
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock != INVALID_SOCKET) { fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK); }
#endif
        if (sock == INVALID_SOCKET) {
            logToConsole(L"Socket creation failed!!");

            return EFI_ABORTED;
        }
        tcpData->socket = sock;

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(tcpData->configData.AccessPoint.RemotePort);
        #ifdef _MSC_VER
        serverAddr.sin_addr.S_un.S_addr = *((unsigned long*)tcpData->configData.AccessPoint.RemoteAddress.Addr);
        #else
        serverAddr.sin_addr.s_addr = *((unsigned int*)tcpData->configData.AccessPoint.RemoteAddress.Addr);
        #endif

        // 4s-bounded connect (< 5s reaper so it self-resolves first): a dead peer can't pin the slot
        // for the OS SYN timeout (~127s); Configure(NULL)'s shutdown wakes the poll early.
        std::thread connectThread([tcpData, serverAddr, ConnectionToken, sock]() {
            tcpData->connectStatus = ConnectStatus::Connecting;
            bool ok = false;
            if (connect(sock, (const sockaddr*)&serverAddr, sizeof(serverAddr)) == 0) {
                ok = true;
            } else {
#ifdef _MSC_VER
                bool inProgress = (WSAGetLastError() == WSAEWOULDBLOCK);
                WSAPOLLFD pfd{}; pfd.fd = sock; pfd.events = POLLOUT;
                int pr = inProgress ? WSAPoll(&pfd, 1, 4000) : -1;
#else
                bool inProgress = (errno == EINPROGRESS);
                pollfd pfd{}; pfd.fd = sock; pfd.events = POLLOUT;
                int pr = inProgress ? poll(&pfd, 1, 4000) : -1;
#endif
                if (pr > 0 && (pfd.revents & POLLOUT) && !(pfd.revents & (POLLERR | POLLHUP))) {
                    int soerr = 0; socklen_t len = sizeof(soerr);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
                    ok = (soerr == 0);
                }
            }
            if (ok) {
                tcpData->connectStatus = ConnectStatus::Connected;
                ConnectionToken->CompletionToken.Status = EFI_SUCCESS;
#ifdef _MSC_VER
                int nodelay = 1;
                setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelay, sizeof(nodelay));
#endif
            } else {
                tcpData->connectStatus = ConnectStatus::Error;
                ConnectionToken->CompletionToken.Status = EFI_ABORTED;
            }
            });
        connectThread.detach();

        return EFI_SUCCESS;
    }

    // One dedicated send worker per socket. Consumes hasPending under mtx at start of work,
    // does the send unlocked, then writes Token Status last as the completion signal to main.
    // Main never sees Status != -1 until the worker has finished touching pendingToken/Token.
    static void sendWorkerLoop(std::shared_ptr<PerSocketIo> io)
    {
        while (!io->stop.load(std::memory_order_acquire))
        {
            EFI_TCP4_IO_TOKEN* token = nullptr;
            {
                std::unique_lock<std::mutex> lk(io->mtx);
                io->cv.wait(lk, [&] {
                    return io->hasPending || io->stop.load(std::memory_order_acquire);
                });
                if (io->stop.load(std::memory_order_acquire)) return;
                token = io->pendingToken;
                io->hasPending = false;
            }

            int totalSentBytes = 0;
            auto& fragment = token->Packet.TxData->FragmentTable[0];
            EFI_STATUS finalStatus = EFI_SUCCESS;
            bool abandoned = false;
            // Abort a send only after 5s of zero progress (not total time) so big transfers aren't cut mid-stream.
            constexpr unsigned long long NO_PROGRESS_TIMEOUT_NS = 5'000'000'000ULL;
            auto lastProgress = std::chrono::high_resolution_clock::now();
            while ((unsigned int)totalSentBytes < fragment.FragmentLength)
            {
                // Stop (DestroyChild/reconfigure): abandon WITHOUT touching token — the socket is being
                // torn down and the peer slot's token may already be re-armed for a new connection.
                if (io->stop.load(std::memory_order_acquire)) { abandoned = true; break; }
                auto now = std::chrono::high_resolution_clock::now();
                if ((unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastProgress).count() > NO_PROGRESS_TIMEOUT_NS) {
                    finalStatus = EFI_TIMEOUT;
                    break;
                }
                auto sentBytes = send(io->socket, (const char*)fragment.FragmentBuffer + totalSentBytes, fragment.FragmentLength - totalSentBytes, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (sentBytes > 0)
                {
                    totalSentBytes += sentBytes;
                    lastProgress = now;
                }
                else if (sentBytes == 0)
                {
                    finalStatus = EFI_ABORTED;
                    break;
                }
                else if (sentBytes == SOCKET_ERROR)
                {
#ifdef _MSC_VER
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK)
                    {
                        preciseSleepMicros(1000);
                        continue;
                    }
                    logToConsole(L"Closed a transmit socket");
                    finalStatus = EFI_ABORTED;
                    break;
#else
                    if (errno == EWOULDBLOCK || errno == EAGAIN)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    logToConsole(L"Closed a transmit socket");
                    finalStatus = EFI_ABORTED;
                    break;
#endif
                }
            }

            if (!abandoned)
            {
                if ((unsigned int)totalSentBytes >= fragment.FragmentLength)
                    finalStatus = EFI_SUCCESS;
                // Status write MUST be the last touch on `token` for this op — main may
                // immediately reuse the Token / FragmentBuffer once it sees Status != -1.
                token->CompletionToken.Status = finalStatus;
            }
        }
    }

    static void recvWorkerLoop(std::shared_ptr<PerSocketIo> io)
    {
        while (!io->stop.load(std::memory_order_acquire))
        {
            EFI_TCP4_IO_TOKEN* token = nullptr;
            {
                std::unique_lock<std::mutex> lk(io->mtx);
                io->cv.wait(lk, [&] {
                    return io->hasPending || io->stop.load(std::memory_order_acquire);
                });
                if (io->stop.load(std::memory_order_acquire)) return;
                token = io->pendingToken;
                io->hasPending = false;
            }

            // Read at most the free space the caller reserved (FragmentLength), NOT BUFFER_SIZE:
            // BUFFER_SIZE overruns receiveBuffer when it holds a partial message, clobbering the
            // adjacent peer's buffer (parses as malformed -> force-forgotten).
            const unsigned int maxReceiveSize = (unsigned int)token->Packet.RxData->FragmentTable[0].FragmentLength;
            char* dst = (char*)token->Packet.RxData->FragmentTable[0].FragmentBuffer;

            // Wait for readability instead of completing on EAGAIN (that made main re-arm every pass).
            // Woken by data, FIN/RST, or the shutdown() in Configure(NULL); stop checked before each recv.
            long long receivedBytes = SOCKET_ERROR;
            for (;;)
            {
                if (io->stop.load(std::memory_order_acquire)) break;
                receivedBytes = recv(io->socket, dst, maxReceiveSize, MSG_DONTWAIT);
                if (receivedBytes >= 0) break;
#ifdef _MSC_VER
                if (WSAGetLastError() != WSAEWOULDBLOCK) break;
#else
                if (errno != EWOULDBLOCK && errno != EAGAIN) break;
#endif
                if (io->stop.load(std::memory_order_acquire)) break;
#ifdef _MSC_VER
                WSAPOLLFD pfd{};
                pfd.fd = io->socket;
                pfd.events = POLLIN;
                WSAPoll(&pfd, 1, RECV_POLL_SLICE_MS);
#else
                pollfd pfd{};
                pfd.fd = io->socket;
                pfd.events = POLLIN;
                poll(&pfd, 1, RECV_POLL_SLICE_MS);
#endif
            }

            EFI_STATUS finalStatus;
            unsigned int dataLen = 0;
            if (receivedBytes > 0)
            {
                dataLen = (unsigned int)receivedBytes;
                finalStatus = EFI_SUCCESS;
            }
            else
            {
                // 0 = peer closed, <0 = error; EAGAIN only exits the loop via stop (guarded below).
                finalStatus = EFI_ABORTED;
            }
            // Stop (DestroyChild/reconfigure): abandon WITHOUT touching token — the slot may be re-armed.
            if (io->stop.load(std::memory_order_acquire)) continue;
            token->Packet.RxData->DataLength = dataLen;
            // Publish DataLength before Status: the main loop reads Status first, then DataLength;
            // without this barrier it can see SUCCESS with a stale buffer-sized DataLength.
            std::atomic_thread_fence(std::memory_order_release);
            // Status write MUST be the last touch on `token` for this op.
            token->CompletionToken.Status = finalStatus;
        }
    }

    static void initializeUefi() {
        const unsigned int hwConcurrency = totalProcessorCount();
        const unsigned int lastCpu = hwConcurrency > 0 ? hwConcurrency - 1 : 0;
        #ifndef _MSC_VER
        setNonBlockingInput(true);
        #if defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(lastCpu, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        #endif // macOS: no cpu affinity API
        #else
		// NOTE: In MSVC Release Mode, so the scheduler often just keeps the main thread on one CPU core (the best core), dont need to set affinity because it will slow down the main thread performance
        HANDLE hThread = GetCurrentThread();
        SetThreadAffinityMask(hThread, 1ULL << lastCpu);
        // Keep one-millisecond network retries precise on Windows.
        timeBeginPeriod(1);
        // Disable background throttling that overrides the requested timer resolution.
        PROCESS_POWER_THROTTLING_STATE pt;
        memset(&pt, 0, sizeof(pt));
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.StateMask = 0;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
        pt.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));

        #endif

        ih = new EFI_HANDLE;
        st = new EFI_SYSTEM_TABLE;
        st->BootServices = new EFI_BOOT_SERVICES;
        st->RuntimeServices = new EFI_RUNTIME_SERVICES;
        st->ConOut = new EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
        st->ConIn = new EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
        bs = st->BootServices;
        rs = st->RuntimeServices;

        ////// RuntimeServices Implementation ///////
        rs->GetTime = Overload::GetTime;
        rs->SetTime = Overload::SetTime;

        ////// BootServices Implementation ///////
        bs->SetMem = setMem;
        bs->Stall = Overload::Stall;
        bs->SetWatchdogTimer = Overload::SetWatchdogTimer;
        bs->LocateProtocol = Overload::LocateProtocol;
        bs->CloseProtocol = Overload::CloseProtocol;
        bs->OpenProtocol = Overload::OpenProtocol;
        bs->WaitForEvent = Overload::WaitForEvent;
        bs->LocateHandleBuffer = Overload::LocateHandleBuffer;
        bs->CreateEvent = Overload::CreateEvent;
        bs->CloseEvent = Overload::CloseEvent;
        bs->CheckEvent = Overload::CheckEvent;

        ///// SystemTable Implementation /////
        st->ConOut->ClearScreen = Overload::ClearScreen;
        st->ConIn->ReadKeyStroke = Overload::ReadKeyStroke;

        // Per-socket send/recv worker threads are spawned lazily on first Transmit/Receive call.

        // Reserve space
        incomingSocketMap.reserve(1024);
    }
};

void logToConsole_1(const CHAR16* message);
#define logToConsole logToConsole_1
