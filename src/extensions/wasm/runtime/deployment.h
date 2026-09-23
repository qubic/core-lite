#pragma once

// Module upload, deployment, activation scheduling, and boot setup.
#ifdef LITE_WASM_SC

#include "extensions/wasm/runtime/contract_slots.h"
#include "extensions/wasm/runtime/deployment_protocol.h"
#include "extensions/wasm/runtime/engine.h"

namespace Wasm::Runtime
{

[[maybe_unused]] static void beginModuleUpload(unsigned long long sessionId, unsigned int totalSize, unsigned int chunkCount, const unsigned char* finalHash,
    unsigned int tick)
{
    const bool retry = moduleUpload.active && !moduleUploadStale(tick);
    if (!tryBeginModuleUpload(sessionId, totalSize, chunkCount, finalHash, tick))
    {
        if (moduleUpload.active)
        {
            logColorToScreen("WARN", "LITEDYN: UploadBegin rejected; session " + std::to_string(moduleUpload.sessionId) + " is active");
        }
        else
        {
            logColorToScreen("WARN", "LITEDYN: UploadBegin rejected; invalid size or chunk count");
        }
        return;
    }

    logToConsole(retry ? L"LITEDYN: UploadBegin retry accepted" : L"LITEDYN: UploadBegin received");
}

[[maybe_unused]] static void receiveModuleChunk(unsigned long long sessionId, unsigned int sequence, const unsigned char* data, unsigned int dataLength,
    unsigned int tick)
{
    tryReceiveModuleChunk(sessionId, sequence, data, dataLength, tick);
}

// Runs once per tick so an upload whose client died frees the node without a restart.
[[maybe_unused]] static void expireStaleModuleUpload(unsigned int tick)
{
    if (!moduleUploadStale(tick))
    {
        return;
    }

    logColorToScreen("WARN", "LITEDYN: upload session " + std::to_string(moduleUpload.sessionId) + " dropped after " + std::to_string(tick - moduleUpload.lastProgressTick) + " idle ticks");
    moduleUpload = ModuleUpload{};
}

static bool moduleUploadComplete()
{
    if (!moduleUpload.active || moduleUpload.receivedCount != moduleUpload.chunkCount)
    {
        return false;
    }

    unsigned char calculatedHash[32];

    KangarooTwelve(moduleUploadBuffer, moduleUpload.totalSize, calculatedHash, 32);
    for (int index = 0; index < 32; index++)
    {
        if (calculatedHash[index] != moduleUpload.finalHash[index])
        {
            return false;
        }
    }

    return true;
}

// Defined by runtime/engine.h earlier in the extension translation unit.
static bool loadFromBytes(unsigned int contractIndex, const unsigned char* bytes, unsigned int length);
static bool isContractLoaded(unsigned int contractIndex);
static bool hasPendingMigration(unsigned int contractIndex);
static void runPendingMigration(unsigned int contractIndex);
static bool wasStateSeeded(unsigned int contractIndex);

// the HTTP thread reads the record while the tick thread writes it.
static void recordDeployOutcome(unsigned long long sessionId, unsigned int slot, unsigned int tick, const char* code, const std::string& message)
{
    TraceLockScope lock;
    storeDeployOutcome(lastDeployOutcome, sessionId, slot, tick, code, message);
}

[[maybe_unused]] static DeployOutcome deployOutcomeSnapshot()
{
    TraceLockScope lock;
    return lastDeployOutcome;
}

[[maybe_unused]] static void deployModule(unsigned long long sessionId, unsigned int targetSlot, const unsigned char* finalHash, unsigned int abiVersion,
    unsigned int /*stateLayoutVersion*/,
    const char* name, unsigned int tick)
{
    const auto refuse = [&](const char* code, const std::string& message)
    {
        logColorToScreen("ERROR", "LITEDYN: deploy refused; " + message);
        recordDeployOutcome(sessionId, targetSlot, tick, code, message);
    };

    const int slotOffset = reservedSlotOffset(targetSlot);
    if (slotOffset < 0)
    {
        refuse(DEPLOY_CODE_BAD_SLOT, "slot " + std::to_string(targetSlot) + " is not a dynamic contract slot");
        return;
    }

    if (abiVersion != WASM_ABI_VERSION)
    {
        refuse(DEPLOY_CODE_ABI_MISMATCH, "unsupported Wasm ABI version " + std::to_string(abiVersion) + "; expected " + std::to_string(WASM_ABI_VERSION));
        return;
    }

    if (sessionId != moduleUpload.sessionId)
    {
        refuse(DEPLOY_CODE_SESSION_MISMATCH, "session " + std::to_string(sessionId) + " is not the upload session on this node");
        return;
    }

    if (!moduleUpload.active || moduleUpload.receivedCount != moduleUpload.chunkCount)
    {
        refuse(DEPLOY_CODE_INCOMPLETE,
            "upload incomplete (" + std::to_string(moduleUpload.active ? moduleUpload.receivedCount : 0u) + "/" + std::to_string(moduleUpload.chunkCount) + " chunks)");
        return;
    }

    if (!moduleUploadComplete())
    {
        refuse(DEPLOY_CODE_HASH_MISMATCH, "uploaded bytes do not hash to the digest the upload announced");
        return;
    }

    for (int index = 0; index < 32; index++)
    {
        if (finalHash[index] != moduleUpload.finalHash[index])
        {
            refuse(DEPLOY_CODE_HASH_MISMATCH, "deploy names a different module digest than the upload");
            return;
        }
    }

    bool loadOk = false;
    const unsigned char* artifact = moduleUploadBuffer;
    const bool hasWasmMagic = moduleUpload.totalSize >= 4 && artifact[0] == 0x00 && artifact[1] == 0x61 && artifact[2] == 0x73 && artifact[3] == 0x6d;

    if (hasWasmMagic)
    {
        lastLoadError.clear();
        loadOk = loadFromBytes(targetSlot, moduleUploadBuffer, moduleUpload.totalSize);
        if (!loadOk)
        {
            refuse(DEPLOY_CODE_LOAD_FAILED, lastLoadError.empty() ? "wasm load failed" : lastLoadError);
        }
    }
    else
    {
        refuse(DEPLOY_CODE_NOT_WASM, "upload is not a wasm module ('\\0asm' expected)");
    }

    if (!loadOk)
    {
        logToConsole(L"LITEDYN: ERROR load failed - resident slot unchanged");
        moduleUpload.active = false;
        return;
    }

    ContractSlot& slot = contractSlots[slotOffset];
    copyMem(slot.codeHash, finalHash, 32);
    if (name)
    {
        copyMem(slot.name, name, 32);
        slot.name[31] = 0;
    }

    slot.armed = true;
    slot.version++;
    logToConsole(L"LITEDYN: Deploy accepted, slot armed");

    // a seeded slot is already constructed, now and for every later redeploy
    if (wasStateSeeded(targetSlot))
    {
        slot.everInitialized = true;
    }

    slot.needsMigrate = hasPendingMigration(targetSlot);
    slot.constructed = slot.everInitialized && !slot.needsMigrate;
    if (slot.needsMigrate)
    {
        logToConsole(L"LITEDYN: migrate scheduled for next tick");
    }

    moduleUpload.active = false;
    recordDeployOutcome(sessionId, targetSlot, tick, DEPLOY_CODE_OK, "slot armed");
}


[[maybe_unused]] static void dispatchDeploymentTransaction(unsigned short inputType, const unsigned char* input, unsigned int size, unsigned int tick)
{
    if (inputType == WASM_DEPLOYMENT_UPLOAD_BEGIN_INPUT_TYPE)
    {
        DeploymentProtocol::UploadBeginMessage message;
        if (size < sizeof(message))
        {
            return;
        }

        copyMem(&message, input, sizeof(message));
        beginModuleUpload(message.sessionId, message.totalSize, message.chunkCount, message.finalHash, tick);
    }
    else if (inputType == WASM_DEPLOYMENT_UPLOAD_CHUNK_INPUT_TYPE)
    {
        DeploymentProtocol::UploadChunkHeader message;
        if (size < sizeof(message))
        {
            return;
        }

        copyMem(&message, input, sizeof(message));
        if (sizeof(message) + message.dataLength > size)
        {
            return;
        }

        receiveModuleChunk(message.sessionId, message.sequence, input + sizeof(message), message.dataLength, tick);
    }
    else if (inputType == WASM_DEPLOYMENT_DEPLOY_INPUT_TYPE)
    {
        DeploymentProtocol::DeployHeader message;
        if (size < sizeof(message))
        {
            return;
        }

        copyMem(&message, input, sizeof(message));
        const char* name = nullptr;
        if (size >= sizeof(DeploymentProtocol::DeployMessage))
        {
            name = reinterpret_cast<const char*>(input + sizeof(message));
        }

        deployModule(message.sessionId, message.targetSlot, message.finalHash, message.abiVersion, message.stateLayoutVersion, name, tick);
    }
}

// native contracts have no deploy to take a staged state, so the tick thread writes it between ticks, where INITIALIZE would run
static void applyStagedNativeStates()
{
    if (!g_nativeStateStaged.exchange(false, std::memory_order_acquire))
    {
        return;
    }

    for (unsigned int contractIndex = 1; contractIndex < WASM_RESERVED_SLOT_BASE; contractIndex++)
    {
        unsigned char* stagedBytes = nullptr;
        unsigned long long stagedSize = 0;
        if (!takeStagedState(contractIndex, stagedBytes, stagedSize, /*dropIncomplete=*/false))
        {
            continue;
        }

        if (stagedSize == contractDescriptions[contractIndex].stateSize && contractStates[contractIndex])
        {
            contractStateLock[contractIndex].acquireWrite();
            {
                StateWriteSeqScope writeSeq(true, contractIndex);
                copyMem(contractStates[contractIndex], stagedBytes, stagedSize);
            }
            __markContractStateDirty(contractIndex);
            contractStateLock[contractIndex].releaseWrite();
            logColorToScreen("INFO", "LITEDYN: staged state applied idx=" + std::to_string(contractIndex) + " (" + std::to_string(stagedSize) + " bytes)");
        }

        free(stagedBytes);
    }
}

static bool hasPendingActivation()
{
    if (g_nativeStateStaged.load(std::memory_order_acquire))
    {
        return true;
    }

    for (unsigned int slotOffset = 0; slotOffset < WASM_RESERVED_SLOT_COUNT; slotOffset++)
    {
        const ContractSlot& slot = contractSlots[slotOffset];
        if (slot.armed && (!slot.constructed || slot.needsMigrate))
        {
            return true;
        }
    }

    return false;
}

[[maybe_unused]] static void activatePendingContracts()
{
    applyStagedNativeStates();

    for (unsigned int slotOffset = 0; slotOffset < WASM_RESERVED_SLOT_COUNT; slotOffset++)
    {
        ContractSlot& slot = contractSlots[slotOffset];
        if (!slot.armed)
        {
            continue;
        }

        const unsigned int contractIndex = WASM_RESERVED_SLOT_BASE + slotOffset;
        if (slot.needsMigrate)
        {
            runPendingMigration(contractIndex);
            slot.needsMigrate = false;
            slot.constructed = true;
            continue;
        }

        if (slot.constructed)
        {
            continue;
        }

        if (contractSystemProcedures[contractIndex][INITIALIZE])
        {
            QpiContextSystemProcedureCall qpiContext(contractIndex, INITIALIZE);

            qpiContext.call();
            slot.everInitialized = true;
            logToConsole(L"LITEDYN: slot constructed (INITIALIZE ran)");
        }
        else
        {
            logToConsole(L"LITEDYN: ERROR construct skipped - tables unpatched (load failed)");
        }

        slot.constructed = true;
    }
}

// The dev reserve every runtime seeds a slot with, so the dormancy point is the same wherever a contract
// developer runs it: about a hundred state-changing procedures on a state near the 1 GiB limit.
#define LITE_DEV_FEE_RESERVE 100000000000

[[maybe_unused]] static void initializeDeployment()
{
    logToConsole(L"LITEWASM: runtime deployment enabled for testnet lite RAM");

    for (unsigned int slotOffset = 0; slotOffset < WASM_RESERVED_SLOT_COUNT; slotOffset++)
    {
        const unsigned int contractIndex = WASM_RESERVED_SLOT_BASE + slotOffset;

        contractError[contractIndex] = NoContractError;
        if (getContractFeeReserve(contractIndex) <= 0)
        {
            setContractFeeReserve(contractIndex, LITE_DEV_FEE_RESERVE);
        }
    }
}

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
