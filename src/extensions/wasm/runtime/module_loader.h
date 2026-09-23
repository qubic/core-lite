#pragma once

// WAMR module loading, export discovery, state takeover, and migration setup.
#ifdef LITE_WASM_SC

#include "extensions/wasm/runtime/dispatch.h"
#include "extensions/wasm/runtime/state_backend.h"

namespace Wasm::Runtime
{

static void captureState(const EngineSlot& slot, unsigned int contractIndex, StateSnapshot& snapshot)
{
    if (!slot.instance || !slot.stateStubReleased || !slot.stateSize || !contractStates[contractIndex])
    {
        return;
    }

    snapshot.size = slot.stateSize;
    snapshot.buffer.allocate(snapshot.size);
    if (snapshot.buffer.data)
    {
        copyMem(snapshot.buffer.data, contractStates[contractIndex], snapshot.size);
    }
}

static std::string lastLoadError;

// every refusal to load names its reason here, so a deploy can report it to the client that sent the module.
static bool loadFail(const std::string& reason)
{
    lastLoadError = reason;
    logColorToScreen("ERROR", "LITEWASM: " + reason);
    return false;
}

// swaps a staged initial state in for the captured one; false when it fits neither the new StateData nor its MIGRATE input
static bool adoptStagedState(unsigned int contractIndex, const ModuleResources& moduleSet, const ModuleLayout& layout, StateSnapshot& snapshot, bool& seeded)
{
    unsigned char* stagedBytes = nullptr;
    unsigned long long stagedSize = 0;

    seeded = false;
    if (!takeStagedState(contractIndex, stagedBytes, stagedSize, /*dropIncomplete=*/true))
    {
        return true;
    }

    wasm_function_inst_t hasMigration = wasm_runtime_lookup_function(moduleSet.instance, "has_migrate");
    wasm_function_inst_t oldStateSize = wasm_runtime_lookup_function(moduleSet.instance, "migrate_old_state_size");
    const bool migrates = hasMigration && oldStateSize && callU32(moduleSet.execEnv, hasMigration);
    const uint32_t migrationOldStateSize = migrates ? callU32(moduleSet.execEnv, oldStateSize) : 0;

    if (stagedSize != layout.stateSize && !(migrates && stagedSize == migrationOldStateSize))
    {
        free(stagedBytes);
        return loadFail("staged state is " + std::to_string(stagedSize) + " bytes, contract state is " + std::to_string(layout.stateSize)
            + (migrates ? " (MIGRATE reads " + std::to_string(migrationOldStateSize) + ")" : ""));
    }

    snapshot.buffer.allocate(0);
    snapshot.buffer.data = stagedBytes;
    snapshot.size = (uint32_t)stagedSize;
    seeded = true;
    logColorToScreen("INFO", "LITEWASM: state seeded from staged bytes — " + std::to_string(stagedSize) + " bytes");
    return true;
}

static void unloadSlot(EngineSlot& slot)
{
    if (slot.loadExecEnv)
    {
        wasm_runtime_destroy_exec_env(slot.loadExecEnv);
        slot.loadExecEnv = nullptr;
    }

    if (slot.instance)
    {
        wasm_runtime_deinstantiate(slot.instance);
        slot.instance = nullptr;
    }

    if (slot.module)
    {
        wasm_runtime_unload(slot.module);
        slot.module = nullptr;
    }

    if (slot.moduleBuffer)
    {
        free(slot.moduleBuffer);
        slot.moduleBuffer = nullptr;
    }

    for (uint32_t entryIndex = 0; entryIndex < slot.entryCount; ++entryIndex)
    {
        // Clear the shared-core registration row before freeing the closure, so a redeploy that drops
        // this entry cannot leave a non-null pointer into freed trampoline code that dispatch would jump to.
        const EntryBinding& binding = slot.entryBindings[entryIndex];
        if (binding.kind == DispatchKind::UserFunction)
        {
            contractUserFunctions[binding.contractIndex][binding.inputType] = nullptr;
            contractUserFunctionInputSizes[binding.contractIndex][binding.inputType] = 0;
            contractUserFunctionOutputSizes[binding.contractIndex][binding.inputType] = 0;
            contractUserFunctionLocalsSizes[binding.contractIndex][binding.inputType] = 0;
        }
        else if (binding.kind == DispatchKind::UserProcedure)
        {
            contractUserProcedures[binding.contractIndex][binding.inputType] = nullptr;
            contractUserProcedureInputSizes[binding.contractIndex][binding.inputType] = 0;
            contractUserProcedureOutputSizes[binding.contractIndex][binding.inputType] = 0;
            contractUserProcedureLocalsSizes[binding.contractIndex][binding.inputType] = 0;
        }

        if (slot.entryClosures[entryIndex])
        {
            ffi_closure_free(slot.entryClosures[entryIndex]);
            slot.entryClosures[entryIndex] = nullptr;
        }
    }

    for (uint32_t systemProcedure = 0; systemProcedure < WASM_SYSTEM_PROCEDURE_COUNT; ++systemProcedure)
    {
        if (slot.systemClosures[systemProcedure])
        {
            // Same as above for a system procedure the redeploy may have dropped.
            const EntryBinding& binding = slot.systemBindings[systemProcedure];
            contractSystemProcedures[binding.contractIndex][systemProcedure] = nullptr;
            contractSystemProcedureLocalsSizes[binding.contractIndex][systemProcedure] = 0;
            ffi_closure_free(slot.systemClosures[systemProcedure]);
            slot.systemClosures[systemProcedure] = nullptr;
        }
    }

    if (slot.migrationClosure)
    {
        ffi_closure_free(slot.migrationClosure);
        slot.migrationClosure = nullptr;
    }

    slot.hasMigration = false;
    slot.entryCount = 0;
    slot.loaded = false;
}

static bool prepareModuleBuffer(ModuleResources& moduleSet, const unsigned char* bytes, unsigned int length)
{
    // WAMR mutates and retains this buffer for the module lifetime.
    moduleSet.moduleBuffer = (unsigned char*)malloc(length);
    if (!moduleSet.moduleBuffer)
    {
        return loadFail("oom");
    }

    copyMem(moduleSet.moduleBuffer, bytes, length);
    return true;
}

static bool loadModule(unsigned int length, ModuleResources& moduleSet)
{
    char error[192] = {};

    moduleSet.module = wasm_runtime_load(moduleSet.moduleBuffer, length, error, sizeof(error));
    if (!moduleSet.module)
    {
        return loadFail(std::string("load failed: ") + error);
    }

    moduleSet.instance = wasm_runtime_instantiate(moduleSet.module, 64 * 1024, 1024 * 1024, error, sizeof(error));
    if (!moduleSet.instance)
    {
        return loadFail(std::string("instantiate failed: ") + error);
    }

    moduleSet.execEnv = wasm_runtime_create_exec_env(moduleSet.instance, 64 * 1024);
    if (!moduleSet.execEnv)
    {
        return loadFail("exec env alloc failed");
    }

    return true;
}

static bool findRequiredExports(wasm_module_inst_t instance, RequiredExports& exports)
{
    exports.contractIndex = wasm_runtime_lookup_function(instance, "contract_index");
    exports.stateAddress = wasm_runtime_lookup_function(instance, "state_addr");
    exports.stateSize = wasm_runtime_lookup_function(instance, "state_size");
    exports.ioBase = wasm_runtime_lookup_function(instance, "io_base");
    exports.registrationCount = wasm_runtime_lookup_function(instance, "reg_count");
    exports.registrationInfo = wasm_runtime_lookup_function(instance, "reg_info");
    exports.dispatch = wasm_runtime_lookup_function(instance, "dispatch");

    if (!exports.contractIndex || !exports.stateAddress || !exports.stateSize || !exports.ioBase || !exports.registrationCount || !exports.registrationInfo || !exports.dispatch)
    {
        return loadFail("missing required export");
    }

    return true;
}

static bool validateContractIndex(unsigned int targetContractIndex, const ModuleResources& moduleSet, const RequiredExports& exports)
{
    wasm_valkind_t resultType = WASM_I32;
    if (wasm_func_get_param_count(exports.contractIndex, moduleSet.instance) != 0 || wasm_func_get_result_count(exports.contractIndex, moduleSet.instance) != 1)
    {
        return loadFail("contract_index must have signature () -> i32");
    }

    wasm_func_get_result_types(exports.contractIndex, moduleSet.instance, &resultType);
    if (resultType != WASM_I32)
    {
        return loadFail("contract_index must have signature () -> i32");
    }

    uint32_t arguments[1] = { 0 };
    if (!wasm_runtime_call_wasm(moduleSet.execEnv, exports.contractIndex, 0, arguments))
    {
        const char* exception = wasm_runtime_get_exception(moduleSet.instance);
        return loadFail(std::string("contract_index() failed: ") + (exception ? exception : "unknown trap"));
    }

    const unsigned int compiledContractIndex = arguments[0];
    if (compiledContractIndex != targetContractIndex)
    {
        return loadFail("artifact slot mismatch: compiled " + std::to_string(compiledContractIndex) + ", target " + std::to_string(targetContractIndex));
    }

    return true;
}

static bool callU32Checked(const ModuleResources& moduleSet, wasm_function_inst_t function, uint32_t& result)
{
    uint32_t arguments[1] = { 0 };
    if (!wasm_runtime_call_wasm(moduleSet.execEnv, function, 0, arguments))
    {
        return loadFail("metadata export trapped");
    }

    result = arguments[0];
    return true;
}

static bool discoverMemoryLayout(ModuleLayout& layout, const ModuleResources& moduleSet, const RequiredExports& exports)
{
    if (!callU32Checked(moduleSet, exports.stateAddress, layout.stateOffset) || !callU32Checked(moduleSet, exports.stateSize, layout.stateSize) || !callU32Checked(moduleSet, exports.ioBase, layout.ioBaseOffset))
    {
        return false;
    }

    wasm_global_inst_t legacyArenaTop = {};
    if (wasm_runtime_get_export_global_inst(moduleSet.instance, "arena_top", &legacyArenaTop))
    {
        return loadFail("legacy arena_top export is not supported");
    }

    wasm_function_inst_t ioSize = wasm_runtime_lookup_function(moduleSet.instance, "io_size");
    uint32_t ioCapacity = 0;
    if (ioSize && (!callU32Checked(moduleSet, ioSize, ioCapacity) || ioCapacity < WASM_IO_CAPACITY))
    {
        return loadFail("contract io region too small for the engine carve (rebuild the contract)");
    }

    return true;
}

static void adoptModule(EngineSlot& slot, ModuleResources& moduleSet, const RequiredExports& exports, const ModuleLayout& layout)
{
    slot.moduleBuffer = moduleSet.moduleBuffer;
    slot.module = moduleSet.module;
    slot.instance = moduleSet.instance;
    slot.loadExecEnv = moduleSet.execEnv;
    slot.dispatchFunction = exports.dispatch;
    slot.stateOffset = layout.stateOffset;
    slot.stateSize = layout.stateSize;
    slot.ioBaseOffset = layout.ioBaseOffset;
    slot.journalOverflowed = false;
    attachJournal(
        slot.instance, slot.loadExecEnv, slot.ioBaseOffset, (uint32_t)WASM_IO_CAPACITY, slot.stateSize, slot.journalBaseOffset, slot.journalHeader);
    moduleSet.release();
}

static void takeOverState(EngineSlot& slot, unsigned int contractIndex)
{
    if (!slot.stateStubReleased)
    {
        // The adapter releases the reserve according to the active state backend.
        transferContractStateToWasm(contractIndex);
        slot.stateStubReleased = true;
    }

    contractStates[contractIndex] = (unsigned char*)wasm_runtime_addr_app_to_native(slot.instance, slot.stateOffset);
}

static void configureMigration(EngineSlot& slot, unsigned int contractIndex)
{
    slot.hasMigration = false;
    slot.migrationOldStateSize = 0;
    slot.migrationLocalsSize = 0;
    contractMigrateProcedures[contractIndex] = nullptr;
    contractMigrateOldStateSizes[contractIndex] = 0;
    contractMigrateLocalsSizes[contractIndex] = 0;

    wasm_function_inst_t hasMigration = wasm_runtime_lookup_function(slot.instance, "has_migrate");
    if (!hasMigration || !callU32(slot.loadExecEnv, hasMigration))
    {
        return;
    }

    wasm_function_inst_t oldStateSize = wasm_runtime_lookup_function(slot.instance, "migrate_old_state_size");
    wasm_function_inst_t localsSize = wasm_runtime_lookup_function(slot.instance, "migrate_locals_size");

    slot.migrationOldStateSize = oldStateSize ? callU32(slot.loadExecEnv, oldStateSize) : 0;
    slot.migrationLocalsSize = localsSize ? callU32(slot.loadExecEnv, localsSize) : 0;
    slot.migrationBinding = {
        contractIndex,
        0,
        DispatchKind::Migration,
    };

    void* code = nullptr;
    ffi_closure* closure = (ffi_closure*)ffi_closure_alloc(sizeof(ffi_closure), &code);
    if (closure && ffi_prep_closure_loc(closure, &migrationCallInterface, migrationClosure, &slot.migrationBinding, code) == FFI_OK)
    {
        slot.migrationClosure = closure;
        slot.hasMigration = true;
        contractMigrateProcedures[contractIndex] = (MIGRATE_PROCEDURE)code;
        contractMigrateOldStateSizes[contractIndex] = slot.migrationOldStateSize;
        contractMigrateLocalsSizes[contractIndex] = slot.migrationLocalsSize;
        return;
    }

    if (closure)
    {
        ffi_closure_free(closure);
        logToConsole(L"LITEWASM: migrate closure alloc failed");
    }
}

static void restoreState(EngineSlot& slot, unsigned int contractIndex, StateSnapshot& snapshot)
{
    if (!snapshot.buffer.data)
    {
        return;
    }

    if (slot.hasMigration && slot.migrationOldStateSize == snapshot.size)
    {
        slot.pendingOldState = snapshot.buffer.release();
        slot.pendingOldStateSize = snapshot.size;
        logColorToScreen("INFO", "LITEWASM: migrate pending — old state " + std::to_string(snapshot.size) + " bytes");
        return;
    }

    const uint32_t preservedSize = snapshot.size < slot.stateSize ? snapshot.size : slot.stateSize;
    copyMem(contractStates[contractIndex], snapshot.buffer.data, preservedSize);

    if (slot.migrationOldStateSize && slot.migrationOldStateSize != snapshot.size)
    {
        logColorToScreen("WARNING", "LITEWASM: migrate OldStateData size " + std::to_string(slot.migrationOldStateSize) + " != live state " + std::to_string(snapshot.size) + " — raw-preserved instead");
        return;
    }

    logColorToScreen("INFO", "LITEWASM: state preserved across upgrade — " + std::to_string(preservedSize) + " bytes");
}

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
