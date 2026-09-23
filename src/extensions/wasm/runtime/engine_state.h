#pragma once

// WAMR instance state, libffi state, memory layout, and loader-owned buffers.
#ifdef LITE_WASM_SC

#if !defined(TESTNET) || !defined(TESTNET_LITE_RAM)
#error "LITE_WASM_SC requires TESTNET and TESTNET_LITE_RAM"
#endif

#include <ffi.h>
#include <string>
#include <chrono>
#include "wasm_export.h"
#include "extensions/wasm/runtime/arena_scope.h"
#include "extensions/wasm/runtime/contract_slots.h"
#include "extensions/wasm/runtime/lhost_registry.h"
#include "extensions/wasm/runtime/state_write_journal.h"

void logColorToScreen(std::string type, std::string msg);

#ifndef WASM_ARENA_SIZE
#define WASM_ARENA_SIZE (1024u * 1024u * 1024u)
#endif

namespace Wasm::Runtime
{

static constexpr unsigned long long WASM_IO_CAPACITY = (unsigned long long)WASM_DISPATCH_FRAME_CAPACITY + WASM_ARENA_SIZE;

static bool engineReady = false;
static ffi_cif dispatchCallInterface;
static ffi_type* dispatchCallArguments[5];
static ffi_cif migrationCallInterface;
static ffi_type* migrationCallArguments[4];

struct EntryBinding
{
    uint32_t contractIndex;
    uint16_t inputType;
    DispatchKind kind;
};

struct EngineSlot
{
    bool loaded = false;
    wasm_module_t module = nullptr;
    wasm_module_inst_t instance = nullptr;
    wasm_exec_env_t loadExecEnv = nullptr;
    wasm_function_inst_t dispatchFunction = nullptr;
    unsigned char* moduleBuffer = nullptr;
    uint32_t stateOffset = 0;
    uint32_t stateSize = 0;
    uint32_t ioBaseOffset = 0;
    uint32_t contextOffset = 0;
    uint32_t entryCount = 0;
    EntryBinding entryBindings[WASM_MAX_USER_ENTRIES] = {};
    ffi_closure* entryClosures[WASM_MAX_USER_ENTRIES] = {};
    EntryBinding systemBindings[WASM_SYSTEM_PROCEDURE_COUNT] = {};
    ffi_closure* systemClosures[WASM_SYSTEM_PROCEDURE_COUNT] = {};
    uint16_t systemInputSizes[WASM_SYSTEM_PROCEDURE_COUNT] = {};
    uint16_t systemOutputSizes[WASM_SYSTEM_PROCEDURE_COUNT] = {};
    bool stateStubReleased = false;
    std::string lastTrap;
    bool hasMigration = false;
    uint32_t migrationOldStateSize = 0;
    uint32_t migrationLocalsSize = 0;
    EntryBinding migrationBinding = {};
    ffi_closure* migrationClosure = nullptr;
    unsigned char* pendingOldState = nullptr;
    uint32_t pendingOldStateSize = 0;
    // the last load started from a staged state, so the slot counts as constructed and INITIALIZE must not run over it
    bool stateSeeded = false;
    // Zero when the artifact carries no journal, which leaves the page tracker as the only diff source.
    uint32_t journalBaseOffset = 0;
    JournalHeader journalHeader = {};
    // Latched once a dispatch overflows: the before-image of the blocks it missed is already gone.
    bool journalOverflowed = false;
};

static EngineSlot engineSlots[WASM_RESERVED_SLOT_COUNT];

static inline int engineSlotOffset(unsigned int contractIndex)
{
    const int slotOffset = (int)contractIndex - (int)reservedSlotBase();
    if (slotOffset < 0 || slotOffset >= WASM_RESERVED_SLOT_COUNT)
    {
        return -1;
    }

    return slotOffset;
}

static inline bool isContractLoaded(unsigned int contractIndex)
{
    const int slotOffset = engineSlotOffset(contractIndex);
    return slotOffset >= 0 && engineSlots[slotOffset].loaded;
}

static inline std::string lastTrap(unsigned int contractIndex)
{
    const int slotOffset = engineSlotOffset(contractIndex);
    if (slotOffset < 0 || !engineSlots[slotOffset].loaded)
    {
        return std::string();
    }

    return engineSlots[slotOffset].lastTrap;
}

static inline uint32_t callU32(wasm_exec_env_t execEnv, wasm_function_inst_t function)
{
    uint32_t arguments[1] = { 0 };

    wasm_runtime_call_wasm(execEnv, function, 0, arguments);
    return arguments[0];
}

static inline unsigned long long effectiveStateSize(unsigned int contractIndex, unsigned long long defaultSize)
{
    const int slotOffset = engineSlotOffset(contractIndex);
    if (slotOffset < 0 || !engineSlots[slotOffset].loaded)
    {
        return defaultSize;
    }

    return engineSlots[slotOffset].stateSize;
}

static inline void ensureThreadEnvironment()
{
    if (!wasm_runtime_thread_env_inited())
    {
        wasm_runtime_init_thread_env();
    }
}

static thread_local wasm_exec_env_t currentEnvironment = nullptr;
static thread_local uint32_t slotCallDepth[WASM_RESERVED_SLOT_COUNT] = {};
// Depth across every slot, so a root frame can be told from a callee living in another slot.
static thread_local uint32_t dispatchDepth = 0;
static thread_local CallContext* slotCallContexts[WASM_RESERVED_SLOT_COUNT] = {};

struct IoSizes
{
    // A migration's input is the whole old state, which outgrows 16 bits long before a user entry does.
    uint32_t input = 0;
    uint16_t output = 0;
};

static MemoryLayout resolveMemoryLayout(const EngineSlot& slot)
{
    return fixedMemoryLayout(slot.ioBaseOffset);
}

static bool resolveArenaLimit(const MemoryLayout& fixedLayout, uint32_t& arenaLimit)
{
    const unsigned long long limit = (unsigned long long)fixedLayout.arenaOffset + WASM_ARENA_SIZE;
    if (limit > 0xffffffffull)
    {
        return false;
    }

    arenaLimit = (uint32_t)limit;
    return true;
}

static bool resolveIoSizes(uint32_t contractIndex, uint16_t inputType, DispatchKind kind, const EngineSlot& slot, IoSizes& sizes)
{
    switch (kind)
    {
        case DispatchKind::UserFunction:
            sizes.input = contractUserFunctionInputSizes[contractIndex][inputType];
            sizes.output = contractUserFunctionOutputSizes[contractIndex][inputType];
            break;
        case DispatchKind::SystemProcedure:
            sizes.input = slot.systemInputSizes[inputType];
            sizes.output = slot.systemOutputSizes[inputType];
            break;
        case DispatchKind::UserProcedure:
            sizes.input = contractUserProcedureInputSizes[contractIndex][inputType];
            sizes.output = contractUserProcedureOutputSizes[contractIndex][inputType];
            break;
        case DispatchKind::Migration:
            return false;
    }

    if (sizes.input > WASM_INPUT_CAPACITY || sizes.output > WASM_OUTPUT_CAPACITY)
    {
        logColorToScreen("ERROR", "LITEWASM dispatch in/out exceeds io region idx=" + std::to_string(contractIndex) + " in=" + std::to_string(sizes.input) + " out=" + std::to_string(sizes.output));
        return false;
    }

    return true;
}


struct ModuleResources
{
    wasm_module_t module = nullptr;
    wasm_module_inst_t instance = nullptr;
    wasm_exec_env_t execEnv = nullptr;
    unsigned char* moduleBuffer = nullptr;

    ~ModuleResources()
    {
        if (execEnv)
        {
            wasm_runtime_destroy_exec_env(execEnv);
        }

        if (instance)
        {
            wasm_runtime_deinstantiate(instance);
        }

        if (module)
        {
            wasm_runtime_unload(module);
        }

        if (moduleBuffer)
        {
            free(moduleBuffer);
        }
    }

    void release()
    {
        module = nullptr;
        instance = nullptr;
        execEnv = nullptr;
        moduleBuffer = nullptr;
    }

    ModuleResources() = default;
    ModuleResources(const ModuleResources&) = delete;
    ModuleResources& operator=(const ModuleResources&) = delete;
};

struct OwnedBuffer
{
    unsigned char* data = nullptr;

    ~OwnedBuffer()
    {
        if (data)
        {
            free(data);
        }
    }

    void allocate(size_t size)
    {
        if (data)
        {
            free(data);
        }

        data = size ? (unsigned char*)malloc(size) : nullptr;
    }

    unsigned char* release()
    {
        unsigned char* releasedData = data;

        data = nullptr;
        return releasedData;
    }

    OwnedBuffer() = default;
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;
};

struct StateSnapshot
{
    OwnedBuffer buffer;
    uint32_t size = 0;
};

struct RequiredExports
{
    wasm_function_inst_t contractIndex = nullptr;
    wasm_function_inst_t stateAddress = nullptr;
    wasm_function_inst_t stateSize = nullptr;
    wasm_function_inst_t ioBase = nullptr;
    wasm_function_inst_t registrationCount = nullptr;
    wasm_function_inst_t registrationInfo = nullptr;
    wasm_function_inst_t dispatch = nullptr;
};

struct ModuleLayout
{
    uint32_t stateOffset = 0;
    uint32_t stateSize = 0;
    uint32_t ioBaseOffset = 0;
};

struct EntryInfo
{
    uint32_t inputType;
    uint32_t kind;
    uint32_t inputSize;
    uint32_t outputSize;
};

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
