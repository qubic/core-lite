#pragma once

#define _GNU_SOURCE
#include "contract_core/contract_def.h"
#include "extensions/utils.h"
#include "userfaultfd.h"
#include <K12/kangaroo_twelve_xkcp.h>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/userfaultfd.h>
#include <list>
#include <mutex>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <functional>

#ifdef LITE_ENGINE_DEBUG
extern std::function<void()> engineCustomeActionCallback;
#endif

class K12Engine
{
protected:
    struct Intermediate
    {
        unsigned char intermediate[maxCapacityInBytes];
    };

    // map from chunk index to intermediate state
    std::vector<Intermediate> intermediateMap;
    // map from chunk index to isChanged flag
    std::vector<bool> isChunkChangedMap;
    unsigned int maxChunks;
    unsigned char *_state;
    size_t _stateSize;

    unsigned char *_lastOutput;
    size_t _lastOutputSize;

    int _KangarooTwelve_Update(XKCP::KangarooTwelve_Instance *ktInstance, const unsigned char *input, size_t inputByteLen, bool useCache)
    {
        if (ktInstance->phase != XKCP::ABSORBING)
            return 1;

        if (ktInstance->blockNumber == 0)
        {
            /* First block, absorb in final node */
            unsigned int len = (inputByteLen < (K12_chunkSize - ktInstance->queueAbsorbedLen)) ? (unsigned int)inputByteLen : (K12_chunkSize - ktInstance->queueAbsorbedLen);
            XKCP::TurboSHAKE_Absorb(&ktInstance->finalNode, input, len);
            input += len;
            inputByteLen -= len;
            ktInstance->queueAbsorbedLen += len;
            if ((ktInstance->queueAbsorbedLen == K12_chunkSize) && (inputByteLen != 0))
            {
                /* First block complete and more input data available, finalize it */
                const unsigned char padding = 0x03; /* '110^6': message hop, simple padding */
                ktInstance->queueAbsorbedLen = 0;
                ktInstance->blockNumber = 1;
                XKCP::TurboSHAKE_Absorb(&ktInstance->finalNode, &padding, 1);
                ktInstance->finalNode.byteIOIndex = (ktInstance->finalNode.byteIOIndex + 7) & ~7; /* Zero padding up to 64 bits */
            }
        }
        else if (ktInstance->queueAbsorbedLen != 0)
        {
            /* There is data in the queue, absorb further in queue until block complete */
            unsigned int len = (inputByteLen < (K12_chunkSize - ktInstance->queueAbsorbedLen)) ? (unsigned int)inputByteLen : (K12_chunkSize - ktInstance->queueAbsorbedLen);
            XKCP::TurboSHAKE_Absorb(&ktInstance->queueNode, input, len);
            input += len;
            inputByteLen -= len;
            ktInstance->queueAbsorbedLen += len;
            if (ktInstance->queueAbsorbedLen == K12_chunkSize)
            {
                int capacityInBytes = 2 * (ktInstance->securityLevel) / 8;
                unsigned char intermediate[maxCapacityInBytes];
                // assert(capacityInBytes <= maxCapacityInBytes);
                ktInstance->queueAbsorbedLen = 0;
                ++ktInstance->blockNumber;
                XKCP::TurboSHAKE_AbsorbDomainSeparationByte(&ktInstance->queueNode, K12_suffixLeaf);
                XKCP::TurboSHAKE_Squeeze(&ktInstance->queueNode, intermediate, capacityInBytes);
                XKCP::TurboSHAKE_Absorb(&ktInstance->finalNode, intermediate, capacityInBytes);
            }
        }

        while (inputByteLen > 0)
        {
            int capacityInBytes = 2 * (ktInstance->securityLevel) / 8;
            unsigned int len = (inputByteLen < K12_chunkSize) ? (unsigned int)inputByteLen : K12_chunkSize;
            unsigned int chunkIndex = ktInstance->blockNumber;

            if (!isChunkChangedMap[chunkIndex] && useCache)
            {
                if (len == K12_chunkSize)
                {
                    unsigned char *intermediate = intermediateMap[chunkIndex].intermediate;
                    XKCP::TurboSHAKE_Absorb(&ktInstance->finalNode, intermediate, capacityInBytes);
                    input += len;
                    inputByteLen -= len;
                    ++ktInstance->blockNumber;
                    continue;
                }
            }

            XKCP::TurboSHAKE_Initialize(&ktInstance->queueNode, ktInstance->securityLevel);
            XKCP::TurboSHAKE_Absorb(&ktInstance->queueNode, input, len);
            input += len;
            inputByteLen -= len;
            if (len == K12_chunkSize)
            {
                unsigned char intermediate[maxCapacityInBytes];
                // assert(capacityInBytes <= maxCapacityInBytes);
                ++ktInstance->blockNumber;
                XKCP::TurboSHAKE_AbsorbDomainSeparationByte(&ktInstance->queueNode, K12_suffixLeaf);
                XKCP::TurboSHAKE_Squeeze(&ktInstance->queueNode, intermediate, capacityInBytes);
                XKCP::TurboSHAKE_Absorb(&ktInstance->finalNode, intermediate, capacityInBytes);

                // cache the intermediate state
                Intermediate &inter = intermediateMap[chunkIndex];
                std::memcpy(inter.intermediate, intermediate, capacityInBytes);
                isChunkChangedMap[chunkIndex] = false;
            }
            else
            {
                ktInstance->queueAbsorbedLen = len;
            }
        }

        return 0;
    }

public:
    K12Engine(unsigned char *state, size_t stateSize)
    {
        _state = state;
        _stateSize = stateSize;
        maxChunks = (stateSize + K12_chunkSize - 1) / K12_chunkSize;

        isChunkChangedMap.resize(maxChunks);
        intermediateMap.resize(maxChunks);
        for (unsigned int i = 0; i < maxChunks; i++)
        {
            isChunkChangedMap[i] = true;
        }
        for (unsigned int i = 0; i < maxChunks; i++)
        {
            std::memset(intermediateMap[i].intermediate, 0, maxCapacityInBytes);
        }

        _lastOutput = new unsigned char[1024];
        _lastOutputSize = 0;
    }

    int getHash(unsigned char* output, size_t outputByteLen, bool useCache = true)
    {
        if (_lastOutput && outputByteLen == _lastOutputSize && isAllChunksUnchanged())
        {
            // return cached output
            std::memcpy(output, _lastOutput, outputByteLen);
            return 0;
        }

        XKCP::KangarooTwelve_Instance ktInstance;

        if (outputByteLen == 0)
            return 1;
        XKCP::KangarooTwelve_Initialize(&ktInstance, 128, outputByteLen);
        if (_KangarooTwelve_Update(&ktInstance, _state, _stateSize, useCache) != 0)
            return 1;
        int ok =  XKCP::KangarooTwelve_Final(&ktInstance, output, nullptr, 0);

        // cache last output
        std::memcpy(_lastOutput, output, outputByteLen);
        _lastOutputSize = outputByteLen;

        return ok;
    }

    void markChunkChanged(unsigned int chunkIndex)
    {
        if (chunkIndex < maxChunks)
        {
            isChunkChangedMap[chunkIndex] = true;
        }
    }

    bool isAllChunksUnchanged() const
    {
        for (unsigned int i = 0; i < maxChunks; i++)
        {
            if (isChunkChangedMap[i])
            {
                return false;
            }
        }
        return true;
    }

    unsigned int getMaxChunks() const { return maxChunks; }
};

// linux userfaultfd integration into K12Engine
class ContractStateEngine : public K12Engine
{
public:
    // Access tracker (LRU eviction)
    static inline size_t MAX_RAM_USEAGE = 10ULL * 1024 * 1024 * 1024; // 10 GB
    static inline std::list<unsigned long long> accessList; // <contractIndex | chunkIndex>
    static inline std::unordered_map<unsigned long long, std::list<unsigned long long>::iterator> accessMap; // tracker if <contractIndex | chunkIndex> is in accessList

    // IO related
    static constexpr size_t MAX_IO_NAME_LEN = 128;
    static constexpr CHAR16 BASE_DIR[] = L"contract_states/";
    static inline std::unordered_map<unsigned int, std::mutex> ioLocks;

    // Lazy loading related
    static inline std::vector<ContractStateEngine*> allEngines;
    UserFaultFD uffd;
    size_t nonPaddedSize;
    size_t paddedSize;
    unsigned int contractIndex;
    bool isUffdRegistered = false;
    std::mutex faultLock;
    int diskFd = -1;
    int memfdFd = -1;

    std::vector<bool> isChunkLoadedInMemoryMap;
    unsigned char tmpBuffer[K12_chunkSize];

    // Thread-local handoff for the memfd fd allocated inside allocateState().
    // The base K12Engine ctor receives only the buffer pointer, so we stash
    // the fd here for the derived ctor body to pick up. Serial construction
    // in qubic.cpp:7038 keeps this race-free; the thread_local guards anyway.
    static inline thread_local int lastMemfdFd = -1;

    static void* allocateState(size_t size) {
        size = alignToPageSize(size);
        int fd = memfd_create("qlite", MFD_CLOEXEC);
        if (fd == -1) throw std::runtime_error("Error: memfd_create failed | Line: " + std::to_string(__LINE__));
        if (ftruncate(fd, size) == -1) {
            close(fd);
            throw std::runtime_error("Error: ftruncate failed | Line: " + std::to_string(__LINE__));
        }
        void* buf = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        if (buf == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("Error: mmap failed | Line: " + std::to_string(__LINE__));
        }
        memset(buf, 0, size);
        lastMemfdFd = fd; // keep the fd open; derived ctor grabs it for copy_file_range
        return buf;
    }

    static bool create(unsigned char **state, size_t stateSize, unsigned int contractIndex) {
        static std::once_flag flag;
        std::call_once(flag, []() {
            allEngines.resize(contractCount);
        });

        auto engine = new ContractStateEngine(state, stateSize, contractIndex);
        // register only won't do anything, need call protect or save chunk to disk to trigger the handler
        engine->registerUserFaultFD();

        allEngines[contractIndex] = engine;

        return true;
    }

    static void registerAllUserFaultFDs() {
        for (auto engine : allEngines) {
            if (engine) {
                engine->registerUserFaultFD();
            }
        }
    }

    static ContractStateEngine* getEngine(unsigned int contractIndex) {
        if (contractIndex < allEngines.size()) {
            return allEngines[contractIndex];
        }
        return nullptr;
    }

    static void updateAccessTracker(unsigned int contractIndex, unsigned int chunkIndex) {
        unsigned long long key = ((unsigned long long)contractIndex << 32) | chunkIndex;
        auto it = accessMap.find(key);
        if (it != accessMap.end()) {
            // already in access list, move to the front
            accessList.splice(accessList.begin(), accessList, it->second);
        } else {
            // not in access list, add to the front and record in map
            accessList.push_front(key);
            accessMap[key] = accessList.begin();
        }
    }

    static size_t getRamUsageByAllEngines()
    {
        size_t usage = 0;
        for (auto engine : allEngines) {
            if (engine) {
                usage += engine->getTotalMemoryInRam();
            }
        }
        return usage;
    }

    static size_t tryEvictChunks(size_t requiredSize = 0) {
        size_t freedSize = 0;
        while (getRamUsageByAllEngines() + requiredSize > MAX_RAM_USEAGE && !accessList.empty()) {
            unsigned long long key = accessList.back();
            accessList.pop_back();
            accessMap.erase(key);

            unsigned int contractIndex = (unsigned int)(key >> 32);
            unsigned int chunkIndex = (unsigned int)(key & 0xFFFFFFFF);

            ContractStateEngine* engine = getEngine(contractIndex);
            if (engine && engine->isChunkLoadedInMemoryMap[chunkIndex]) {
                if (engine->saveChunkToDisk(chunkIndex)) {
                    freedSize += K12_chunkSize;
                }
            }
        }
        return freedSize;
    }

    ContractStateEngine(unsigned char **state, size_t stateSize, unsigned int contractIndex)
        : K12Engine((unsigned char*)allocateState(stateSize), stateSize)
    {
        memfdFd = lastMemfdFd; // grab the fd allocateState stashed for us
        lastMemfdFd = -1;
        *state = _state;
        this->contractIndex = contractIndex;
        this->nonPaddedSize = stateSize;
        this->paddedSize = alignToPageSize(stateSize);
        this->isChunkLoadedInMemoryMap.resize(maxChunks);
        for (unsigned int i = 0; i < maxChunks; i++)
        {
            isChunkLoadedInMemoryMap[i] = true; // memset in allocateState so all pages are in memory
        }

        // ensure base directory exists
        createDir(BASE_DIR);

        // open single sparse backing file for this contract; one inode per contract,
        // chunk IO via pread/pwrite at offset = chunkIndex * K12_chunkSize
        char path[64];
        std::snprintf(path, sizeof(path), "contract_states/contract_%04u.bin", contractIndex);
        diskFd = open(path, O_RDWR | O_CREAT, 0600);
        if (diskFd == -1) {
            throw std::runtime_error("Error: open contract state file failed: " + std::string(path));
        }
        if (ftruncate(diskFd, paddedSize) == -1) {
            close(diskFd);
            throw std::runtime_error("Error: ftruncate failed: " + std::string(path));
        }
    }

    bool loadChunkFromDisk(unsigned int chunkIndex, unsigned char *destBuffer, size_t chunkSize)
    {
        // lock IO for this contract
        std::lock_guard<std::mutex> lock(ioLocks[contractIndex]);

        size_t expectedSize = K12_chunkSize;
        if (paddedSize % K12_chunkSize != 0 && chunkIndex == maxChunks - 1)
        {
            expectedSize = paddedSize % K12_chunkSize;
        }
        if (chunkSize != expectedSize)
        {
            std::cout << "Contract " << contractIndex << ": Chunk " << chunkIndex
                      << " requested size mismatch. Requested: " << chunkSize
                      << ", expected: " << expectedSize << "\n";
            return false;
        }

        off_t offset = (off_t)chunkIndex * (off_t)K12_chunkSize;
        ssize_t n = pread(diskFd, destBuffer, chunkSize, offset);
        if (n != (ssize_t)chunkSize)
        {
            std::cout << "Contract " << contractIndex << ": pread chunk " << chunkIndex
                      << " failed, got " << n << " expected " << chunkSize << "\n";
            return false;
        }
        isChunkLoadedInMemoryMap[chunkIndex] = true;
        return true;
    }

    bool saveChunkToDisk(unsigned int chunkIndex)
    {
        std::lock_guard<std::mutex> lock(ioLocks[contractIndex]);

        size_t offset = chunkIndex * (size_t)K12_chunkSize;
        size_t chunkSize = K12_chunkSize;
        if (paddedSize % K12_chunkSize != 0 && chunkIndex == maxChunks - 1)
        {
            chunkSize = paddedSize % K12_chunkSize;
        }

        // Read via the memfd file descriptor (not the UFFD-monitored mmap) so
        // sparse / unfaulted pages return zeros instead of EFAULT, then pwrite
        // to the disk file. Both sides use a single fd each, no per-chunk
        // fopen/fclose overhead.
        ssize_t r = pread(memfdFd, tmpBuffer, chunkSize, (off_t)offset);
        bool success = (r == (ssize_t)chunkSize);
        if (!success)
        {
            std::cout << "Contract " << contractIndex << ": pread memfd chunk " << chunkIndex
                      << " failed, got " << r << " expected " << chunkSize
                      << " errno=" << errno << " (" << strerror(errno) << ")\n";
        }
        else
        {
            ssize_t w = pwrite(diskFd, tmpBuffer, chunkSize, (off_t)offset);
            success = (w == (ssize_t)chunkSize);
            if (!success)
            {
                std::cout << "Contract " << contractIndex << ": pwrite disk chunk " << chunkIndex
                          << " failed, got " << w << " expected " << chunkSize
                          << " errno=" << errno << " (" << strerror(errno) << ")\n";
            }
        }
        isChunkLoadedInMemoryMap[chunkIndex] = false;

        // release the memfd page back to the kernel
        if (madvise(_state + offset, chunkSize, MADV_REMOVE) == -1)
        {
            std::cout << "Contract " << contractIndex << ": madvise failed for chunk " << chunkIndex << "\n";
            success = false;
        }

        return success;
    }

    bool flushAllChunksToDisk(bool needToBeChanged = false)
    {
        bool allOk = true;
        for (unsigned int i = 0; i < maxChunks; i++)
        {
            if ((!needToBeChanged || isChunkChangedMap[i]) && isChunkLoadedInMemoryMap[i])
            {
                bool ok = saveChunkToDisk(i);
                if (!ok)
                {
                    std::cout << "Contract " << contractIndex << ": Failed to save chunk " << i << " to disk\n";
                    allOk = false;
                }
            }
        }
        return allOk;
    }

    void registerUserFaultFD()
    {
        if (isUffdRegistered) return;

        // register region for write-protect tracking
        uffdio_register reg{};
        reg.range.start = (uint64_t)_state;
        reg.range.len = paddedSize;
        reg.mode = UFFDIO_REGISTER_MODE_WP | UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_MINOR;

        if (ioctl(uffd.get(), UFFDIO_REGISTER, &reg) == -1)
            throw std::runtime_error("Error: UFFDIO_REGISTER ioctl failed for contract " + std::to_string(contractIndex));

        isUffdRegistered = true;

        std::thread handler([=]()
            {
                size_t page_size = SYSTEM_PAGE_SIZE;
                pollfd pfd{ uffd.get(), POLLIN, 0 };
                while (true) {
                    poll(&pfd, 1, -1);
                    uffd_msg msg;
                    if (read(uffd.get(), &msg, sizeof(msg)) != sizeof(msg))
                        continue;

                    if (msg.event != UFFD_EVENT_PAGEFAULT) continue;

                    auto flags = msg.arg.pagefault.flags;

                    bool is_wp = flags & UFFD_PAGEFAULT_FLAG_WP;
                    bool is_minor = flags & UFFD_PAGEFAULT_FLAG_MINOR; // aka read in our case

                    // If neither is set → MISSING
                    bool is_missing = !is_wp && !is_minor;

                    auto accessAddress = msg.arg.pagefault.address;
                    auto pageAddress = msg.arg.pagefault.address & ~(page_size - 1);

                    size_t offset = accessAddress - (size_t)_state;
                    unsigned int chunkIndex = offset / K12_chunkSize;

                    size_t startRange = (size_t)_state + (chunkIndex * (size_t)K12_chunkSize);
                    // if there is only 1 page left (system memory page) then just need to cover to the last system page (cover full K12 chunk will go beyond the state size)
                    size_t lenRange = std::min(paddedSize - (chunkIndex * (size_t)K12_chunkSize), (size_t)K12_chunkSize);

#ifdef LITE_ENGINE_DEBUG
                    if (engineCustomeActionCallback) {
                        engineCustomeActionCallback();
                    }
#endif

                    {
                        // handle write-protect page fault
                        if (is_wp)
                        {
                            updateAccessTracker(contractIndex, chunkIndex);
                            markChunkChanged(chunkIndex);
#ifdef LITE_ENGINE_DEBUG
                            printf("Contract %u: page fault at address 0x%llx, chunk %u marked changed\n",
                                   contractIndex, (unsigned long long)accessAddress, chunkIndex);
#endif

                            // remove write-protect so write can continue
                            uffdio_writeprotect uwp{};
                            uwp.range.start = startRange;
                            uwp.range.len = lenRange;
                            uwp.mode = 0;

                            if (ioctl(uffd.get(), UFFDIO_WRITEPROTECT, &uwp) == -1)
                            {
                                std::cout << "Contract " << contractIndex << ": UFFDIO_WRITEPROTECT remove failed\n";
                            }
                        }

                        // handle missing page fault
                        if (is_missing)
                        {
                            bool loadOk = false;
                            do
                            {
                                loadOk = loadChunkFromDisk(chunkIndex, tmpBuffer, lenRange);
                                if (!loadOk)
                                {
                                    std::cout << "Critical error: Contract " << contractIndex
                                              << ": Failed to load chunk " << chunkIndex
                                              << " from disk. Retrying in 1 second...\n";
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                                }
                            } while (!loadOk);
#ifdef LITE_ENGINE_DEBUG
                            printf("Loaded chunk %u for contract %u from disk\n",
                                   chunkIndex, contractIndex);
#endif

                            // copy data into the page
                            uffdio_copy uc{};
                            uc.src = (uint64_t)tmpBuffer;
                            uc.dst = startRange;
                            uc.len = lenRange;
                            uc.mode = UFFDIO_CONTINUE_MODE_WP;
                            if (ioctl(uffd.get(), UFFDIO_COPY, &uc) == -1)
                            {
                                std::cout << "Contract " << contractIndex << ": UFFDIO_COPY failed\n";
                            }
                            if (uc.copy != uc.len)
                            {
                                std::cout << "Contract " << contractIndex << ": UFFDIO_COPY incomplete copy\n";
                            }
                        }

                        // handle minor page fault (read)
                        if (is_minor)
                        {
                            updateAccessTracker(contractIndex, chunkIndex);
#ifdef LITE_ENGINE_DEBUG
                            printf("Found minor page fault at contract %llu address 0x%llx, chunk %u\n", contractIndex,
                                   (unsigned long long)accessAddress, chunkIndex);
#endif
                            // resume execution
                            uffdio_continue ucont{};
                            ucont.range.start = startRange;
                            ucont.range.len = lenRange;
                            ucont.mode = UFFDIO_CONTINUE_MODE_WP;
                            if (ioctl(uffd.get(), UFFDIO_CONTINUE, &ucont) == -1)
                            {
                                std::cout << "Contract " << contractIndex << ": UFFDIO_CONTINUE failed\n";
                                // retry
                                while (true)
                                {
                                    // remove PTE for this page and retry
                                    if (madvise((void*)startRange, lenRange, MADV_DONTNEED) == -1)
                                    {
                                        std::cout << "Contract " << contractIndex << ": madvise failed during UFFDIO_CONTINUE retry\n";
                                        std::cout << "Error " << errno << ": " << strerror(errno) << "\n";
                                    }
                                    if (ioctl(uffd.get(), UFFDIO_CONTINUE, &ucont) != -1) {
                                        break;
                                    }
                                    std::cout << "Contract " << contractIndex << ": UFFDIO_CONTINUE retry failed\n";
                                    std::cout << "Error " << errno << ": " << strerror(errno) << "\n";
                                    std::cout << "Details: address 0x" << std::hex << ucont.range.start << std::dec
                                              << ", length " << ucont.range.len << "\n";
                                    std::cout << "Chunk index: " << chunkIndex << " | Max chunks: " << maxChunks << "\n";
                                    // Check alignment specifically
                                    if (ucont.range.start % page_size != 0)
                                    {
                                        std::cout << "Contract " << contractIndex << ": UFFDIO_CONTINUE failed due to unaligned address 0x"
                                                  << std::hex << ucont.range.start << std::dec << "\n";
                                    }
                                    if (ucont.range.len % page_size != 0)
                                    {
                                        std::cout << "Contract " << contractIndex << ": UFFDIO_CONTINUE failed due to unaligned length "
                                                  << ucont.range.len << "\n";
                                    }
                                    if (errno != EEXIST)
                                    {
                                        std::cout << "Contract " << contractIndex << ": UFFDIO_CONTINUE failed due to unexpected error (cannot ignored)\n";
                                    } else
                                    {
                                        std::cout << "Contract " << contractIndex << ": UFFDIO_CONTINUE failed due to EEXIST (page already present), skip continue operation\n";
                                        markChunkChanged(chunkIndex); // mark changed to be safe
                                        // by default, kernel wont awake the faulting thread if UFFDIO_CONTINUE fails with EEXIST
                                        // so we need to awake it
                                        uffdio_range range;
                                        range.start = startRange;
                                        range.len = lenRange;
                                        if (ioctl(uffd.get(), UFFDIO_WAKE, &range) == -1)
                                        {
                                            std::cout << "Contract " << contractIndex << ": UFFDIO_WAKE failed\n";
                                        }
                                        break;
                                    }
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                                }
                            }
                        }
                    }
                }
            });
        handler.detach();
    }

    size_t getTotalMemoryInRam()
    {
        size_t totalRam = 0;
        for (unsigned int i = 0; i < maxChunks; i++)
        {
            if (isChunkLoadedInMemoryMap[i])
            {
                if (paddedSize % K12_chunkSize != 0 && i == maxChunks - 1)
                {
                    totalRam += paddedSize % K12_chunkSize;
                }
                else
                {
                    totalRam += K12_chunkSize;
                }
            }
        }
        return totalRam;
    }

    void reprotectWriteRegion(size_t startOffset = 0, size_t len = 0) {
        if (!isUffdRegistered) return;

        std::lock_guard<std::mutex> lock(faultLock);

        if (len == 0 && startOffset == 0)
        {
            len = paddedSize;
        }

        uffdio_writeprotect wp {};

        wp.range.start = (uint64_t)_state + startOffset;
        wp.range.len   = len;

        // UFFDIO_WRITEPROTECT_MODE_WP: Sets the write-protect bit
        // (If you set mode to 0, it removes protection)
        wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;

        if (ioctl(uffd.get(), UFFDIO_WRITEPROTECT, &wp) == -1) {
            std::cout << "Contract " << contractIndex << ": UFFDIO_WRITEPROTECT failesd\n";

            // mark all chunks as changed to be safe
            for (unsigned int i = 0; i < maxChunks; i++)
            {
                isChunkChangedMap[i] = true;
            }
        }
    }

    void reprotectReadRegion(size_t startOffset = 0, size_t len = 0)
    {
        if (!isUffdRegistered) return;

        std::lock_guard<std::mutex> lock(faultLock);

        if (len == 0 && startOffset == 0)
        {
            len = paddedSize;
        }

        madvise(_state + startOffset, len, MADV_DONTNEED); // remove only PTE mappings, all our data still safe. next read will trigger minor fault
    }

    unsigned long long touchAllPages()
    {
        unsigned long long sum = 0;
        for (size_t offset = 0; offset < paddedSize; offset += SYSTEM_PAGE_SIZE)
        {
            sum += _state[offset];
        }
        return sum;
    }

    int getHashAndReprotect(unsigned char* output, size_t outputByteLen)
    {
        int res = getHash(output, outputByteLen);
        reprotectWriteRegion();
        reprotectReadRegion();
        return res;
    }

};