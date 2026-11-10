// Implements NO_UEFI versions of several functions that require cstdlib functions.
// Having them in a separate cpp file avoids the name clash between cstdlib's system and Qubic's system.

#include <cstring>
#include <cstdlib>
#include <ctime>
#include <mutex>

#define NO_UEFI

#include "platform/time.h"

#if defined(__linux__) || defined(__APPLE__)
#include <sched.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdint.h>
#include <sys/mman.h>
#include <map>
#else
#include <Windows.h>
#include <conio.h>
#include <map>
#endif

void setMem(void* buffer, unsigned long long size, unsigned char value)
{
    memset(buffer, value, size);
}

void copyMem(void* destination, const void* source, unsigned long long length)
{
    memcpy(destination, source, length);
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

struct VirtualMapping
{
    bool commitMem;
    unsigned long long size;
    bool hugePageHint;
    bool noHugePages;
};
// Never destroyed: pools are still freed by static destructors after a plain global lock/table would be
// gone (macOS rejects a destroyed mutex with EINVAL).
static std::map<unsigned long long, VirtualMapping>& commitMemMap()
{
    static auto* table = new std::map<unsigned long long, VirtualMapping>();
    return *table;
}
static std::mutex& commitMemMapLock()
{
    static auto* lock = new std::mutex();
    return *lock;
}

// Mapping that contains [address, address + size), or end() if none does. Caller holds the lock.
static std::map<unsigned long long, VirtualMapping>::iterator findVirtualMappingLocked(const void* address, const unsigned long long size)
{
    const unsigned long long begin = (unsigned long long)address;
    auto it = commitMemMap().upper_bound(begin);
    if (it == commitMemMap().begin())
    {
        return commitMemMap().end();
    }
    --it;
    if (begin + size > it->first + it->second.size)
    {
        return commitMemMap().end();
    }
    return it;
}

static bool lookupVirtualMapping(const void* address, const unsigned long long size, VirtualMapping& out)
{
    std::lock_guard<std::mutex> lk(commitMemMapLock());
    auto it = findVirtualMappingLocked(address, size);
    if (it == commitMemMap().end())
    {
        return false;
    }
    out = it->second;
    return true;
}

static void registerVirtualMapping(void* address, const unsigned long long size, bool commitMem, bool hugePageHint)
{
    std::lock_guard<std::mutex> lk(commitMemMapLock());
    commitMemMap()[(unsigned long long)address] = { commitMem, size, hugePageHint, false };
}

bool qVirtualContains(const void* address, const unsigned long long size)
{
    VirtualMapping mapping;
    return lookupVirtualMapping(address, size, mapping);
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
        std::lock_guard<std::mutex> lk(commitMemMapLock());
        auto mapping = commitMemMap().find((unsigned long long)buffer);
        if (mapping != commitMemMap().end())
        {
            mappingSize = mapping->second.size;
            commitMemMap().erase(mapping);
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

void updateTime()
{
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
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::gmtime(&t);
    return ms((unsigned char)(tm->tm_year % 100), tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, 0);
}

#ifdef _MSC_VER
void* qVirtualAlloc(const unsigned long long size, bool commitMem = false, bool hugePageHint = true) {
    void *addr = VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | (commitMem ? MEM_COMMIT : 0), PAGE_READWRITE);
    if (addr != nullptr)
    {
        registerVirtualMapping(addr, size, commitMem, hugePageHint);
        return addr;
    }
    printf("CRITIAL: VirtualAlloc failed in qVirtualAlloc");
    return nullptr;
}

void qVirtualAdviseNoHugePages(void* address, const unsigned long long size)
{
    std::lock_guard<std::mutex> lk(commitMemMapLock());
    auto mapping = findVirtualMappingLocked(address, size);
    if (mapping != commitMemMap().end())
    {
        mapping->second.noHugePages = true;
    }
}

void* qVirtualCommit(void* address, const unsigned long long size) {
	return VirtualAlloc(address, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE);
}

unsigned long long qGetPageSize() {
    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    return (unsigned long long)systemInfo.dwPageSize;
}

bool qVirtualFreeAndRecommit(void* address, const unsigned long long size) {
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
#else
static void qVirtualAdviseHugePages(void* address, const unsigned long long size, bool hugePageHint, bool noHugePages)
{
#if defined(__linux__)
    if (noHugePages)
    {
        madvise(address, size, MADV_NOHUGEPAGE);
    }
    else if (hugePageHint)
    {
        madvise(address, size, MADV_HUGEPAGE);
    }
#else
    (void)address; (void)size; (void)hugePageHint; (void)noHugePages;
#endif
}

void* qVirtualAlloc(const unsigned long long size, bool commitMem = false, bool hugePageHint = true) {
    int prot = commitMem ? (PROT_READ | PROT_WRITE) : PROT_NONE;
    void* addr = mmap(nullptr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (addr != MAP_FAILED)
    {
        qVirtualAdviseHugePages(addr, size, hugePageHint, false);
        registerVirtualMapping(addr, size, commitMem, hugePageHint);
        return addr;
    }

    printf("CRITIAL: mmap failed in qVirtualAlloc");
    return nullptr;
}

void qVirtualAdviseNoHugePages(void* address, const unsigned long long size)
{
    std::lock_guard<std::mutex> lk(commitMemMapLock());
    auto mapping = findVirtualMappingLocked(address, size);
    if (mapping != commitMemMap().end())
    {
        mapping->second.noHugePages = true;
        qVirtualAdviseHugePages(address, size, false, true);
    }
}

void* qVirtualCommit(void* address, const unsigned long long size) {
    static long ps = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)address & ~(ps - 1);
    uintptr_t end   = (uintptr_t)address + size;
    size_t aligned_len = end - start;
    aligned_len = (aligned_len + ps - 1) & ~(ps - 1);
    if (mprotect((void*)start, aligned_len, PROT_READ | PROT_WRITE) == 0)
    {
        return address;
    }

    printf("CRITIAL: mprotect failed in qVirtualCommit");
    return nullptr;
}

bool qVirtualFreeAndRecommit(void* address, const unsigned long long size) {
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
    if (mmap(remapStart, remapSize, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) != remapStart)
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

unsigned long long mainThreadProcessorID = 1;