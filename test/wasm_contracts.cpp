// Exercise the Wasm registration, dispatch, and state ABI through WAMR.
#ifdef LITE_WASM_SC

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "gtest/gtest.h"
#ifdef _WIN32
#define WASM_RUNTIME_API_EXTERN
#endif
#include "wasm_export.h"
constexpr unsigned short WASM_RESERVED_SLOT_BASE = 28;
constexpr unsigned short WASM_RESERVED_SLOT_COUNT = 4;
#include "extensions/wasm/runtime/arena_scope.h"
#include "extensions/wasm/runtime/contract_slots.h"
#include "extensions/wasm/runtime/deployment_protocol.h"
#include "extensions/wasm/runtime/state_write_journal.h"
#include "wasm_contract_fixture.h"

namespace
{
enum
{
    KIND_FUNCTION = 0,
    KIND_PROCEDURE = 1,
};

struct WasmFixture
{
    wasm_module_t mod = nullptr;
    wasm_module_inst_t inst = nullptr;
    wasm_exec_env_t env = nullptr;
    unsigned char buf[8192];

    bool load()
    {
        static char heap[8 * 1024 * 1024];
        RuntimeInitArgs ia;
        memset(&ia, 0, sizeof ia);
        ia.mem_alloc_type = Alloc_With_Pool;
        ia.mem_alloc_option.pool.heap_buf = heap;
        ia.mem_alloc_option.pool.heap_size = sizeof heap;
        if (!wasm_runtime_full_init(&ia))
        {
            return false;
        }
        if (g_wasmFixtureLen > sizeof buf)
        {
            return false;
        }

        // WAMR mutates the module buffer while loading it.
        memcpy(buf, g_wasmFixture, g_wasmFixtureLen);
        char err[192];
        mod = wasm_runtime_load(buf, g_wasmFixtureLen, err, sizeof err);
        if (!mod)
        {
            return false;
        }
        inst = wasm_runtime_instantiate(mod, 64 * 1024, 1024 * 1024, err, sizeof err);
        if (!inst)
        {
            return false;
        }
        env = wasm_runtime_create_exec_env(inst, 64 * 1024);
        return env != nullptr;
    }

    ~WasmFixture()
    {
        if (env)
        {
            wasm_runtime_destroy_exec_env(env);
        }
        if (inst)
        {
            wasm_runtime_deinstantiate(inst);
        }
        if (mod)
        {
            wasm_runtime_unload(mod);
        }
        wasm_runtime_destroy();
    }

    uint32_t call(const char* fn, uint32_t* argv, uint32_t n)
    {
        wasm_function_inst_t f = wasm_runtime_lookup_function(inst, fn);
        EXPECT_NE(f, nullptr) << fn;
        EXPECT_TRUE(wasm_runtime_call_wasm(env, f, n, argv)) << fn << ": " << wasm_runtime_get_exception(inst);
        return argv[0];
    }

    void* nat(uint32_t off)
    {
        return wasm_runtime_addr_app_to_native(inst, off);
    }
};
} // namespace

TEST(WasmContracts, DispatchDepthRestores)
{
    uint32_t depth = 0;

    {
        Wasm::Runtime::DispatchDepthScope outer(depth);
        EXPECT_EQ(depth, 1u);

        {
            Wasm::Runtime::DispatchDepthScope nested(depth);
            EXPECT_EQ(depth, 2u);
        }
        EXPECT_EQ(depth, 1u);
    }
    EXPECT_EQ(depth, 0u);
}

TEST(WasmContracts, DeploymentProtocolLayout)
{
    using namespace Wasm::Runtime::DeploymentProtocol;

    EXPECT_EQ(DeploymentAddress.u64._0, 99999u);
    EXPECT_EQ(DeploymentAddress.u64._1, 0u);
    EXPECT_EQ(DeploymentAddress.u64._2, 0u);
    EXPECT_EQ(DeploymentAddress.u64._3, 0u);

    EXPECT_EQ(offsetof(UploadBeginMessage, sessionId), 0u);
    EXPECT_EQ(offsetof(UploadBeginMessage, totalSize), 8u);
    EXPECT_EQ(offsetof(UploadBeginMessage, chunkCount), 12u);
    EXPECT_EQ(offsetof(UploadBeginMessage, finalHash), 16u);

    EXPECT_EQ(offsetof(UploadChunkHeader, sessionId), 0u);
    EXPECT_EQ(offsetof(UploadChunkHeader, sequence), 8u);
    EXPECT_EQ(offsetof(UploadChunkHeader, dataLength), 12u);

    EXPECT_EQ(offsetof(DeployHeader, sessionId), 0u);
    EXPECT_EQ(offsetof(DeployHeader, targetSlot), 8u);
    EXPECT_EQ(offsetof(DeployHeader, finalHash), 12u);
    EXPECT_EQ(offsetof(DeployHeader, abiVersion), 44u);
    EXPECT_EQ(offsetof(DeployHeader, stateLayoutVersion), 48u);
    EXPECT_EQ(offsetof(DeployMessage, name), 52u);
}

TEST(WasmContracts, UploadBeginPreservesTheActiveSession)
{
    using namespace Wasm::Runtime;

    moduleUpload = ModuleUpload{};
    std::memset(moduleUploadBuffer, 0, sizeof(moduleUploadBuffer));
    std::memset(receivedChunkBits, 0, sizeof(receivedChunkBits));
    unsigned char firstHash[32];
    unsigned char otherHash[32];
    std::memset(firstHash, 0x11, sizeof(firstHash));
    std::memset(otherHash, 0x22, sizeof(otherHash));

    ASSERT_TRUE(tryBeginModuleUpload(11, WASM_UPLOAD_CHUNK_SIZE * 2u, 2, firstHash));
    moduleUploadBuffer[0] = 0xab;
    moduleUploadBuffer[WASM_UPLOAD_CHUNK_SIZE] = 0xcd;
    receivedChunkBits[0] = 1;
    moduleUpload.receivedCount = 1;

    EXPECT_TRUE(tryBeginModuleUpload(11, WASM_UPLOAD_CHUNK_SIZE, 1, otherHash));
    EXPECT_EQ(moduleUpload.sessionId, 11u);
    EXPECT_EQ(moduleUpload.totalSize, WASM_UPLOAD_CHUNK_SIZE * 2u);
    EXPECT_EQ(moduleUpload.chunkCount, 2u);
    EXPECT_EQ(moduleUpload.receivedCount, 1u);
    EXPECT_EQ(std::memcmp(moduleUpload.finalHash, firstHash, sizeof(firstHash)), 0);
    EXPECT_EQ(receivedChunkBits[0], 1u);
    EXPECT_EQ(moduleUploadBuffer[0], 0xab);
    EXPECT_EQ(moduleUploadBuffer[WASM_UPLOAD_CHUNK_SIZE], 0xcd);

    EXPECT_FALSE(tryBeginModuleUpload(22, WASM_UPLOAD_CHUNK_SIZE, 1, otherHash));
    EXPECT_EQ(moduleUpload.sessionId, 11u);
    EXPECT_EQ(moduleUpload.totalSize, WASM_UPLOAD_CHUNK_SIZE * 2u);
    EXPECT_EQ(moduleUpload.chunkCount, 2u);
    EXPECT_EQ(moduleUpload.receivedCount, 1u);
    EXPECT_EQ(std::memcmp(moduleUpload.finalHash, firstHash, sizeof(firstHash)), 0);
    EXPECT_EQ(receivedChunkBits[0], 1u);
    EXPECT_EQ(moduleUploadBuffer[0], 0xab);
    EXPECT_EQ(moduleUploadBuffer[WASM_UPLOAD_CHUNK_SIZE], 0xcd);
}

TEST(WasmContracts, StaleUploadSessionGivesWayToANewOne)
{
    using namespace Wasm::Runtime;

    moduleUpload = ModuleUpload{};
    std::memset(receivedChunkBits, 0, sizeof(receivedChunkBits));
    unsigned char firstHash[32];
    unsigned char otherHash[32];
    std::memset(firstHash, 0x11, sizeof(firstHash));
    std::memset(otherHash, 0x22, sizeof(otherHash));
    unsigned char chunk[WASM_UPLOAD_CHUNK_SIZE] = {};

    ASSERT_TRUE(tryBeginModuleUpload(11, WASM_UPLOAD_CHUNK_SIZE * 2u, 2, firstHash, 100));
    ASSERT_TRUE(tryReceiveModuleChunk(11, 0, chunk, WASM_UPLOAD_CHUNK_SIZE, 105));
    EXPECT_EQ(moduleUpload.lastProgressTick, 105u);

    // Still within the idle window: another session is refused and nothing moves.
    EXPECT_FALSE(moduleUploadStale(105 + WASM_UPLOAD_STALE_TICKS));
    EXPECT_FALSE(tryBeginModuleUpload(22, WASM_UPLOAD_CHUNK_SIZE, 1, otherHash, 105 + WASM_UPLOAD_STALE_TICKS));
    EXPECT_EQ(moduleUpload.sessionId, 11u);
    EXPECT_EQ(moduleUpload.receivedCount, 1u);

    // One tick past the window the old session is abandoned and the new one takes the slot.
    EXPECT_TRUE(moduleUploadStale(106 + WASM_UPLOAD_STALE_TICKS));
    EXPECT_TRUE(tryBeginModuleUpload(22, WASM_UPLOAD_CHUNK_SIZE, 1, otherHash, 106 + WASM_UPLOAD_STALE_TICKS));
    EXPECT_EQ(moduleUpload.sessionId, 22u);
    EXPECT_EQ(moduleUpload.receivedCount, 0u);
    EXPECT_EQ(moduleUpload.lastProgressTick, 106u + WASM_UPLOAD_STALE_TICKS);
    EXPECT_EQ(std::memcmp(moduleUpload.finalHash, otherHash, sizeof(otherHash)), 0);
    EXPECT_EQ(receivedChunkBits[0], 0u);
}

TEST(WasmContracts, UploadBeginRejectsInvalidShapesWithoutMutation)
{
    using namespace Wasm::Runtime;

    struct InvalidShape
    {
        const char* name;
        unsigned int totalSize;
        unsigned int chunkCount;
    };
    const InvalidShape cases[] = {
        { "empty", 0, 0 },
        { "zero chunks", 1, 0 },
        { "too many chunks", 1, 2 },
        { "extra full chunk", WASM_UPLOAD_CHUNK_SIZE, 2 },
        { "missing final chunk", WASM_UPLOAD_CHUNK_SIZE + 1, 1 },
        {
            "above module limit",
            WASM_MAX_MODULE_SIZE + 1u,
            WASM_MAX_UPLOAD_CHUNKS,
        },
    };
    unsigned char finalHash[32];
    std::memset(finalHash, 0x11, sizeof(finalHash));

    for (const InvalidShape& testCase : cases)
    {
        SCOPED_TRACE(testCase.name);
        moduleUpload = ModuleUpload{};
        std::memset(moduleUploadBuffer, 0x5a, 64);
        std::memset(receivedChunkBits, 0xa5, sizeof(receivedChunkBits));

        EXPECT_FALSE(tryBeginModuleUpload(17, testCase.totalSize, testCase.chunkCount, finalHash));
        EXPECT_FALSE(moduleUpload.active);
        EXPECT_EQ(moduleUpload.sessionId, 0u);
        EXPECT_EQ(moduleUpload.totalSize, 0u);
        EXPECT_EQ(moduleUpload.chunkCount, 0u);
        EXPECT_EQ(moduleUpload.receivedCount, 0u);
        EXPECT_EQ(moduleUploadBuffer[0], 0x5a);
        EXPECT_EQ(receivedChunkBits[0], 0xa5);
    }
}

TEST(WasmContracts, UploadChunksRejectInvalidSequenceOrLengthWithoutMutation)
{
    using namespace Wasm::Runtime;

    struct RejectedChunk
    {
        const char* name;
        unsigned int totalSize;
        bool receiveFirst;
        unsigned long long sessionId;
        unsigned int sequence;
        unsigned int dataLength;
    };
    const unsigned long long activeSessionId = 31;
    const unsigned int twoFullChunksSizeBytes = WASM_UPLOAD_CHUNK_SIZE * 2u;
    const unsigned int finalChunkSizeBytes = 92;
    const unsigned int partialFinalSizeBytes = WASM_UPLOAD_CHUNK_SIZE + finalChunkSizeBytes;
    const RejectedChunk cases[] = {
        {
            "short first",
            twoFullChunksSizeBytes,
            false,
            activeSessionId,
            0,
            WASM_UPLOAD_CHUNK_SIZE - 1u,
        },
        {
            "oversized first",
            twoFullChunksSizeBytes,
            false,
            activeSessionId,
            0,
            WASM_UPLOAD_CHUNK_SIZE + 1u,
        },
        // only the sequence bound refuses these two: the length each one carries is the length its offset works out to.
        {
            "empty chunk after the last",
            twoFullChunksSizeBytes,
            false,
            activeSessionId,
            2,
            0,
        },
        {
            "full chunk past the end",
            twoFullChunksSizeBytes,
            false,
            activeSessionId,
            3,
            WASM_UPLOAD_CHUNK_SIZE,
        },
        {
            "duplicate",
            twoFullChunksSizeBytes,
            true,
            activeSessionId,
            0,
            WASM_UPLOAD_CHUNK_SIZE,
        },
        {
            "short final",
            partialFinalSizeBytes,
            true,
            activeSessionId,
            1,
            finalChunkSizeBytes - 1u,
        },
        {
            "oversized final",
            partialFinalSizeBytes,
            true,
            activeSessionId,
            1,
            finalChunkSizeBytes + 1u,
        },
        {
            "stale session",
            twoFullChunksSizeBytes,
            false,
            activeSessionId + 1,
            0,
            WASM_UPLOAD_CHUNK_SIZE,
        },
    };
    unsigned char finalHash[32];
    std::memset(finalHash, 0x22, sizeof(finalHash));
    std::vector<unsigned char> firstChunk(WASM_UPLOAD_CHUNK_SIZE, 0x33);
    std::vector<unsigned char> rejectedChunk(WASM_UPLOAD_CHUNK_SIZE + 1, 0x44);

    for (const RejectedChunk& testCase : cases)
    {
        SCOPED_TRACE(testCase.name);
        moduleUpload = ModuleUpload{};
        std::memset(moduleUploadBuffer, 0x5a, testCase.totalSize);
        std::memset(receivedChunkBits, 0, sizeof(receivedChunkBits));
        const unsigned int chunkCount = expectedModuleUploadChunkCount(testCase.totalSize);
        ASSERT_TRUE(tryBeginModuleUpload(activeSessionId, testCase.totalSize, chunkCount, finalHash));
        if (testCase.receiveFirst)
        {
            ASSERT_TRUE(tryReceiveModuleChunk(activeSessionId, 0, firstChunk.data(), WASM_UPLOAD_CHUNK_SIZE));
        }

        const ModuleUpload uploadBefore = moduleUpload;
        const std::vector<unsigned char> bufferBefore(moduleUploadBuffer, moduleUploadBuffer + testCase.totalSize);
        const std::vector<unsigned char> chunkBitsBefore(receivedChunkBits, receivedChunkBits + sizeof(receivedChunkBits));

        EXPECT_FALSE(tryReceiveModuleChunk(testCase.sessionId, testCase.sequence, rejectedChunk.data(), testCase.dataLength));
        EXPECT_EQ(moduleUpload.active, uploadBefore.active);
        EXPECT_EQ(moduleUpload.sessionId, uploadBefore.sessionId);
        EXPECT_EQ(moduleUpload.totalSize, uploadBefore.totalSize);
        EXPECT_EQ(moduleUpload.chunkCount, uploadBefore.chunkCount);
        EXPECT_EQ(moduleUpload.receivedCount, uploadBefore.receivedCount);
        EXPECT_EQ(std::memcmp(moduleUpload.finalHash, uploadBefore.finalHash, sizeof(moduleUpload.finalHash)), 0);
        EXPECT_EQ(std::memcmp(moduleUploadBuffer, bufferBefore.data(), bufferBefore.size()), 0);
        EXPECT_EQ(std::memcmp(receivedChunkBits, chunkBitsBefore.data(), chunkBitsBefore.size()), 0);
    }
}

TEST(WasmContracts, UploadChunksAcceptExactSequentialPayloads)
{
    using namespace Wasm::Runtime;

    const unsigned int totalSize = WASM_UPLOAD_CHUNK_SIZE * 2u + 1u;
    unsigned char finalHash[32];
    std::memset(finalHash, 0x66, sizeof(finalHash));
    std::vector<unsigned char> fullChunk(WASM_UPLOAD_CHUNK_SIZE, 0x77);
    const unsigned char lastByte = 0x88;
    moduleUpload = ModuleUpload{};
    std::memset(moduleUploadBuffer, 0, totalSize);
    std::memset(receivedChunkBits, 0, sizeof(receivedChunkBits));

    ASSERT_TRUE(tryBeginModuleUpload(41, totalSize, expectedModuleUploadChunkCount(totalSize), finalHash));
    EXPECT_TRUE(tryReceiveModuleChunk(41, 0, fullChunk.data(), WASM_UPLOAD_CHUNK_SIZE));
    EXPECT_TRUE(tryReceiveModuleChunk(41, 1, fullChunk.data(), WASM_UPLOAD_CHUNK_SIZE));
    EXPECT_TRUE(tryReceiveModuleChunk(41, 2, &lastByte, 1));

    EXPECT_EQ(moduleUpload.receivedCount, expectedModuleUploadChunkCount(totalSize));
    EXPECT_EQ(moduleUploadBuffer[0], 0x77);
    EXPECT_EQ(moduleUploadBuffer[WASM_UPLOAD_CHUNK_SIZE], 0x77);
    EXPECT_EQ(moduleUploadBuffer[totalSize - 1], 0x88);
}

// every chunk of a module is sent for one tick, and nothing promises they are processed in the order they were sent.
TEST(WasmContracts, UploadChunksAcceptAnyArrivalOrder)
{
    using namespace Wasm::Runtime;

    const unsigned int totalSize = WASM_UPLOAD_CHUNK_SIZE * 2u + 1u;
    unsigned char finalHash[32];
    std::memset(finalHash, 0x66, sizeof(finalHash));
    std::vector<unsigned char> firstChunk(WASM_UPLOAD_CHUNK_SIZE, 0x71);
    std::vector<unsigned char> secondChunk(WASM_UPLOAD_CHUNK_SIZE, 0x72);
    const unsigned char lastByte = 0x73;
    moduleUpload = ModuleUpload{};
    std::memset(moduleUploadBuffer, 0, totalSize);
    std::memset(receivedChunkBits, 0, sizeof(receivedChunkBits));

    ASSERT_TRUE(tryBeginModuleUpload(42, totalSize, expectedModuleUploadChunkCount(totalSize), finalHash));
    EXPECT_TRUE(tryReceiveModuleChunk(42, 2, &lastByte, 1));
    EXPECT_TRUE(tryReceiveModuleChunk(42, 0, firstChunk.data(), WASM_UPLOAD_CHUNK_SIZE));
    EXPECT_TRUE(tryReceiveModuleChunk(42, 1, secondChunk.data(), WASM_UPLOAD_CHUNK_SIZE));

    EXPECT_EQ(moduleUpload.receivedCount, expectedModuleUploadChunkCount(totalSize));
    EXPECT_EQ(moduleUploadBuffer[0], 0x71);
    EXPECT_EQ(moduleUploadBuffer[WASM_UPLOAD_CHUNK_SIZE - 1u], 0x71);
    EXPECT_EQ(moduleUploadBuffer[WASM_UPLOAD_CHUNK_SIZE], 0x72);
    EXPECT_EQ(moduleUploadBuffer[totalSize - 2u], 0x72);
    EXPECT_EQ(moduleUploadBuffer[totalSize - 1u], 0x73);

    // a repeat is refused whenever it arrives, and leaves the bytes it would have replaced.
    EXPECT_FALSE(tryReceiveModuleChunk(42, 0, secondChunk.data(), WASM_UPLOAD_CHUNK_SIZE));
    EXPECT_EQ(moduleUpload.receivedCount, expectedModuleUploadChunkCount(totalSize));
    EXPECT_EQ(moduleUploadBuffer[0], 0x71);
}

TEST(WasmContracts, DeployOutcomeKeepsASessionsFirstVerdict)
{
    using namespace Wasm::Runtime;

    DeployOutcome stored;
    storeDeployOutcome(stored, 7, WASM_RESERVED_SLOT_BASE, 100, DEPLOY_CODE_INCOMPLETE, "upload incomplete (3/4 chunks)");
    EXPECT_TRUE(stored.set);
    EXPECT_FALSE(stored.ok);
    EXPECT_STREQ(stored.code, "incomplete");
    EXPECT_STREQ(stored.message, "upload incomplete (3/4 chunks)");

    // the missing chunk arrived and the resent DEPLOY went through.
    storeDeployOutcome(stored, 7, WASM_RESERVED_SLOT_BASE, 101, DEPLOY_CODE_OK, "slot armed");
    EXPECT_TRUE(stored.ok);
    EXPECT_EQ(stored.tick, 101u);

    // a resend that lands after the success finds the upload closed, and must not undo it.
    storeDeployOutcome(stored, 7, WASM_RESERVED_SLOT_BASE, 102, DEPLOY_CODE_INCOMPLETE, "upload incomplete (0/4 chunks)");
    EXPECT_TRUE(stored.ok);
    EXPECT_EQ(stored.tick, 101u);

    storeDeployOutcome(stored, 8, WASM_RESERVED_SLOT_BASE + 1u, 103, DEPLOY_CODE_ABI_MISMATCH, "unsupported Wasm ABI version 6; expected 7");
    EXPECT_EQ(stored.sessionId, 8u);
    EXPECT_EQ(stored.slot, WASM_RESERVED_SLOT_BASE + 1u);
    EXPECT_STREQ(stored.code, "abi-mismatch");

    // after a definitive refusal each resend fails earlier, and none of them may replace the reason.
    storeDeployOutcome(stored, 8, WASM_RESERVED_SLOT_BASE + 1u, 104, DEPLOY_CODE_SESSION_MISMATCH, "session 8 is not the upload session on this node");
    EXPECT_STREQ(stored.code, "abi-mismatch");
    EXPECT_EQ(stored.tick, 103u);

    const std::string oversized(1000, 'x');
    storeDeployOutcome(stored, 9, WASM_RESERVED_SLOT_BASE, 105, DEPLOY_CODE_LOAD_FAILED, oversized);
    EXPECT_EQ(strlen(stored.message), sizeof(stored.message) - 1);
}

TEST(WasmContracts, ReservedSlotOffsetRejectsNonDynamicContractSlots)
{
    using namespace Wasm::Runtime;

    struct SlotCase
    {
        unsigned int contractIndex;
        int expectedOffset;
    };
    const SlotCase cases[] = {
        { WASM_RESERVED_SLOT_BASE - 1u, -1 },
        { WASM_RESERVED_SLOT_BASE, 0 },
        {
            WASM_RESERVED_SLOT_BASE + WASM_RESERVED_SLOT_COUNT - 1u,
            (int)WASM_RESERVED_SLOT_COUNT - 1,
        },
        { WASM_RESERVED_SLOT_BASE + WASM_RESERVED_SLOT_COUNT, -1 },
    };

    for (const SlotCase& testCase : cases)
    {
        EXPECT_EQ(reservedSlotOffset(testCase.contractIndex), testCase.expectedOffset);
    }
}

TEST(WasmContracts, NestedCallUsesIsolatedFrame)
{
    using namespace Wasm::Runtime;

    const MemoryLayout fixed = fixedMemoryLayout(4096);
    const uint32_t parentArenaTop = fixed.arenaOffset + 21;
    const uint32_t expectedInput = (parentArenaTop + 7u) & ~7u;
    const uint32_t arenaLimit = expectedInput + WASM_DISPATCH_FRAME_CAPACITY + 64;
    MemoryLayout nested = {};

    ASSERT_TRUE(nestedMemoryLayout(fixed, arenaLimit, parentArenaTop, nested));
    EXPECT_EQ(nested.inputOffset, expectedInput);
    EXPECT_EQ(nested.outputOffset, expectedInput + WASM_INPUT_CAPACITY);
    EXPECT_EQ(nested.localsOffset, expectedInput + WASM_INPUT_CAPACITY + WASM_OUTPUT_CAPACITY);
    EXPECT_EQ(nested.arenaOffset, expectedInput + WASM_DISPATCH_FRAME_CAPACITY);
    EXPECT_GE(nested.inputOffset, fixed.arenaOffset);

    std::vector<unsigned char> memory(arenaLimit + 1, 0);
    memset(memory.data() + fixed.inputOffset, 0x11, WASM_INPUT_CAPACITY);
    memset(memory.data() + fixed.outputOffset, 0x22, WASM_OUTPUT_CAPACITY);
    memset(memory.data() + fixed.localsOffset, 0x33, WASM_LOCALS_CAPACITY);
    memset(memory.data() + nested.inputOffset, 0x44, WASM_DISPATCH_FRAME_CAPACITY);

    EXPECT_EQ(memory[fixed.inputOffset], 0x11);
    EXPECT_EQ(memory[fixed.outputOffset], 0x22);
    EXPECT_EQ(memory[fixed.localsOffset], 0x33);

    MemoryLayout exhausted = {};
    EXPECT_FALSE(nestedMemoryLayout(fixed, nested.arenaOffset - 1, parentArenaTop, exhausted));
}

TEST(WasmContracts, EntryLocalsAreFullyZeroed)
{
    std::vector<unsigned char> locals(Wasm::Runtime::WASM_LOCALS_CAPACITY, 0xa5);

    Wasm::Runtime::zeroEntryLocals(locals.data());

    for (unsigned char byte : locals)
    {
        ASSERT_EQ(byte, 0);
    }
}

TEST(WasmContracts, RegistrationDispatchAndStateRoundTrip)
{
    WasmFixture w;
    ASSERT_TRUE(w.load());

    wasm_function_inst_t contractIndex = wasm_runtime_lookup_function(w.inst, "contract_index");
    ASSERT_NE(contractIndex, nullptr);
    EXPECT_EQ(wasm_func_get_param_count(contractIndex, w.inst), 0u);
    EXPECT_EQ(wasm_func_get_result_count(contractIndex, w.inst), 1u);
    wasm_valkind_t resultType = WASM_I64;
    wasm_func_get_result_types(contractIndex, w.inst, &resultType);
    EXPECT_EQ(resultType, WASM_I32);

    uint32_t a[5] = { 0 };
    EXPECT_EQ(w.call("contract_index", a, 0), 29u);
    EXPECT_EQ(w.call("reg_count", a, 0), 2u);

    a[0] = 0;
    uint32_t io = w.call("io_base", a, 0);
    struct EntryInfo
    {
        uint32_t inputType;
        uint32_t kind;
        uint32_t inSize;
        uint32_t outSize;
    };
    a[0] = 0;
    a[1] = io;
    w.call("reg_info", a, 2);
    EntryInfo* e0 = (EntryInfo*)w.nat(io);
    EXPECT_EQ(e0->inputType, 1u);
    EXPECT_EQ(e0->kind, (uint32_t)KIND_FUNCTION);
    EXPECT_EQ(e0->outSize, 8u);
    a[0] = 1;
    a[1] = io;
    w.call("reg_info", a, 2);
    EntryInfo* e1 = (EntryInfo*)w.nat(io);
    EXPECT_EQ(e1->inputType, 2u);
    EXPECT_EQ(e1->kind, (uint32_t)KIND_PROCEDURE);
    EXPECT_EQ(e1->inSize, 8u);

    const uint32_t IN = io;
    const uint32_t OUT = io + 64;
    const uint32_t LOCALS = io + 128;

    for (uint64_t i = 1; i <= 3; i++)
    {
        *(uint64_t*)w.nat(IN) = 1;
        a[0] = KIND_PROCEDURE;
        a[1] = 2;
        a[2] = IN;
        a[3] = OUT;
        a[4] = LOCALS;
        w.call("dispatch", a, 5);
        EXPECT_EQ(*(uint64_t*)w.nat(OUT), i) << "INC returns running count";
    }

    a[0] = KIND_FUNCTION;
    a[1] = 1;
    a[2] = IN;
    a[3] = OUT;
    a[4] = LOCALS;
    w.call("dispatch", a, 5);
    EXPECT_EQ(*(uint64_t*)w.nat(OUT), 3u) << "get_count after 3 increments";
}

TEST(WasmContracts, SystemProceduresMaskAndDispatch)
{
    WasmFixture w;
    ASSERT_TRUE(w.load());
    enum
    {
        KIND_SYSPROC = 2,
        SP_INITIALIZE = 0,
        SP_POST_INCOMING_TRANSFER = 9,
    };

    uint32_t a[5] = { 0 };
    EXPECT_EQ(w.call("reg_sysproc_mask", a, 0), (1u << SP_INITIALIZE) | (1u << SP_POST_INCOMING_TRANSFER));
    a[0] = SP_POST_INCOMING_TRANSFER;
    EXPECT_EQ(w.call("sysproc_in_size", a, 1), 8u);
    a[0] = SP_INITIALIZE;
    EXPECT_EQ(w.call("sysproc_in_size", a, 1), 0u);

    a[0] = 0;
    uint32_t io = w.call("io_base", a, 0);
    a[0] = 0;
    uint32_t st = w.call("state_addr", a, 0);
    const uint32_t IN = io;
    const uint32_t OUT = io + 64;
    const uint32_t LOCALS = io + 128;

    a[0] = KIND_SYSPROC;
    a[1] = SP_INITIALIZE;
    a[2] = IN;
    a[3] = OUT;
    a[4] = LOCALS;
    w.call("dispatch", a, 5);
    EXPECT_EQ(((uint64_t*)w.nat(st))[0], 4242u) << "INITIALIZE sysproc ran via kind=2 dispatch";

    *(uint64_t*)w.nat(IN) = 777;
    a[0] = KIND_SYSPROC;
    a[1] = SP_POST_INCOMING_TRANSFER;
    a[2] = IN;
    a[3] = OUT;
    a[4] = LOCALS;
    w.call("dispatch", a, 5);
    EXPECT_EQ(((uint64_t*)w.nat(st))[1], 777u) << "input sysproc read its marshalled input";
}

// Run Qinit-built fixtures directly in WAMR for cross-host state parity.
namespace
{
void hs_void_i(wasm_exec_env_t, uint32_t)
{
}

void hs_assert(wasm_exec_env_t, uint32_t, uint32_t, uint32_t)
{
}

uint32_t hs_fd_write(wasm_exec_env_t, uint32_t, uint32_t, uint32_t, uint32_t)
{
    return 0;
}

uint32_t hs_fd_close(wasm_exec_env_t, uint32_t)
{
    return 0;
}

uint32_t hs_fd_seek(wasm_exec_env_t, uint32_t, uint64_t, uint32_t, uint32_t)
{
    return 0;
}

uint32_t hs_acquireScratch(wasm_exec_env_t executionEnvironment, uint64_t size, uint32_t initializeToZero)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(executionEnvironment);
    void* native = nullptr;
    const uint32_t offset = (uint32_t)wasm_runtime_module_malloc(inst, (uint32_t)size, &native);
    if (offset && initializeToZero && native)
    {
        memset(native, 0, (size_t)size);
    }
    return offset;
}

void hs_releaseScratch(wasm_exec_env_t executionEnvironment, uint32_t offset)
{
    if (offset)
    {
        wasm_runtime_module_free(wasm_runtime_get_module_inst(executionEnvironment), offset);
    }
}

// ---- qinit parity shims ----
// Registered so the parity sweep can execute contracts that call a host function instead of trapping
// on an unresolved import. Every value below mirrors what a bare QubicSimulator reports — tick 0,
// epoch 0, clock pinned to 2024-01-01T00:00:00Z — because the sweep compares this runtime against
// that simulator. A shim returning anything else would manufacture a divergence rather than reveal
// one, which is the opposite of what a third oracle is for.
//
// Deliberately NOT shimmed: transfers, the asset ledger, logging, inter-contract calls, IPO, mining
// and the oracle. Those need real host state, and a stub that invents an answer turns "this contract
// is unreachable" into "this contract silently agreed", which is strictly worse than a trap.
// dayOfWeek is left out too: it needs dayIndex, and no corpus contract calls it.

extern "C" void qinitShimK12(const void* input, unsigned int length, void* output32);

void* appToNative(wasm_exec_env_t executionEnvironment, uint32_t offset)
{
    return wasm_runtime_addr_app_to_native(wasm_runtime_get_module_inst(executionEnvironment), offset);
}

// lh_k12 carries no output length over the ABI; it is always 32 bytes, matching qpi_services.h's
// hashK12 and the qinit engine's k12Bytes. Both were checked to produce identical digests before
// this shim was trusted.
void hs_k12(wasm_exec_env_t executionEnvironment, uint32_t inputOffset, uint32_t length, uint32_t outputOffset)
{
    qinitShimK12(appToNative(executionEnvironment, inputOffset), length, appToNative(executionEnvironment, outputOffset));
}

uint32_t hs_zero(wasm_exec_env_t)
{
    return 0;
}

// month() and day() are both 1 on 2024-01-01.
uint32_t hs_one(wasm_exec_env_t)
{
    return 1;
}

// The engine reports the year in Qubic's two-digit form: (getUTCFullYear() - 2000) & 0xff.
uint32_t hs_year(wasm_exec_env_t)
{
    return 24;
}

void hs_void(wasm_exec_env_t)
{
}

void hs_now(wasm_exec_env_t executionEnvironment, uint32_t outputOffset)
{
    // packDateAndTime(Date.UTC(2024, 0, 1)): year+2000 at bit 46, month at 42, day at 37, rest zero.
    const uint64_t packed = ((uint64_t)2024 << 46) | ((uint64_t)1 << 42) | ((uint64_t)1 << 37);
    memcpy(appToNative(executionEnvironment, outputOffset), &packed, sizeof packed);
}

int hexNibble(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}
} // namespace

TEST(WasmContracts, CrossHostStateEquivalence)
{
    const char* path = getenv("QINIT_WASM");
    if (!path)
    {
        GTEST_SKIP() << "set QINIT_WASM to a qinit-built pure-state wasm (e.g. DigestProbe)";
    }
    const int ops = getenv("QINIT_OPS") ? atoi(getenv("QINIT_OPS")) : 1;

    FILE* fp = fopen(path, "rb");
    ASSERT_NE(fp, nullptr) << "open " << path;
    fseek(fp, 0, SEEK_END);
    long flen = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<unsigned char> buf(flen);
    ASSERT_EQ(fread(buf.data(), 1, flen, fp), (size_t)flen);
    fclose(fp);

    static char heap[32 * 1024 * 1024];
    RuntimeInitArgs ia;
    memset(&ia, 0, sizeof ia);
    ia.mem_alloc_type = Alloc_With_Pool;
    ia.mem_alloc_option.pool.heap_buf = heap;
    ia.mem_alloc_option.pool.heap_size = sizeof heap;
    ASSERT_TRUE(wasm_runtime_full_init(&ia));

    static NativeSymbol lhostNs[] = {
        { "beginFn", (void*)hs_void_i, "(i)", nullptr },
        { "endFn", (void*)hs_void_i, "(i)", nullptr },
        { "markDirty", (void*)hs_void_i, "(i)", nullptr },
        { "acquireScratch", (void*)hs_acquireScratch, "(Ii)i", nullptr },
        { "releaseScratch", (void*)hs_releaseScratch, "(i)", nullptr },
        { "k12", (void*)hs_k12, "(iii)", nullptr },
        { "tick", (void*)hs_zero, "()i", nullptr },
        { "epoch", (void*)hs_zero, "()i", nullptr },
        { "initialTick", (void*)hs_zero, "()i", nullptr },
        { "numberOfTickTransactions", (void*)hs_zero, "()i", nullptr },
        { "year", (void*)hs_year, "()i", nullptr },
        { "month", (void*)hs_one, "()i", nullptr },
        { "day", (void*)hs_one, "()i", nullptr },
        { "hour", (void*)hs_zero, "()i", nullptr },
        { "minute", (void*)hs_zero, "()i", nullptr },
        { "second", (void*)hs_zero, "()i", nullptr },
        { "millisecond", (void*)hs_zero, "()i", nullptr },
        { "now", (void*)hs_now, "(i)", nullptr },
        { "pauseLog", (void*)hs_void, "()", nullptr },
        { "resumeLog", (void*)hs_void, "()", nullptr },
    };
    static NativeSymbol envNs[] = {
        { "_ZL21addDebugMessageAssertPKcS0_j", (void*)hs_assert, "(iii)", nullptr },
    };
    static NativeSymbol wasiNs[] = {
        { "fd_write", (void*)hs_fd_write, "(iiii)i", nullptr },
        { "fd_close", (void*)hs_fd_close, "(i)i", nullptr },
        { "fd_seek", (void*)hs_fd_seek, "(iIii)i", nullptr },
    };
    ASSERT_TRUE(wasm_runtime_register_natives("lhost", lhostNs, sizeof lhostNs / sizeof lhostNs[0]));
    ASSERT_TRUE(wasm_runtime_register_natives("env", envNs, 1));
    ASSERT_TRUE(wasm_runtime_register_natives("wasi_snapshot_preview1", wasiNs, 3));

    char err[256];
    wasm_module_t mod = wasm_runtime_load(buf.data(), (uint32_t)flen, err, sizeof err);
    ASSERT_NE(mod, nullptr) << err;
    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, 256 * 1024, 4 * 1024 * 1024, err, sizeof err);
    ASSERT_NE(inst, nullptr) << err;
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 256 * 1024);
    ASSERT_NE(env, nullptr);

    const char* expectedSlotValue = getenv("QINIT_EXPECTED_SLOT");
    ASSERT_NE(expectedSlotValue, nullptr) << "set QINIT_EXPECTED_SLOT for raw WAMR parity";
    const uint32_t expectedSlot = (uint32_t)strtoul(expectedSlotValue, nullptr, 10);
    wasm_function_inst_t contractIndex = wasm_runtime_lookup_function(inst, "contract_index");
    ASSERT_NE(contractIndex, nullptr) << "missing required contract_index export";
    ASSERT_EQ(wasm_func_get_param_count(contractIndex, inst), 0u);
    ASSERT_EQ(wasm_func_get_result_count(contractIndex, inst), 1u);
    wasm_valkind_t contractIndexResultType = WASM_I64;
    wasm_func_get_result_types(contractIndex, inst, &contractIndexResultType);
    ASSERT_EQ(contractIndexResultType, WASM_I32);
    uint32_t contractIndexArguments[1] = { 0 };
    ASSERT_TRUE(wasm_runtime_call_wasm(env, contractIndex, 0, contractIndexArguments)) << wasm_runtime_get_exception(inst);
    ASSERT_EQ(contractIndexArguments[0], expectedSlot) << "artifact slot mismatch: compiled " << contractIndexArguments[0] << ", target " << expectedSlot;

    bool trapped = false;
    auto call = [&](const char* fn, uint32_t* arguments, uint32_t argumentCount, bool expectSuccess = true) -> uint32_t
    {
        wasm_function_inst_t f = wasm_runtime_lookup_function(inst, fn);
        EXPECT_NE(f, nullptr) << fn;
        const bool ok = f && wasm_runtime_call_wasm(env, f, argumentCount, arguments);
        if (expectSuccess)
        {
            EXPECT_TRUE(ok) << fn << ": " << wasm_runtime_get_exception(inst);
        }
        trapped = !ok;
        return arguments[0];
    };

    uint32_t a[5] = { 0 };
    uint32_t io = call("io_base", a, 0);
    a[0] = 0;
    uint32_t st = call("state_addr", a, 0);
    a[0] = 0;
    uint32_t ss = call("state_size", a, 0);
    a[0] = 0;
    uint32_t carve = call("io_size", a, 0);
    a[0] = 0;
    uint32_t ctx = call("ctx_addr", a, 0);

    // Populate the per-call context header the host normally fills. Without this it stays zeroed, and
    // qpi.invocator() / originator() / invocationReward() — which are struct reads, not lhost calls,
    // so they never trap — silently answer with zeros. That is a false agreement the shim-trap
    // classifier cannot see. Layout mirrors packages/engine/src/contract/abi.ts QpiContext, and the
    // values mirror writeCtx in packages/engine/src/contract/runtime.ts.
    {
        unsigned char* context = (unsigned char*)wasm_runtime_addr_app_to_native(inst, ctx);
        memset(context, 0, 256);
        const uint32_t slot = expectedSlot;          // @0   currentContractIndex
        const int32_t stackIndex = -1;               // @4   host writes -1
        const uint64_t contractId = expectedSlot;    // @8   id(slot, 0, 0, 0) low lane
        memcpy(context + 0, &slot, sizeof slot);
        memcpy(context + 4, &stackIndex, sizeof stackIndex);
        memcpy(context + 8, &contractId, sizeof contractId);
        // originator @40 and invocator @72 stay zero: the sweep drives the simulator with no context
        // either, and the engine leaves them zeroed when none is supplied.
    }

    // The journal sits past the io carve. An artifact built before it carries none, and then the run
    // reports state only — an absent CROSSHOST_DIFF line means no journal, not an empty diff.
    unsigned int journalBase = 0;
    Wasm::Runtime::JournalHeader journalHeader = {};
    const bool haveJournal = Wasm::Runtime::attachJournal(inst, env, io, carve, ss, journalBase, journalHeader);
    unsigned char* linearMemory = haveJournal ? (unsigned char*)wasm_runtime_addr_app_to_native(inst, journalBase) - journalBase : nullptr;

    const uint32_t IN = io;
    const uint32_t OUT = io + 64;
    const uint32_t LOCALS = io + 128;
    enum
    {
        KIND_PROCEDURE = 1,
        KIND_SYSPROC = 2,
        SP_INITIALIZE = 0,
    };

    // Resolve the procedure output width from the registration table.
    struct EntryInfo
    {
        uint32_t inputType;
        uint32_t kind;
        uint32_t inSize;
        uint32_t outSize;
    };
    uint32_t outputSize = 0;
    a[0] = 0;
    const uint32_t entryCount = call("reg_count", a, 0);
    for (uint32_t i = 0; i < entryCount; ++i)
    {
        a[0] = i;
        a[1] = io;
        call("reg_info", a, 2);
        const EntryInfo* entry = (const EntryInfo*)wasm_runtime_addr_app_to_native(inst, io);
        if (entry && entry->kind == KIND_PROCEDURE && entry->inputType == 1)
        {
            outputSize = entry->outSize;
            break;
        }
    }

    a[0] = KIND_SYSPROC;
    a[1] = SP_INITIALIZE;
    a[2] = IN;
    a[3] = OUT;
    a[4] = LOCALS;
    call("dispatch", a, 5);

    auto stateHex = [&]()
    {
        const unsigned char* state = (const unsigned char*)wasm_runtime_addr_app_to_native(inst, st);
        std::string encoded;
        encoded.reserve(ss * 2);
        char byteHex[3];
        for (uint32_t i = 0; i < ss; i++)
        {
            snprintf(byteHex, sizeof(byteHex), "%02x", state[i]);
            encoded += byteHex;
        }
        return encoded;
    };

    const char* script = getenv("QINIT_SCRIPT");
    const bool captureCheckpoints = getenv("QINIT_CAPTURE_CHECKPOINTS") != nullptr;
    if (script && *script)
    {
        std::string scriptText(script);
        size_t position = 0;
        uint32_t operationIndex = 0;
        while (position < scriptText.size())
        {
            const size_t separator = scriptText.find(';', position);
            const std::string operation = scriptText.substr(position, separator == std::string::npos ? std::string::npos : separator - position);
            position = separator == std::string::npos ? scriptText.size() : separator + 1;
            if (operation.empty())
            {
                continue;
            }

            const size_t colon = operation.find(':');
            const int inputType = atoi(operation.substr(0, colon).c_str());
            const std::string inputHex = colon == std::string::npos ? std::string() : operation.substr(colon + 1);
            unsigned char* nativeInput = (unsigned char*)wasm_runtime_addr_app_to_native(inst, IN);
            memset(nativeInput, 0, 64);
            for (size_t i = 0; i + 1 < inputHex.size() && i / 2 < 64; i += 2)
            {
                const int highNibble = hexNibble(inputHex[i]);
                const int lowNibble = hexNibble(inputHex[i + 1]);
                if (highNibble >= 0 && lowNibble >= 0)
                {
                    nativeInput[i / 2] = (unsigned char)((highNibble << 4) | lowNibble);
                }
            }
            a[0] = KIND_PROCEDURE;
            a[1] = (uint32_t)inputType;
            a[2] = IN;
            a[3] = OUT;
            a[4] = LOCALS;
            // The same scope production dispatch uses, so a bug in it fails here too.
            Wasm::Runtime::StateJournalScope journalScope(haveJournal, linearMemory, journalBase, st, journalHeader);
            call("dispatch", a, 5, false);
            if (trapped)
            {
                printf("CROSSHOST_OP=%u:trap\n", operationIndex);
                if (haveJournal)
                {
                    // A trap leaves the journal holding a partial call; report it rather than a diff.
                    printf("CROSSHOST_DIFF=%u:trap\n", operationIndex);
                }
                if (captureCheckpoints)
                {
                    printf("CROSSHOST_CHECKPOINT=%u:%s\n", operationIndex, stateHex().c_str());
                }
                operationIndex++;
                break;
            }
            const unsigned char* nativeOutput = (const unsigned char*)wasm_runtime_addr_app_to_native(inst, OUT);
            std::string outHex;
            outHex.reserve(outputSize * 2);
            char byteHex[3];
            for (uint32_t i = 0; i < outputSize; ++i)
            {
                snprintf(byteHex, sizeof(byteHex), "%02x", nativeOutput[i]);
                outHex += byteHex;
            }
            printf("CROSSHOST_OP=%u:ok:%s\n", operationIndex, outHex.c_str());
            if (haveJournal)
            {
                std::vector<Wasm::Runtime::StateRegionTrace> journalDiff;
                if (!journalScope.finish(journalDiff))
                {
                    printf("CROSSHOST_DIFF=%u:overflow\n", operationIndex);
                }
                else
                {
                    std::string encodedDiff;
                    for (const Wasm::Runtime::StateRegionTrace& region : journalDiff)
                    {
                        if (!encodedDiff.empty())
                        {
                            encodedDiff += ";";
                        }
                        encodedDiff += std::to_string(region.offset) + "," + region.before + "," + region.after;
                    }
                    printf("CROSSHOST_DIFF=%u:%s\n", operationIndex, encodedDiff.c_str());
                }
            }
            if (captureCheckpoints)
            {
                printf("CROSSHOST_CHECKPOINT=%u:%s\n", operationIndex, stateHex().c_str());
            }
            operationIndex++;
        }
    }
    else
    {
        for (int i = 0; i < ops; i++)
        {
            a[0] = KIND_PROCEDURE;
            a[1] = 1;
            a[2] = IN;
            a[3] = OUT;
            a[4] = LOCALS;
            call("dispatch", a, 5);
        }
    }

    printf("CROSSHOST_STATE=%s\n", stateHex().c_str());

    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    wasm_runtime_destroy();
}

#include "wasm_trap_fixture.h"
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define close _close
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#define pipe(fds) _pipe((fds), 8192, _O_BINARY)
#define read _read
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <string>

TEST(WasmContracts, TrapAutoDumpHasMappableOffset)
{
    static char heap[8 * 1024 * 1024];
    RuntimeInitArgs ia;
    memset(&ia, 0, sizeof ia);
    ia.mem_alloc_type = Alloc_With_Pool;
    ia.mem_alloc_option.pool.heap_buf = heap;
    ia.mem_alloc_option.pool.heap_size = sizeof heap;
    ASSERT_TRUE(wasm_runtime_full_init(&ia));
    unsigned char buf[8192];
    ASSERT_LE(g_wasmTrapFixtureLen, sizeof buf);
    memcpy(buf, g_wasmTrapFixture, g_wasmTrapFixtureLen);
    char err[192];
    wasm_module_t mod = wasm_runtime_load(buf, g_wasmTrapFixtureLen, err, sizeof err);
    ASSERT_NE(mod, nullptr) << err;
    wasm_module_inst_t inst = wasm_runtime_instantiate(mod, 64 * 1024, 1024 * 1024, err, sizeof err);
    ASSERT_NE(inst, nullptr) << err;
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 64 * 1024);
    ASSERT_NE(env, nullptr);
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, "dispatch");
    ASSERT_NE(f, nullptr);

    // Capture the WAMR backtrace written to stdout.
    int saved = dup(fileno(stdout));
    int pfd[2];
    ASSERT_EQ(pipe(pfd), 0);
    fflush(stdout);
    dup2(pfd[1], fileno(stdout));
    close(pfd[1]);

    uint32_t a[5] = { 0, 0, 0, 0, 0 };
    bool ok = wasm_runtime_call_wasm(env, f, 5, a);

    fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);
    char cap[8192];
    int n = (int)read(pfd[0], cap, sizeof(cap) - 1);
    close(pfd[0]);
    cap[n > 0 ? n : 0] = '\0';
    printf("--- captured WAMR auto-dump ---\n%s\n-------------------------------\n", cap);

    EXPECT_FALSE(ok) << "dispatch must trap (divide by zero)";
    std::string out(cap);
    EXPECT_NE(out.find("#0"), std::string::npos) << "WAMR backtrace frame markers present in the auto-dump";
    EXPECT_NE(out.find("0x"), std::string::npos) << "the backtrace carries a (DWARF-mappable) byte offset";

    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    wasm_runtime_destroy();
}
#endif // LITE_WASM_SC
