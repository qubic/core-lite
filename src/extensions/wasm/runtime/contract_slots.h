#pragma once

// Reserved contract slots and in-progress module upload state.
#ifdef LITE_WASM_SC

#include <cstring>
#include <string>

#ifndef WASM_MAX_MODULE_SIZE
#define WASM_MAX_MODULE_SIZE (4u * 1024u * 1024u)
#endif

namespace Wasm::Runtime
{

static constexpr unsigned int WASM_UPLOAD_CHUNK_SIZE = 1008u;
static constexpr unsigned int WASM_MAX_UPLOAD_CHUNKS = (WASM_MAX_MODULE_SIZE - 1u) / WASM_UPLOAD_CHUNK_SIZE + 1u;
// An upload that receives nothing for this many ticks is dropped so a killed client cannot block deployment.
static constexpr unsigned int WASM_UPLOAD_STALE_TICKS = 32u;

struct ContractSlot
{
    bool armed = false;
    bool constructed = false;
    bool everInitialized = false;
    bool needsMigrate = false;
    unsigned char codeHash[32] = {};
    unsigned int version = 0;
    char name[32] = {};
    std::string sourceH;
};

static ContractSlot contractSlots[WASM_RESERVED_SLOT_COUNT];

struct ModuleUpload
{
    bool active = false;
    unsigned long long sessionId = 0;
    unsigned int totalSize = 0;
    unsigned int chunkCount = 0;
    unsigned int receivedCount = 0;
    unsigned int lastProgressTick = 0;
    unsigned char finalHash[32] = {};
};

static ModuleUpload moduleUpload;
static unsigned char moduleUploadBuffer[WASM_MAX_MODULE_SIZE];
static unsigned char receivedChunkBits[(WASM_MAX_UPLOAD_CHUNKS + 7u) / 8u];

static inline unsigned int expectedModuleUploadChunkCount(unsigned int totalSize)
{
    if (!totalSize)
    {
        return 0;
    }

    return (totalSize - 1u) / WASM_UPLOAD_CHUNK_SIZE + 1u;
}

static inline bool validModuleUploadShape(unsigned int totalSize, unsigned int chunkCount)
{
    return totalSize > 0 && totalSize <= WASM_MAX_MODULE_SIZE && chunkCount == expectedModuleUploadChunkCount(totalSize);
}

static inline bool moduleUploadStale(unsigned int tick)
{
    return moduleUpload.active && tick > moduleUpload.lastProgressTick + WASM_UPLOAD_STALE_TICKS;
}

static inline bool tryBeginModuleUpload(unsigned long long sessionId, unsigned int totalSize, unsigned int chunkCount, const unsigned char* finalHash,
    unsigned int tick = 0)
{
    if (moduleUploadStale(tick))
    {
        moduleUpload = ModuleUpload{};
    }

    if (moduleUpload.active)
    {
        return moduleUpload.sessionId == sessionId;
    }

    if (!finalHash || !validModuleUploadShape(totalSize, chunkCount))
    {
        return false;
    }

    moduleUpload.active = true;
    moduleUpload.sessionId = sessionId;
    moduleUpload.totalSize = totalSize;
    moduleUpload.chunkCount = chunkCount;
    moduleUpload.receivedCount = 0;
    moduleUpload.lastProgressTick = tick;
    std::memcpy(moduleUpload.finalHash, finalHash, sizeof(moduleUpload.finalHash));
    std::memset(receivedChunkBits, 0, sizeof(receivedChunkBits));
    return true;
}

static inline bool tryReceiveModuleChunk(unsigned long long sessionId, unsigned int sequence, const unsigned char* data, unsigned int dataLength,
    unsigned int tick = 0)
{
    if (!moduleUpload.active || sessionId != moduleUpload.sessionId)
    {
        return false;
    }

    const unsigned long long destinationOffset = (unsigned long long)sequence * WASM_UPLOAD_CHUNK_SIZE;
    // a chunk names its own offset, so arrival order is free: the bitmap refuses a repeat and the digest checks the whole.
    if (!data || sequence >= moduleUpload.chunkCount)
    {
        return false;
    }

    const unsigned int remainingSize = moduleUpload.totalSize - (unsigned int)destinationOffset;
    const unsigned int expectedDataLength = remainingSize < WASM_UPLOAD_CHUNK_SIZE ? remainingSize
        : WASM_UPLOAD_CHUNK_SIZE;
    if (dataLength != expectedDataLength)
    {
        return false;
    }

    const unsigned int sequenceByte = sequence >> 3;
    const unsigned int sequenceBit = 1u << (sequence & 7);
    if (receivedChunkBits[sequenceByte] & sequenceBit)
    {
        return false;
    }

    std::memcpy(moduleUploadBuffer + destinationOffset, data, dataLength);
    receivedChunkBits[sequenceByte] |= sequenceBit;
    moduleUpload.receivedCount++;
    moduleUpload.lastProgressTick = tick;
    return true;
}

// what became of the last DEPLOY a client can still act on, served beside the upload progress.
struct DeployOutcome
{
    bool set = false;
    unsigned long long sessionId = 0;
    unsigned int slot = 0;
    unsigned int tick = 0;
    bool ok = false;
    char code[24] = {};
    char message[224] = {};
};

static DeployOutcome lastDeployOutcome;

static constexpr const char* DEPLOY_CODE_OK = "ok";
static constexpr const char* DEPLOY_CODE_BAD_SLOT = "bad-slot";
static constexpr const char* DEPLOY_CODE_ABI_MISMATCH = "abi-mismatch";
static constexpr const char* DEPLOY_CODE_SESSION_MISMATCH = "session-mismatch";
static constexpr const char* DEPLOY_CODE_INCOMPLETE = "incomplete";
static constexpr const char* DEPLOY_CODE_HASH_MISMATCH = "hash-mismatch";
static constexpr const char* DEPLOY_CODE_NOT_WASM = "not-wasm";
static constexpr const char* DEPLOY_CODE_LOAD_FAILED = "load-failed";

// a client resends DEPLOY until it sees the slot armed, so a session's verdict stands once given: only an upload
// that was still missing chunks can end differently.
static inline bool deployOutcomeReplaces(const DeployOutcome& stored, unsigned long long sessionId)
{
    return !stored.set || stored.sessionId != sessionId || strcmp(stored.code, DEPLOY_CODE_INCOMPLETE) == 0;
}

static inline void storeDeployOutcome(DeployOutcome& stored, unsigned long long sessionId, unsigned int slot, unsigned int tick, const char* code,
    const std::string& message)
{
    if (!deployOutcomeReplaces(stored, sessionId))
    {
        return;
    }

    stored = DeployOutcome{};
    stored.set = true;
    stored.sessionId = sessionId;
    stored.slot = slot;
    stored.tick = tick;
    stored.ok = strcmp(code, DEPLOY_CODE_OK) == 0;
    strncpy(stored.code, code, sizeof(stored.code) - 1);
    strncpy(stored.message, message.c_str(), sizeof(stored.message) - 1);
}

static inline unsigned int reservedSlotBase()
{
    return WASM_RESERVED_SLOT_BASE;
}

static inline int reservedSlotOffset(unsigned int contractIndex)
{
    const int slotOffset = (int)contractIndex - (int)WASM_RESERVED_SLOT_BASE;
    if (slotOffset < 0 || slotOffset >= (int)WASM_RESERVED_SLOT_COUNT)
    {
        return -1;
    }

    return slotOffset;
}

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
