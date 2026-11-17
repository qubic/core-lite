#pragma once

#include "console_logging.h"
#include "pointer_align.h"
#include <lib/platform_efi/uefi.h>
#include "memory.h"

#include <cstddef>
#include <cstdint>

#ifdef NO_UEFI

#include <cstdlib>
#include <cstdbool>
#include <cstdio>

inline void* qVirtualAlloc(const unsigned long long size, bool commitMem, bool hugePageHint = true);
inline void* qVirtualCommit(void* address, const unsigned long long size);
inline bool qVirtualFreeAndRecommit(void* address, const unsigned long long size);
// True if [address, address + size) lies inside one qVirtualAlloc mapping.
inline bool qVirtualContains(const void* address, const unsigned long long size);
// Opt a mapping out of transparent huge pages (Linux); no-op elsewhere.
inline void qVirtualAdviseNoHugePages(void* address, const unsigned long long size);
#if defined(__linux__)
inline void* qVirtualAllocFileBacked(const unsigned long long size);
#endif
#ifdef _MSC_VER
// Reserve memory and commit pages on first touch.
inline void* qVirtualAllocLazy(const unsigned long long size);
#endif

// Pools this large are mostly untouched in practice. An anonymous mapping is zero-filled on demand,
// so unlike a memset heap block it costs RSS only where something writes.
static constexpr unsigned long long DEMAND_ZERO_POOL_THRESHOLD = 64ULL << 20;
// Heap tolerates a short over-read past the end; a mapping faults, so leave room.
static constexpr unsigned long long DEMAND_ZERO_POOL_SLACK = 64ULL << 10;

// useVirtualMem indicates whether to use VirtualAlloc or malloc
// commitMem indicates whether to commit memory when using VirtualAlloc
// NOTE: commitMem only used if host machine have enough RAM+Pagefile, otherwise VirtualAlloc will fail
// lazyCommit is only safe for buffers written from user mode.
static bool allocPoolWithErrorLog(const wchar_t* name, const unsigned long long size, void** buffer, const int LINE, bool useVirtualMem = false,
    bool commitMem = false, bool lazyCommit = false)
{
    static unsigned long long totalMemoryUsed = 0;
    static unsigned long long totalVirtualMemoryUsed = 0;
    size_t padded_size = (size + 64 - 1) & ~(64 - 1);
    unsigned long long mappingSize = size;
    // A demand-zero pool keeps the system's huge-page default, as its heap block did.
    bool hugePageHint = true;
#if defined(__linux__)
    if (!useVirtualMem && padded_size >= DEMAND_ZERO_POOL_THRESHOLD)
    {
        useVirtualMem = true;
        commitMem = true;
        hugePageHint = false;
        padded_size += DEMAND_ZERO_POOL_SLACK;
        mappingSize = padded_size;
    }
#endif
    if (useVirtualMem) {
#ifdef _MSC_VER
		*buffer = lazyCommit ? qVirtualAllocLazy(mappingSize) : qVirtualAlloc(mappingSize, commitMem, hugePageHint);
#else
		(void)lazyCommit;
		*buffer = qVirtualAlloc(mappingSize, commitMem, hugePageHint);
#endif
    }
    else {
#if defined(__linux__) || defined(__APPLE__)
        *buffer = std::aligned_alloc(64, padded_size);
#else
		*buffer = _aligned_malloc(padded_size, 64);
#endif
    }

    if (*buffer == nullptr)
    {
        printf("Memory allocation failed for %ls on line %u\n", name, LINE);
        return false;
    }

    // Zero out allocated memory
    if(!useVirtualMem)
     setMem(*buffer, padded_size, 0);

    if (useVirtualMem) {
        totalVirtualMemoryUsed += size;
    }
    totalMemoryUsed += padded_size;
    // setText(message, L"Memory allocated ");
    // appendNumber(message, size / 1048576, TRUE);
    // appendText(message, L" MiB for ");
    // appendText(message, name);
    // appendText(message, L"| Total memory used: ");
    // appendNumber(message, totalMemoryUsed / 1048576, TRUE);
    // appendText(message, L" | ");
    // appendNumber(message, totalVirtualMemoryUsed / 1048576, TRUE);
    // appendText(message, L" MiB.");
    // logToConsole(message);
    return true;
}

// For pools written at scattered offsets: with huge pages, each written byte would pin 2 MiB of a
// demand-zero mapping, so these opt out.
static bool allocSparsePoolWithErrorLog(const wchar_t* name, const unsigned long long size, void** buffer, const int LINE)
{
    if (!allocPoolWithErrorLog(name, size, buffer, LINE))
    {
        return false;
    }
    qVirtualAdviseNoHugePages(*buffer, size);
    return true;
}

// For append-only pools that outgrow RAM: the kernel can evict file pages, not anonymous ones.
static bool allocFileBackedPoolWithErrorLog(const wchar_t* name, const unsigned long long size, void** buffer, const int LINE)
{
// A forced epoch switch can reset the colony inside a fork window, which a shared pool cannot undo.
#if defined(__linux__) && defined(USE_SWAP) && !defined(LONG_RUN_LOCAL_TESTNET)
    *buffer = qVirtualAllocFileBacked(size + DEMAND_ZERO_POOL_SLACK);
    CHAR16 line[256];
    setText(line, *buffer != nullptr ? L"File-backed pool: " : L"No file-backed pool, using anonymous memory: ");
    appendText(line, name);
    logToConsole(line);
    if (*buffer != nullptr)
    {
        return true;
    }
#endif
    return allocPoolWithErrorLog(name, size, buffer, LINE);
}

// Zero a range of a pool. Inside a demand-zero mapping the pages go back to the kernel instead of
// being written, so the range stops costing RSS; anything else is memset.
static void zeroPool(void* buffer, const unsigned long long size)
{
    if (qVirtualFreeAndRecommit(buffer, size))
    {
        return;
    }
    setMem(buffer, size, 0);
}

#else

static bool allocPoolWithErrorLog(const CHAR16* name, const unsigned long long size, void** buffer, const int LINE, bool needZerOut = true)
    {
    EFI_STATUS status;
    CHAR16 message[512];
    constexpr EFI_MEMORY_TYPE poolType = EfiRuntimeServicesData;

    // Check for invalid input
    if (buffer == nullptr || size == 0) {
        logStatusAndMemInfoToConsole(L"Invalid buffer pointer or size", EFI_INVALID_PARAMETER, __LINE__, size);
        return false;
    }

    status = bs->AllocatePool(poolType, size, buffer);
    if (status != EFI_SUCCESS)
    {
        setText(message, L"EFI_BOOT_SERVICES.AllocatePool() fails for ");
        appendText(message, name);
        appendText(message, L" with size ");
        appendNumber(message, size, TRUE);
        logStatusAndMemInfoToConsole(message, status, LINE, size);
        return false;
    }
#ifndef NDEBUG
    else {
        setText(message, L"EFI_BOOT_SERVICES.AllocatePool() completed for ");
        appendText(message, name);
        appendText(message, L" with size ");
        appendNumber(message, size, TRUE);
        logStatusAndMemInfoToConsole(message, status, LINE, size);
            }
#endif
    // Zero out allocated memory
    if (*buffer != nullptr && needZerOut) {
        setMem(*buffer, size, 0);
    }
    return true;
}

static bool allocSparsePoolWithErrorLog(const CHAR16* name, const unsigned long long size, void** buffer, const int LINE)
{
    return allocPoolWithErrorLog(name, size, buffer, LINE);
}

static bool allocFileBackedPoolWithErrorLog(const CHAR16* name, const unsigned long long size, void** buffer, const int LINE)
{
    return allocPoolWithErrorLog(name, size, buffer, LINE);
}

static void zeroPool(void* buffer, const unsigned long long size)
{
    setMem(buffer, size, 0);
}

#endif
