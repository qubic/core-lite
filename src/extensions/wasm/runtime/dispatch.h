#pragma once

// WAMR call dispatch, nested environment binding, tracing, and migration calls.
#ifdef LITE_WASM_SC

#include "extensions/wasm/runtime/engine_state.h"
#include "extensions/wasm/runtime/state_write_tracker.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

namespace Wasm::Runtime
{

struct EnvironmentScope
{
    wasm_exec_env_t execEnv = nullptr;
    wasm_exec_env_t parentExecEnv = nullptr;
    bool ownsExecEnv = false;
    bool ready = false;
    wasm_module_inst_t savedInstance = nullptr;
    void* savedUserData = nullptr;

    explicit EnvironmentScope(const EngineSlot& slot)
    {
        // WAMR trap backtraces require every frame in an environment to belong to one module.
        if (currentEnvironment && wasm_runtime_get_module_inst(currentEnvironment) == slot.instance)
        {
            execEnv = currentEnvironment;
            savedInstance = wasm_runtime_get_module_inst(execEnv);
            savedUserData = wasm_runtime_get_user_data(execEnv);
            wasm_runtime_set_module_inst(execEnv, slot.instance);
            ready = true;
            return;
        }

        ensureThreadEnvironment();
        parentExecEnv = currentEnvironment;
        execEnv = wasm_runtime_create_exec_env(slot.instance, 64 * 1024);
        if (!execEnv)
        {
            return;
        }

        currentEnvironment = execEnv;
        ownsExecEnv = true;
        ready = true;
    }

    ~EnvironmentScope()
    {
        if (!ready)
        {
            return;
        }

        if (ownsExecEnv)
        {
            wasm_runtime_set_user_data(execEnv, nullptr);
            wasm_runtime_destroy_exec_env(execEnv);
            currentEnvironment = parentExecEnv;
            return;
        }

        wasm_runtime_set_user_data(execEnv, savedUserData);
        wasm_runtime_set_module_inst(execEnv, savedInstance);
    }

    EnvironmentScope(const EnvironmentScope&) = delete;
    EnvironmentScope& operator=(const EnvironmentScope&) = delete;
};

static CallContext createCallContext(const void* context, uint32_t arenaStart, uint32_t arenaLimit)
{
    CallContext callContext;

    callContext.ctx = context;
    callContext.arenaStart = arenaStart;
    callContext.arenaTop = arenaStart;
    callContext.arenaLimit = arenaLimit;

    if (context)
    {
        copyMem(&callContext.realContext, context, sizeof(callContext.realContext));
    }

    // a warp lasts for one root dispatch, so nested frames see their caller's; the depth is bumped after this.
    if (dispatchDepth == 0)
    {
        clearCheatWarp();
    }

    return callContext;
}

/** Lets the lhost adapters reach the journal, so a host write into state is recorded like a store. */
static void bindJournal(CallContext& callContext, const EngineSlot& slot)
{
    callContext.journalBaseOffset = slot.journalBaseOffset;
    callContext.stateOffset = slot.stateOffset;
    callContext.journalHeader = slot.journalBaseOffset ? &slot.journalHeader : nullptr;
    callContext.guestContextOffset = slot.contextOffset;
}

static void bindEnvironment(wasm_exec_env_t execEnv, CallContext& callContext)
{
    wasm_runtime_set_user_data(execEnv, &callContext);
}

struct CallContextScope
{
    wasm_exec_env_t execEnv;
    CallContext*& activeContext;
    CallContext* savedContext;
    void* savedUserData;

    CallContextScope(wasm_exec_env_t environment, int slotOffset, CallContext& callContext)
        : activeContext(slotCallContexts[slotOffset])
    {
        execEnv = environment;
        savedContext = activeContext;
        savedUserData = wasm_runtime_get_user_data(execEnv);
        activeContext = &callContext;
        wasm_runtime_set_user_data(execEnv, &callContext);
    }

    ~CallContextScope()
    {
        activeContext = savedContext;
        wasm_runtime_set_user_data(execEnv, savedUserData);
    }

    CallContextScope(const CallContextScope&) = delete;
    CallContextScope& operator=(const CallContextScope&) = delete;
};

struct GuestContextScope
{
    const EngineSlot& slot;
    bool restore = false;
    unsigned char savedContext[sizeof(QPI::QpiContext)] = {};

    GuestContextScope(const EngineSlot& engineSlot, bool nested)
        : slot(engineSlot)
    {
        if (!nested || !slot.contextOffset)
        {
            return;
        }

        const void* guestContext = wasm_runtime_addr_app_to_native(slot.instance, slot.contextOffset);
        if (guestContext)
        {
            copyMem(savedContext, guestContext, sizeof(savedContext));
            restore = true;
        }
    }

    ~GuestContextScope()
    {
        if (restore)
        {
            copyMem(wasm_runtime_addr_app_to_native(slot.instance, slot.contextOffset), savedContext, sizeof(savedContext));
        }
    }

    GuestContextScope(const GuestContextScope&) = delete;
    GuestContextScope& operator=(const GuestContextScope&) = delete;
};

class DispatchFrameScope
{
    GuestContextScope nestedGuestRestore;
    CallContext context;
    DispatchDepthScope dispatchDepth;
    CallContextScope contextBinding;

public:
    DispatchFrameScope(const EngineSlot& slot, wasm_exec_env_t execEnv, int slotOffset, const QPI::QpiContext* qpiContext, const MemoryLayout& layout,
        uint32_t arenaLimit, bool nested)
        : nestedGuestRestore(slot, nested),
          context(createCallContext(qpiContext, layout.arenaOffset, arenaLimit)),
          dispatchDepth(slotCallDepth[slotOffset]),
          contextBinding(execEnv, slotOffset, context)
    {
        bindJournal(context, slot);
    }

    CallContext& callContext()
    {
        return context;
    }
};

static void prepareMemory(const EngineSlot& slot, const MemoryLayout& layout, const void* context, const void* input, const IoSizes& sizes)
{
    if (context && slot.contextOffset)
    {
        copyMem(wasm_runtime_addr_app_to_native(slot.instance, slot.contextOffset), context, sizeof(QPI::QpiContext));
    }

    if (sizes.input)
    {
        copyMem(wasm_runtime_addr_app_to_native(slot.instance, layout.inputOffset), input, sizes.input);
    }

    setMem(wasm_runtime_addr_app_to_native(slot.instance, layout.outputOffset), sizes.output ? sizes.output : 1, 0);
    zeroEntryLocals(wasm_runtime_addr_app_to_native(slot.instance, layout.localsOffset));
}

static void finalizeMemory(const EngineSlot& slot, const MemoryLayout& layout, uint32_t contractIndex, DispatchKind kind, void* output, const IoSizes& sizes)
{
    if (sizes.output)
    {
        copyMem(output, wasm_runtime_addr_app_to_native(slot.instance, layout.outputOffset), sizes.output);
    }

    contractStates[contractIndex] = (unsigned char*)wasm_runtime_addr_app_to_native(slot.instance, slot.stateOffset);

    if (kind != DispatchKind::UserFunction)
    {
        hostServices.markDirty(contractIndex);
    }
}

struct DispatchTrace
{
    bool enabled = false;
    bool tracksWrites = false;
    TraceEntry entry;
    unsigned char* state = nullptr;
    std::chrono::steady_clock::time_point startedAt;
};

// What a frame leaves behind for the closure that called it, once every scope of the frame is gone.
struct DispatchOutcome
{
    bool trapped = false;
    bool rootFrame = false;
    uint32_t abortCode = 0;
    std::string trapText;
};

static void beginDispatchTrace(const EngineSlot& slot, uint32_t contractIndex, uint16_t inputType, DispatchKind kind, const void* context, const void* input,
    const IoSizes& sizes, CallContext& callContext, DispatchTrace& trace)
{
    trace.enabled = traceEnabled();
    if (!trace.enabled)
    {
        return;
    }

    trace.tracksWrites = kind != DispatchKind::UserFunction;
    trace.entry.tick = hostServices.tick(context);
    trace.entry.contractIndex = contractIndex;
    trace.entry.inputType = inputType;
    trace.entry.kind = (unsigned char)kind;
    trace.entry.inputSize = sizes.input;
    trace.entry.outputSize = sizes.output;
    trace.entry.stateSize = slot.stateSize;

    if (sizes.input && input)
    {
        const auto* bytes = static_cast<const unsigned char*>(input);
        trace.entry.input.assign(bytes, bytes + sizes.input);
    }

    if (kind == DispatchKind::UserProcedure)
    {
        auto* procedureContext = (const QPI::QpiContextProcedureCall*)context;
        trace.entry.invocator = procedureContext->invocator();
        trace.entry.invocationReward = procedureContext->invocationReward();
    }

    callContext.trace = &trace.entry;
    trace.state = (unsigned char*)wasm_runtime_addr_app_to_native(slot.instance, slot.stateOffset);
    trace.startedAt = std::chrono::steady_clock::now();
    pushTraceFrame(trace.entry);
}

static void finishDispatchTrace(const EngineSlot& slot, const MemoryLayout& layout, const IoSizes& sizes, CallContext& callContext, DispatchTrace& trace)
{
    if (!trace.enabled)
    {
        return;
    }

    trace.entry.ok = slot.lastTrap.empty();
    trace.entry.trap = slot.lastTrap;
    trace.entry.executionNanoseconds = (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - trace.startedAt).count();

    if (sizes.output)
    {
        const void* outputPtr = wasm_runtime_addr_app_to_native(slot.instance, layout.outputOffset);
        if (outputPtr)
        {
            const auto* bytes = static_cast<const unsigned char*>(outputPtr);
            trace.entry.output.assign(bytes, bytes + sizes.output);
        }
    }

    // Post-dispatch version: odd inside the write window, so round up to the next even.
    const unsigned long long writeSeq = g_stateSeq[trace.entry.contractIndex].load(std::memory_order_acquire);
    trace.entry.stateVersion = writeSeq + (writeSeq & 1ull);

    callContext.trace = nullptr;
    popTraceFrame();
    commitTrace(trace.entry);
}

static bool invokeDispatch(EngineSlot& slot, wasm_exec_env_t execEnv, DispatchKind kind, uint16_t inputType, uint32_t inputOffset, uint32_t outputOffset,
    uint32_t localsOffset)
{
    uint32_t arguments[5] = {
        (uint32_t)kind,
        inputType,
        inputOffset,
        outputOffset,
        localsOffset,
    };

    return wasm_runtime_call_wasm(execEnv, slot.dispatchFunction, 5, arguments);
}

// An abort reads the same in the trace and the registry as it does in the fault record; a real trap keeps
// the entry that raised it, which the WAMR text alone does not carry.
static void handleDispatchResult(EngineSlot& slot, uint32_t contractIndex, const std::string& frameLabel, bool succeeded, DispatchOutcome& outcome)
{
    if (succeeded)
    {
        slot.lastTrap.clear();
        return;
    }

    const char* exception = wasm_runtime_get_exception(slot.instance);
    outcome.trapped = true;
    outcome.abortCode = pendingAbortCode;
    outcome.trapText = exception ? exception : "trap";
    slot.lastTrap = outcome.abortCode ? abortMessage(outcome.abortCode) : frameLabel + " — " + outcome.trapText;

    // WAMR prints the original Wasm offsets before unwinding so a debugger can map them through DWARF.
    logColorToScreen("ERROR", "LITEWASM dispatch trap idx=" + std::to_string(contractIndex) + " " + slot.lastTrap);
    wasm_runtime_clear_exception(slot.instance);
}

// Runs once every scope of the frame has unwound, because a halt parks this thread and a function abort
// long-jumps back to the query; neither may skip a destructor. Nested frames settle nothing: a nested
// trap is the caller's to recover, a nested abort is propagated frame by frame up to the root.
static void settleRootFailure(const DispatchOutcome& outcome, DispatchKind kind, uint32_t contractIndex, uint16_t inputType, const void* context)
{
    if (!outcome.trapped || !outcome.rootFrame)
    {
        return;
    }

    pendingAbortCode = 0;
    if (kind != DispatchKind::UserFunction && !outcome.abortCode)
    {
        recordFault(contractIndex, (unsigned char)kind, inputType, outcome.trapText, hostServices.tick(context), hostServices.epoch(context));
    }
    hostServices.abort(context, outcome.abortCode ? outcome.abortCode : WASM_TRAP_ERROR_CODE);
}

/** Regions flattened per byte: the tracker splits a run at page boundaries, the journal at block ones. */
static std::map<unsigned int, std::pair<std::string, std::string>> changedStateBytes(const std::vector<StateRegionTrace>& regions)
{
    std::map<unsigned int, std::pair<std::string, std::string>> bytes;
    for (const StateRegionTrace& region : regions)
    {
        for (size_t index = 0; index * 2u + 1u < region.before.size(); index++)
        {
            bytes[region.offset + (unsigned int)index] = {region.before.substr(index * 2u, 2u), region.after.substr(index * 2u, 2u)};
        }
    }
    return bytes;
}

/** QINIT_STATE_DIFF=verify arms the page tracker beside the journal and compares the two every dispatch. */
static bool verifyingStateDiff()
{
    static const bool enabled = []
    {
        const char* mode = getenv("QINIT_STATE_DIFF");
        const bool verify = mode && strcmp(mode, "verify") == 0;
        if (verify)
        {
            logColorToScreen("INFO", "LITEWASM state diff verify mode: journal and page tracker both armed");
        }

        return verify;
    }();

    return enabled;
}

/**
 * Fills the trace with what the call changed in contract state. The journal is the source whenever the
 * artifact carries one; the page tracker only covers the dispatches it cannot.
 */
static void recordStateDiff(EngineSlot& slot, uint32_t contractIndex, StateJournalScope& journalScope, TraceEntry& traceEntry)
{
    if (!journalScope.engaged)
    {
        // No journal for this dispatch, so the tracker already filled the trace.
        return;
    }

    std::vector<StateRegionTrace> journalDiff;
    if (!journalScope.finish(journalDiff))
    {
        // Overflowed. This call's before-images are gone; the next one falls back to the tracker.
        slot.journalOverflowed = true;
        traceEntry.stateTruncated = true;
        return;
    }

    if (verifyingStateDiff() && !traceEntry.stateTruncated && changedStateBytes(journalDiff) != changedStateBytes(traceEntry.stateDiff))
    {
        logColorToScreen("ERROR",
            "LITEWASM state journal disagrees with the write tracker idx=" + std::to_string(contractIndex) + " journal=" + std::to_string(journalDiff.size())
                + " tracker=" + std::to_string(traceEntry.stateDiff.size()));
    }

    traceEntry.stateDiff = std::move(journalDiff);
    // The journal covered the call whole, so a tracker-side truncation no longer describes this entry.
    traceEntry.stateTruncated = false;
}

static DispatchOutcome dispatchMigration(uint32_t contractIndex, int slotOffset, EngineSlot& slot, const void* context, const void* oldState)
{
    DispatchOutcome outcome;
    const uint32_t oldStateSize = slot.migrationOldStateSize;
    if (oldStateSize > WASM_ARENA_SIZE)
    {
        logColorToScreen("ERROR", "LITEWASM migrate old-state exceeds arena idx=" + std::to_string(contractIndex));
        return outcome;
    }

    EnvironmentScope environment(slot);
    if (!environment.ready)
    {
        return outcome;
    }

    const MemoryLayout layout = resolveMemoryLayout(slot);
    uint32_t arenaLimit = 0;
    if (!resolveArenaLimit(layout, arenaLimit))
    {
        logColorToScreen("ERROR", "LITEWASM migrate arena exceeds Wasm32 memory idx=" + std::to_string(contractIndex));
        return outcome;
    }

    const uint32_t migrationArenaStart = layout.arenaOffset + ((oldStateSize + 15u) & ~15u);
    CallContext callContext = createCallContext(context, migrationArenaStart, arenaLimit);
    callContext.contractIndex = contractIndex;
    callContext.inputType = 0;
    callContext.kind = (unsigned char)DispatchKind::Migration;
    bindJournal(callContext, slot);
    DispatchDepthScope slotDepth(slotCallDepth[slotOffset]);
    DispatchDepthScope frameDepth(dispatchDepth);
    outcome.rootFrame = dispatchDepth == 1;
    // Migration rewrites the whole state and never nests.
    StateWriteSeqScope stateSeq(true, contractIndex);

    bindEnvironment(environment.execEnv, callContext);

    if (context && slot.contextOffset)
    {
        copyMem(wasm_runtime_addr_app_to_native(slot.instance, slot.contextOffset), context, sizeof(QPI::QpiContext));
    }

    if (oldStateSize)
    {
        copyMem(wasm_runtime_addr_app_to_native(slot.instance, layout.arenaOffset), oldState, oldStateSize);
    }

    setMem(wasm_runtime_addr_app_to_native(slot.instance, slot.stateOffset), slot.stateSize, 0);
    setMem(wasm_runtime_addr_app_to_native(slot.instance, layout.localsOffset), WASM_LOCALS_CAPACITY, 0);

    // Traced after the zero-fill: the page tracker protects the state region, so a host write into it
    // would fault, and the before-image the reader sees is the migration's real starting point.
    DispatchTrace trace;
    const IoSizes sizes{oldStateSize, 0};
    beginDispatchTrace(slot, contractIndex, 0, DispatchKind::Migration, context, oldState, sizes, callContext, trace);

    unsigned char* linearMemory = (unsigned char*)wasm_runtime_addr_app_to_native(slot.instance, slot.ioBaseOffset) - slot.ioBaseOffset;
    // A migration always runs at the top of a tick, so there is no parent frame whose journal it could reset.
    const bool journalUsable = slot.journalBaseOffset && !slot.journalOverflowed;
    StateJournalScope journalScope(trace.tracksWrites && journalUsable, linearMemory, slot.journalBaseOffset, slot.stateOffset, slot.journalHeader);
    StateWriteScope pageProtection(trace.tracksWrites && (!journalUsable || verifyingStateDiff()), trace.state, slot.stateSize);

    const bool succeeded = invokeDispatch(slot, environment.execEnv, DispatchKind::Migration, 0, layout.arenaOffset, 0, layout.localsOffset);
    handleDispatchResult(slot, contractIndex, "MIGRATE", succeeded, outcome);
    pageProtection.finish(trace.entry);
    recordStateDiff(slot, contractIndex, journalScope, trace.entry);
    finishDispatchTrace(slot, layout, sizes, callContext, trace);

    contractStates[contractIndex] = (unsigned char*)wasm_runtime_addr_app_to_native(slot.instance, slot.stateOffset);
    hostServices.markDirty(contractIndex);
    return outcome;
}

static DispatchOutcome dispatchCall(uint32_t contractIndex, uint16_t inputType, DispatchKind kind, const void* context, void* statePointer, void* input,
    void* output, void* locals)
{
    (void)statePointer;
    (void)locals;

    DispatchOutcome outcome;
    const int slotOffset = engineSlotOffset(contractIndex);
    if (slotOffset < 0)
    {
        return outcome;
    }

    EngineSlot& slot = engineSlots[slotOffset];
    if (!slot.loaded)
    {
        return outcome;
    }

    if (kind == DispatchKind::Migration)
    {
        return dispatchMigration(contractIndex, slotOffset, slot, context, input);
    }

    IoSizes sizes;
    if (!resolveIoSizes(contractIndex, inputType, kind, slot, sizes))
    {
        return outcome;
    }

    EnvironmentScope environment(slot);
    if (!environment.ready)
    {
        return outcome;
    }

    const MemoryLayout fixedLayout = resolveMemoryLayout(slot);
    uint32_t arenaLimit = 0;
    if (!resolveArenaLimit(fixedLayout, arenaLimit))
    {
        logColorToScreen("ERROR", "LITEWASM dispatch arena exceeds Wasm32 memory idx=" + std::to_string(contractIndex));
        return outcome;
    }

    const bool nested = slotCallDepth[slotOffset] != 0;
    CallContext* parentContext = slotCallContexts[slotOffset];
    MemoryLayout layout = fixedLayout;
    if (nested && !nestedMemoryLayout(fixedLayout, arenaLimit, parentContext ? parentContext->arenaTop : 0, layout))
    {
        logColorToScreen("ERROR", "LITEWASM nested dispatch frame exceeds arena idx=" + std::to_string(contractIndex));
        return outcome;
    }

    DispatchFrameScope frame(slot, environment.execEnv, slotOffset, static_cast<const QPI::QpiContext*>(context), layout, arenaLimit, nested);
    // After `frame` so it destroys first, once finalizeMemory has re-pointed contractStates.
    StateWriteSeqScope stateSeq(kind != DispatchKind::UserFunction && !nested, contractIndex);
    DispatchDepthScope frameDepth(dispatchDepth);
    outcome.rootFrame = dispatchDepth == 1;
    frame.callContext().contractIndex = contractIndex;
    frame.callContext().inputType = inputType;
    frame.callContext().kind = (unsigned char)kind;
    DispatchTrace trace;
    beginDispatchTrace(slot, contractIndex, inputType, kind, context, input, sizes, frame.callContext(), trace);
    prepareMemory(slot, layout, context, input, sizes);

    unsigned char* linearMemory = (unsigned char*)wasm_runtime_addr_app_to_native(slot.instance, slot.ioBaseOffset) - slot.ioBaseOffset;
    // A nested frame would reset the journal out from under its caller, so nesting keeps the tracker.
    const bool journalUsable = slot.journalBaseOffset && !slot.journalOverflowed && !nested;
    StateJournalScope journalScope(trace.tracksWrites && journalUsable, linearMemory, slot.journalBaseOffset, slot.stateOffset, slot.journalHeader);
    StateWriteScope pageProtection(trace.tracksWrites && (!journalUsable || verifyingStateDiff()), trace.state, slot.stateSize);
    const bool succeeded = invokeDispatch(slot, environment.execEnv, kind, inputType, layout.inputOffset, layout.outputOffset, layout.localsOffset);
    const std::string frameLabel = "it=" + std::to_string(inputType) + " kind=" + std::to_string((unsigned int)kind);
    handleDispatchResult(slot, contractIndex, frameLabel, succeeded, outcome);
    pageProtection.finish(trace.entry);
    recordStateDiff(slot, contractIndex, journalScope, trace.entry);

    finalizeMemory(slot, layout, contractIndex, kind, output, sizes);
    finishDispatchTrace(slot, layout, sizes, frame.callContext(), trace);
    return outcome;
}

static void dispatchClosure(ffi_cif*, void*, void** arguments, void* userData)
{
    EntryBinding* binding = (EntryBinding*)userData;
    const void* context = *(const void**)arguments[0];
    const DispatchOutcome outcome = dispatchCall(binding->contractIndex, binding->inputType, binding->kind, context, *(void**)arguments[1], *(void**)arguments[2], *(void**)arguments[3], *(void**)arguments[4]);
    settleRootFailure(outcome, binding->kind, binding->contractIndex, binding->inputType, context);
}

static void migrationClosure(ffi_cif*, void*, void** arguments, void* userData)
{
    EntryBinding* binding = (EntryBinding*)userData;
    const void* context = *(const void**)arguments[0];
    const DispatchOutcome outcome = dispatchCall(binding->contractIndex, 0, DispatchKind::Migration, context, *(void**)arguments[1], *(void**)arguments[2], nullptr, *(void**)arguments[3]);
    settleRootFailure(outcome, DispatchKind::Migration, binding->contractIndex, 0, context);
}

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
