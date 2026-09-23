#pragma once

// Runtime module activation, pending migration, and WAMR initialization.
#ifdef LITE_WASM_SC

#include "extensions/wasm/runtime/registration.h"

namespace Wasm::Runtime
{

[[maybe_unused]] static bool loadFromBytes(unsigned int contractIndex, const unsigned char* bytes, unsigned int length)
{
    const int slotOffset = engineSlotOffset(contractIndex);
    if (slotOffset < 0)
    {
        return loadFail("slot " + std::to_string(contractIndex) + " is not a dynamic contract slot");
    }

    EngineSlot& slot = engineSlots[slotOffset];

    ensureThreadEnvironment();
    ModuleResources moduleSet;
    if (!prepareModuleBuffer(moduleSet, bytes, length))
    {
        return false;
    }

    if (!loadModule(length, moduleSet))
    {
        return false;
    }

    RequiredExports exports;
    if (!findRequiredExports(moduleSet.instance, exports))
    {
        return false;
    }

    if (!validateContractIndex(contractIndex, moduleSet, exports))
    {
        return false;
    }

    ModuleLayout layout;
    if (!discoverMemoryLayout(layout, moduleSet, exports))
    {
        return false;
    }

    StateSnapshot previousState;
    captureState(slot, contractIndex, previousState);

    // a staged state must be refused while the resident module can still stay
    bool stateSeeded = false;
    if (!adoptStagedState(contractIndex, moduleSet, layout, previousState, stateSeeded))
    {
        return false;
    }

    unloadSlot(slot);

    adoptModule(slot, moduleSet, exports, layout);
    takeOverState(slot, contractIndex);
    configureMigration(slot, contractIndex);
    restoreState(slot, contractIndex, previousState);
    discoverRegistration(slot, exports);
    registerUserEntries(slot, contractIndex, exports);
    registerSystemProcedures(slot, contractIndex);

    slot.loaded = true;
    slot.stateSeeded = stateSeeded;
    logColorToScreen("INFO", "LITEWASM: slot loaded (" + std::to_string(slot.entryCount) + " user entries)");
    return true;
}

static inline bool wasStateSeeded(unsigned int contractIndex)
{
    const int slotOffset = engineSlotOffset(contractIndex);
    return slotOffset >= 0 && engineSlots[slotOffset].stateSeeded;
}

static inline bool hasPendingMigration(unsigned int contractIndex)
{
    const int slotOffset = engineSlotOffset(contractIndex);
    return slotOffset >= 0 && engineSlots[slotOffset].pendingOldState != nullptr;
}

[[maybe_unused]] static void runPendingMigration(unsigned int contractIndex)
{
    const int slotOffset = engineSlotOffset(contractIndex);
    if (slotOffset < 0 || !engineSlots[slotOffset].pendingOldState)
    {
        return;
    }

    EngineSlot& slot = engineSlots[slotOffset];
    QpiContextMigrateProcedureCall migrationContext(contractIndex);

    migrationContext.call(slot.pendingOldState);
    free(slot.pendingOldState);
    slot.pendingOldState = nullptr;
    slot.pendingOldStateSize = 0;
    logColorToScreen("INFO", "LITEWASM: migrate complete idx=" + std::to_string(contractIndex));
}

[[maybe_unused]] static bool initializeEngine()
{
    if (engineReady)
    {
        return true;
    }

    for (int argumentIndex = 0; argumentIndex < 5; ++argumentIndex)
    {
        dispatchCallArguments[argumentIndex] = &ffi_type_pointer;
    }

    if (ffi_prep_cif(&dispatchCallInterface, FFI_DEFAULT_ABI, 5, &ffi_type_void, dispatchCallArguments) != FFI_OK)
    {
        logToConsole(L"LITEWASM: libffi cif prep failed");
        return false;
    }

    for (int argumentIndex = 0; argumentIndex < 4; ++argumentIndex)
    {
        migrationCallArguments[argumentIndex] = &ffi_type_pointer;
    }

    if (ffi_prep_cif(&migrationCallInterface, FFI_DEFAULT_ABI, 4, &ffi_type_void, migrationCallArguments) != FFI_OK)
    {
        logToConsole(L"LITEWASM: migrate cif prep failed");
        return false;
    }

    // Per-instance allocation allows contracts with large resident state.
    RuntimeInitArgs arguments;
    setMem(&arguments, sizeof(arguments), 0);
    arguments.mem_alloc_type = Alloc_With_System_Allocator;
    arguments.native_module_name = "lhost";
    arguments.native_symbols = nativeSymbols;
    arguments.n_native_symbols = (int)nativeSymbolCount;
    if (!wasm_runtime_full_init(&arguments))
    {
        logToConsole(L"LITEWASM: WAMR init failed");
        return false;
    }

    engineReady = true;
    logToConsole(L"LITEWASM: runtime ready (WAMR + libffi)");
    return true;
}

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
