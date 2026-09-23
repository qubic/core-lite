#pragma once

// libffi registration for user and system procedure tables.
#ifdef LITE_WASM_SC

#include "extensions/wasm/runtime/module_loader.h"

namespace Wasm::Runtime
{

static void discoverRegistration(EngineSlot& slot, const RequiredExports& exports)
{
    wasm_function_inst_t contextAddress = wasm_runtime_lookup_function(slot.instance, "ctx_addr");
    if (contextAddress)
    {
        slot.contextOffset = callU32(slot.loadExecEnv, contextAddress);
    }

    slot.entryCount = callU32(slot.loadExecEnv, exports.registrationCount);
    if (slot.entryCount > WASM_MAX_USER_ENTRIES)
    {
        slot.entryCount = WASM_MAX_USER_ENTRIES;
    }

    logColorToScreen("INFO", "LITEWASM: loaded contract — " + std::to_string(slot.entryCount) + " entries, stateSize=" + std::to_string(slot.stateSize));
    // Says plainly whether the artifact carries a journal: silence would otherwise read the same as
    // agreement in the checks that follow.
    logColorToScreen("INFO",
        slot.journalBaseOffset ? "LITEWASM: state journal attached at " + std::to_string(slot.journalBaseOffset) + ", capacity "
                + std::to_string(slot.journalHeader.capacityBlocks) + " blocks"
                               : "LITEWASM: no state journal in this artifact — page tracker only");
}

static void registerUserFunction(unsigned int contractIndex, uint16_t inputType, const EntryInfo& entry, void* code)
{
    contractUserFunctions[contractIndex][inputType] = (USER_FUNCTION)code;
    contractUserFunctionInputSizes[contractIndex][inputType] = (uint16_t)entry.inputSize;
    contractUserFunctionOutputSizes[contractIndex][inputType] = (uint16_t)entry.outputSize;
    contractUserFunctionLocalsSizes[contractIndex][inputType] = 0;
}

static void registerUserProcedure(unsigned int contractIndex, uint16_t inputType, const EntryInfo& entry, void* code)
{
    contractUserProcedures[contractIndex][inputType] = (USER_PROCEDURE)code;
    contractUserProcedureInputSizes[contractIndex][inputType] = (uint16_t)entry.inputSize;
    contractUserProcedureOutputSizes[contractIndex][inputType] = (uint16_t)entry.outputSize;
    contractUserProcedureLocalsSizes[contractIndex][inputType] = 0;

    // Oracle notifications use the synthetic procedure ID registered by native contracts.
    if (userProcedureRegistry)
    {
        const unsigned int fullProcedureId = (contractIndex << 22) | inputType;
        userProcedureRegistry->add(
            fullProcedureId,
            {
                (USER_PROCEDURE)code,
                contractIndex,
                0u,
                (uint16_t)entry.inputSize,
                (uint16_t)entry.outputSize,
            });
    }
}

static void registerUserEntries(EngineSlot& slot, unsigned int contractIndex, const RequiredExports& exports)
{
    for (uint32_t entryIndex = 0; entryIndex < slot.entryCount; ++entryIndex)
    {
        uint32_t arguments[2] = {
            entryIndex,
            slot.ioBaseOffset,
        };
        wasm_runtime_call_wasm(slot.loadExecEnv, exports.registrationInfo, 2, arguments);

        auto* entry = (EntryInfo*)wasm_runtime_addr_app_to_native(slot.instance, slot.ioBaseOffset);
        const uint16_t inputType = (uint16_t)entry->inputType;
        slot.entryBindings[entryIndex] = {
            contractIndex,
            inputType,
            (DispatchKind)entry->kind,
        };

        void* code = nullptr;
        ffi_closure* closure = (ffi_closure*)ffi_closure_alloc(sizeof(ffi_closure), &code);
        if (!closure || ffi_prep_closure_loc(closure, &dispatchCallInterface, dispatchClosure, &slot.entryBindings[entryIndex], code) != FFI_OK)
        {
            logToConsole(L"LITEWASM: closure alloc failed");
            continue;
        }

        slot.entryClosures[entryIndex] = closure;
        if (entry->kind == WASM_ENTRY_FUNCTION)
        {
            registerUserFunction(contractIndex, inputType, *entry, code);
        }
        else
        {
            registerUserProcedure(contractIndex, inputType, *entry, code);
        }
    }
}

static uint32_t callWithU32Argument(wasm_exec_env_t execEnv, wasm_function_inst_t function, uint32_t argument)
{
    if (!function)
    {
        return 0;
    }

    uint32_t arguments[1] = { argument };

    wasm_runtime_call_wasm(execEnv, function, 1, arguments);
    return arguments[0];
}

// a row's id indexes contractSystemProcedures, which the node walks by SystemProcedureID.
#define WASM_ASSERT_SYSTEM_PROCEDURE_ROW(symbol, id, method, emptyMember) \
    static_assert((unsigned int)::symbol == id, "ABI row id differs from SystemProcedureID::" #symbol);
WASM_SYSTEM_PROCEDURE_ROWS(WASM_ASSERT_SYSTEM_PROCEDURE_ROW)
#undef WASM_ASSERT_SYSTEM_PROCEDURE_ROW
static_assert((unsigned int)::contractSystemProcedureCount == (unsigned int)WASM_SYSTEM_PROCEDURE_COUNT, "ABI rows do not cover SystemProcedureID");

static void registerSystemProcedures(EngineSlot& slot, unsigned int contractIndex)
{
    wasm_function_inst_t maskFunction = wasm_runtime_lookup_function(slot.instance, "reg_sysproc_mask");
    wasm_function_inst_t localsSizeFunction = wasm_runtime_lookup_function(slot.instance, "sysproc_locals_size");
    wasm_function_inst_t inputSizeFunction = wasm_runtime_lookup_function(slot.instance, "sysproc_in_size");
    wasm_function_inst_t outputSizeFunction = wasm_runtime_lookup_function(slot.instance, "sysproc_out_size");
    if (!maskFunction)
    {
        return;
    }

    const uint32_t mask = callU32(slot.loadExecEnv, maskFunction);
    for (uint32_t systemProcedure = 0; systemProcedure < WASM_SYSTEM_PROCEDURE_COUNT; ++systemProcedure)
    {
        if (!(mask & (1u << systemProcedure)))
        {
            continue;
        }

        slot.systemBindings[systemProcedure] = {
            contractIndex,
            (uint16_t)systemProcedure,
            DispatchKind::SystemProcedure,
        };

        void* code = nullptr;
        ffi_closure* closure = (ffi_closure*)ffi_closure_alloc(sizeof(ffi_closure), &code);
        if (!closure || ffi_prep_closure_loc(closure, &dispatchCallInterface, dispatchClosure, &slot.systemBindings[systemProcedure], code) != FFI_OK)
        {
            continue;
        }

        slot.systemClosures[systemProcedure] = closure;
        contractSystemProcedures[contractIndex][systemProcedure] = (SYSTEM_PROCEDURE)code;
        contractSystemProcedureLocalsSizes[contractIndex][systemProcedure] = (uint16_t)callWithU32Argument(slot.loadExecEnv, localsSizeFunction, systemProcedure);
        slot.systemInputSizes[systemProcedure] = (uint16_t)callWithU32Argument(slot.loadExecEnv, inputSizeFunction, systemProcedure);
        slot.systemOutputSizes[systemProcedure] = (uint16_t)callWithU32Argument(slot.loadExecEnv, outputSizeFunction, systemProcedure);
    }
}

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
