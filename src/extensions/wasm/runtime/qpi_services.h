#pragma once

// Node QPI adapters used by runtime-deployed contracts.
#ifdef LITE_WASM_SC

#include "extensions/wasm/shared/abi_types.h"

#ifdef _MSC_VER
#undef __transfer
#endif

void logToConsole(const CHAR16* message);

namespace Wasm::Runtime
{

static void logBytes(unsigned int contractIndex, unsigned char type, const void* message, unsigned int size)
{
    *((unsigned int*)(void*)message) = contractIndex;
    qLogger::logMessage(size, type, message);

    *((unsigned int*)(void*)message) = 0;
}

static unsigned int enumerateAssets(const void*, unsigned int kind, const void* issuance, const void* ownership, const void* possession, void* outputBuffer,
    unsigned int capacity)
{
    AssetEntry* output = (AssetEntry*)outputBuffer;
    unsigned int count = 0;

    if (kind == 1)
    {
        QPI::AssetPossessionIterator iterator(*(const QPI::Asset*)issuance, *(const QPI::AssetOwnershipSelect*)ownership, *(const QPI::AssetPossessionSelect*)possession);

        while (!iterator.reachedEnd() && count < capacity)
        {
            QPI::id owner = iterator.owner();
            QPI::id possessor = iterator.possessor();

            copyMem(output[count].owner, &owner, 32);
            copyMem(output[count].possessor, &possessor, 32);
            output[count].shares = iterator.numberOfPossessedShares();
            output[count].ownershipManagingContract = iterator.ownershipManagingContract();
            output[count].possessionManagingContract = 0;

            iterator.next();
            count++;
        }
    }
    else
    {
        QPI::AssetOwnershipIterator iterator(*(const QPI::Asset*)issuance, *(const QPI::AssetOwnershipSelect*)ownership);

        while (!iterator.reachedEnd() && count < capacity)
        {
            QPI::id owner = iterator.owner();

            copyMem(output[count].owner, &owner, 32);
            copyMem(output[count].possessor, &owner, 32);
            output[count].shares = iterator.numberOfOwnedShares();
            output[count].ownershipManagingContract = iterator.ownershipManagingContract();
            output[count].possessionManagingContract = 0;

            iterator.next();
            count++;
        }
    }

    return count;
}

static int callContractFunction(const void* callerContext, unsigned int contractIndex, unsigned short inputType, const void* input, unsigned int, void* output,
    unsigned int)
{
    if (contractIndex >= contractCount || !contractUserFunctions[contractIndex][inputType])
    {
        return (int)QPI::CallErrorContractInactive;
    }

    auto* caller = (QPI::QpiContextFunctionCall*)callerContext;
    QPI::InterContractCallError error = QPI::NoCallError;
    const QPI::QpiContextFunctionCall* calleeContext = caller->__qpiConstructContextOtherContractFunctionCall(contractIndex, error);
    if (!calleeContext)
    {
        return (int)error;
    }

    void* state = caller->__qpiAcquireStateForReading(contractIndex);
    void* locals = caller->__qpiAllocLocals(contractUserFunctionLocalsSizes[contractIndex][inputType]);

    contractUserFunctions[contractIndex][inputType](*calleeContext, state, (void*)input, output, locals);

    caller->__qpiFreeLocals();
    caller->__qpiReleaseStateForReading(contractIndex);
    caller->__qpiFreeContext();
    return (int)QPI::NoCallError;
}

static int invokeContractProcedure(const void* callerContext, unsigned int contractIndex, unsigned short inputType, const void* input, unsigned int,
    void* output, unsigned int, long long invocationReward)
{
    if (contractIndex >= contractCount || !contractUserProcedures[contractIndex][inputType])
    {
        return (int)QPI::CallErrorContractInactive;
    }

    auto* caller = (QPI::QpiContextProcedureCall*)callerContext;
    QPI::InterContractCallError error = QPI::NoCallError;
    const QPI::QpiContextProcedureCall* calleeContext = caller->__qpiConstructProcedureCallContext(contractIndex, invocationReward, error, false);
    if (!calleeContext)
    {
        return (int)error;
    }

    void* state = caller->__qpiAcquireStateForWriting(contractIndex);
    void* locals = caller->__qpiAllocLocals(contractUserProcedureLocalsSizes[contractIndex][inputType]);

    contractUserProcedures[contractIndex][inputType](*calleeContext, state, (void*)input, output, locals);

    caller->__qpiFreeLocals();
    caller->__qpiReleaseStateForWriting(contractIndex);
    caller->__qpiFreeContext();
    return (int)QPI::NoCallError;
}

static unsigned short setShareholderProposal(const void* context, unsigned int contractIndex, const void* proposal, long long invocationReward)
{
    return ((QPI::QpiContextProcedureCall*)context)->setShareholderProposal((unsigned short)contractIndex, *(const QPI::Array<QPI::uint8, 1024>*)proposal, invocationReward);
}

static unsigned char setShareholderVotes(const void* context, unsigned int contractIndex, const void* voteData, unsigned int, long long invocationReward)
{
    return (unsigned char)((QPI::QpiContextProcedureCall*)context)->setShareholderVotes((unsigned short)contractIndex, *(const QPI::ProposalMultiVoteDataV1*)voteData, invocationReward);
}

static QPI::QpiContextFunctionCall* functionContext(const void* context)
{
    return (QPI::QpiContextFunctionCall*)context;
}

static QPI::QpiContextProcedureCall* procedureContext(const void* context)
{
    return (QPI::QpiContextProcedureCall*)context;
}

static void beginFunction(unsigned int id)
{
    __beginFunctionOrProcedure(id);
}

static void endFunction(unsigned int id)
{
    __endFunctionOrProcedure(id);
}

static void markContractDirty(unsigned int contractIndex)
{
    __markContractStateDirty(contractIndex);
}

static void pauseLog()
{
    __pauseLogMessage();
}

static void resumeLog()
{
    __resumeLogMessage();
}

static void* acquireScratch(unsigned long long size, bool initializeToZero)
{
    return __acquireScratchpad(size, initializeToZero);
}

static void releaseScratch(void* pointer)
{
    __releaseScratchpad(pointer);
}

static void hashK12(const void* input, unsigned int length, void* output)
{
    KangarooTwelve(input, length, output, 32);
}

static long long transfer(const void* context, const void* destination, long long amount)
{
    return procedureContext(context)->transfer(*(const m256i*)destination, amount);
}

static long long transferTyped(const void* context, const void* destination, long long amount, unsigned char transferType)
{
    return procedureContext(context)->__transfer(*(const m256i*)destination, amount, transferType);
}

static void abortCall(const void* context, unsigned int errorCode)
{
    procedureContext(context)->__qpiAbort(errorCode);
}

static long long burn(const void* context, long long amount, unsigned int contractIndex)
{
    return procedureContext(context)->burn(amount, contractIndex);
}

// CC_WARP shifts only what the contract observes; the node still commits the real tick and epoch, so a
// warping contract cannot move consensus. Reset per dispatch, and constant-folded away off testnet.
#if defined(TESTNET)
static unsigned int cheatTickOffset = 0;
static unsigned short cheatEpochOffset = 0;
#else
static constexpr unsigned int cheatTickOffset = 0;
static constexpr unsigned short cheatEpochOffset = 0;
#endif

static unsigned short epoch(const void* context)
{
    return (unsigned short)(functionContext(context)->epoch() + cheatEpochOffset);
}

static unsigned int tick(const void* context)
{
    return functionContext(context)->tick() + cheatTickOffset;
}

static unsigned int initialTick(const void* context)
{
    return functionContext(context)->initialTick();
}

static int numberOfTickTransactions(const void* context)
{
    return functionContext(context)->numberOfTickTransactions();
}

static unsigned char getEntity(const void* context, const void* id, void* entity)
{
    return (unsigned char)functionContext(context)->getEntity(*(const m256i*)id, *(QPI::Entity*)entity);
}

static long long queryFeeReserve(const void* context, unsigned int contractIndex)
{
    return functionContext(context)->queryFeeReserve(contractIndex);
}

static void nextId(const void* context, const void* id, void* output)
{
    *(m256i*)output = functionContext(context)->nextId(*(const m256i*)id);
}

static void previousId(const void* context, const void* id, void* output)
{
    *(m256i*)output = functionContext(context)->prevId(*(const m256i*)id);
}

static unsigned char isContractId(const void* context, const void* id)
{
    return (unsigned char)functionContext(context)->isContractId(*(const m256i*)id);
}

static void arbitrator(const void* context, void* output)
{
    *(m256i*)output = functionContext(context)->arbitrator();
}

static void computor(const void* context, unsigned short index, void* output)
{
    *(m256i*)output = functionContext(context)->computor(index);
}

static unsigned char day(const void* context)
{
    return functionContext(context)->day();
}

static unsigned char year(const void* context)
{
    return functionContext(context)->year();
}

static unsigned char hour(const void* context)
{
    return functionContext(context)->hour();
}

static unsigned char minute(const void* context)
{
    return functionContext(context)->minute();
}

static unsigned char month(const void* context)
{
    return functionContext(context)->month();
}

static unsigned char second(const void* context)
{
    return functionContext(context)->second();
}

static unsigned short millisecond(const void* context)
{
    return functionContext(context)->millisecond();
}

static void now(const void* context, void* output)
{
    *(QPI::DateAndTime*)output = functionContext(context)->now();
}

static void previousSpectrumDigest(const void* context, void* output)
{
    *(m256i*)output = functionContext(context)->getPrevSpectrumDigest();
}

static void previousUniverseDigest(const void* context, void* output)
{
    *(m256i*)output = functionContext(context)->getPrevUniverseDigest();
}

static void previousComputerDigest(const void* context, void* output)
{
    *(m256i*)output = functionContext(context)->getPrevComputerDigest();
}

static unsigned char isAssetIssued(const void* context, const void* issuer, unsigned long long name)
{
    return (unsigned char)functionContext(context)->isAssetIssued(*(const m256i*)issuer, name);
}

static long long issueAsset(const void* context, unsigned long long name, const void* issuer, signed char decimals, long long shares, unsigned long long unit)
{
    return procedureContext(context)->issueAsset(name, *(const QPI::id*)issuer, decimals, shares, unit);
}

static long long numberOfShares(const void* context, const void* asset, const void* ownership, const void* possession)
{
    return functionContext(context)->numberOfShares(*(const QPI::Asset*)asset, *(const QPI::AssetOwnershipSelect*)ownership, *(const QPI::AssetPossessionSelect*)possession);
}

static long long numberOfPossessedShares(const void* context, unsigned long long name, const void* issuer, const void* owner, const void* possessor,
    unsigned short ownershipManagement, unsigned short possessionManagement)
{
    return functionContext(context)->numberOfPossessedShares(name, *(const m256i*)issuer, *(const m256i*)owner, *(const m256i*)possessor, ownershipManagement, possessionManagement);
}

static long long transferShareOwnershipAndPossession(const void* context, unsigned long long name, const void* issuer, const void* owner, const void* possessor,
    long long shares, const void* newOwner)
{
    return procedureContext(context)->transferShareOwnershipAndPossession(name, *(const m256i*)issuer, *(const m256i*)owner, *(const m256i*)possessor, shares, *(const m256i*)newOwner);
}

static long long acquireShares(const void* context, unsigned long long name, const void* issuer, const void* owner, const void* possessor, long long shares,
    unsigned short sourceOwnershipManagement, unsigned short sourcePossessionManagement, long long fee)
{
    return procedureContext(context)->acquireShares(QPI::Asset{ *(const m256i*)issuer, name }, *(const m256i*)owner, *(const m256i*)possessor, shares, sourceOwnershipManagement, sourcePossessionManagement, fee);
}

static long long releaseShares(const void* context, unsigned long long name, const void* issuer, const void* owner, const void* possessor, long long shares,
    unsigned short destinationOwnershipManagement, unsigned short destinationPossessionManagement, long long fee)
{
    return procedureContext(context)->releaseShares(QPI::Asset{ *(const m256i*)issuer, name }, *(const m256i*)owner, *(const m256i*)possessor, shares, destinationOwnershipManagement, destinationPossessionManagement, fee);
}

static unsigned char dayOfWeek(const void* context, unsigned char year, unsigned char month, unsigned char day)
{
    return functionContext(context)->dayOfWeek(year, month, day);
}

static unsigned char signatureValidity(const void* context, const void* entity, const void* digest, const void* signature)
{
    return (unsigned char)functionContext(context)->signatureValidity(*(const m256i*)entity, *(const m256i*)digest, *(const QPI::Array<QPI::sint8, 64>*)signature);
}

static long long bidInIPO(const void* context, unsigned int contractIndex, long long price, unsigned int quantity)
{
    return procedureContext(context)->bidInIPO(contractIndex, price, quantity);
}

static void ipoBidId(const void* context, unsigned int contractIndex, unsigned int bidIndex, void* output)
{
    *(m256i*)output = functionContext(context)->ipoBidId(contractIndex, bidIndex);
}

static long long ipoBidPrice(const void* context, unsigned int contractIndex, unsigned int bidIndex)
{
    return functionContext(context)->ipoBidPrice(contractIndex, bidIndex);
}

static void computeMiningFunction(const void* context, const void* seed, const void* publicKey, const void* nonce, void* output)
{
    *(m256i*)output = functionContext(context)->computeMiningFunction(*(const m256i*)seed, *(const m256i*)publicKey, *(const m256i*)nonce);
}

static void initMiningSeed(const void* context, const void* seed)
{
    functionContext(context)->initMiningSeed(*(const m256i*)seed);
}

static unsigned char getOracleQueryStatus(const void* context, long long queryId)
{
    return functionContext(context)->getOracleQueryStatus(queryId);
}

static unsigned char getOcInvocationStatus(const void* context, long long invocationId)
{
    return functionContext(context)->getOcInvocationStatus(invocationId);
}

static long long invokeOc(const void* context, unsigned int interfaceIndex, const void* request, unsigned int requestSize)
{
    static_assert(OCI::ocInterfacesCount == 1, "add Wasm OC dispatch case");

    if (!context || !request || interfaceIndex >= OCI::ocInterfacesCount || requestSize != OCI::ocInterfaces[interfaceIndex].requestSize)
    {
        return -1;
    }

    switch (interfaceIndex)
    {
    case OCI::Mock::ocInterfaceIndex:
    {
        OCI::Mock::OcRequest typedRequest;
        copyMem(&typedRequest, request, sizeof(typedRequest));
        return procedureContext(context)->__qpiInvokeOC<OCI::Mock>(typedRequest);
    }
    default:
        return -1;
    }
}

static unsigned char unsubscribeOracle(const void* context, int subscriptionId)
{
    return (unsigned char)procedureContext(context)->unsubscribeOracle(subscriptionId);
}

static unsigned char distributeDividends(const void* context, long long amountPerShare)
{
    return (unsigned char)procedureContext(context)->distributeDividends(amountPerShare);
}

// Mirrors QpiContext's protected layout so a prank can rewrite the caller a contract observes. The
// size assert turns any drift in QpiContext into a compile error rather than a silent misread.
struct CheatContextImage
{
    unsigned int currentContractIndex;
    int stackIndex;
    m256i currentContractId;
    m256i originator;
    m256i invocator;
    long long invocationReward;
    unsigned char entryPoint;
};
static_assert(sizeof(CheatContextImage) == sizeof(QPI::QpiContext), "CheatContextImage out of sync with QPI::QpiContext");

static void clearCheatWarp()
{
#if defined(TESTNET)
    cheatTickOffset = 0;
    cheatEpochOffset = 0;
#endif
}

#if defined(TESTNET)
// Sets a balance outright rather than transferring, which is the whole point of a deal.
static long long dealCheatBalance(const m256i& publicKey, long long amount)
{
    // The amount arrives as an unsigned word, so a value past the signed range lands here negative.
    // A negative balance is meaningless, and letting it through would decrease against index -1.
    if (amount < 0)
    {
        return CHEAT_ERR_UNKNOWN_OP;
    }

    const int index = spectrumIndex(publicKey);
    const long long current = index < 0 ? 0 : energy(index);

    if (current > amount)
    {
        return decreaseEnergy(index, current - amount) ? amount : CHEAT_ERR_WRONG_CONTEXT;
    }

    if (current < amount)
    {
        increaseEnergy(publicKey, amount - current);
    }

    return amount;
}

#endif

// Rewrites the guest's copy of the context. The host's own QpiContext is untouched, so the node still
// bills and attributes the real caller; only what the contract reads changes. Lives here rather than in
// the vtable because the guest address comes from the adapter, and the vtable signature must mirror the
// guest's exactly.
static long long prankCheatCaller(const void* context, void* guestContext, const m256i* caller, long long invocationReward)
{
#if defined(TESTNET)
    const CheatContextImage* hostImage = (const CheatContextImage*)context;

    if (!hostImage || hostImage->entryPoint == (unsigned char)DispatchKind::UserFunction)
    {
        return CHEAT_ERR_WRONG_CONTEXT;
    }

    if (!guestContext)
    {
        return CHEAT_ERR_WRONG_CONTEXT;
    }

    CheatContextImage* image = (CheatContextImage*)guestContext;

    // Unprank restores what the host handed the guest at dispatch, which is the host's own context.
    image->originator = caller ? *caller : hostImage->originator;
    image->invocator = caller ? *caller : hostImage->invocator;
    image->invocationReward = caller ? invocationReward : hostImage->invocationReward;
    return image->invocationReward;
#else
    (void)context;
    (void)guestContext;
    (void)caller;
    (void)invocationReward;
    return CHEAT_ERR_DISABLED;
#endif
}

// Development cheatcodes, opcode-dispatched behind one ABI row so a new one costs no import. Every
// state-touching opcode is testnet-only, and refusal is always a negative return, never a trap.
// CHEAT_OP_PRINT and the prank opcodes never reach here: the adapter owns the trace and guest memory.
static long long cheat(const void* context, unsigned int op, unsigned long long a, unsigned long long b, void* ptr, unsigned int len)
{
#if defined(TESTNET)
    const CheatContextImage* image = (const CheatContextImage*)context;

    // Every opcode below mutates something a read-only call must not touch.
    if (!image || image->entryPoint == (unsigned char)DispatchKind::UserFunction)
    {
        return CHEAT_ERR_WRONG_CONTEXT;
    }

    switch (op)
    {
    case CHEAT_OP_DEAL:
        return (ptr && len == 32) ? dealCheatBalance(*(const m256i*)ptr, (long long)a) : CHEAT_ERR_UNKNOWN_OP;

    case CHEAT_OP_WARP_TICK:
        cheatTickOffset += (unsigned int)a;
        return (long long)cheatTickOffset;

    case CHEAT_OP_WARP_EPOCH:
        cheatEpochOffset = (unsigned short)(cheatEpochOffset + a);
        return (long long)cheatEpochOffset;

    default:
        return CHEAT_ERR_UNKNOWN_OP;
    }
#else
    (void)context;
    (void)op;
    (void)a;
    (void)b;
    (void)ptr;
    (void)len;
    return CHEAT_ERR_DISABLED;
#endif
}

} // namespace Wasm::Runtime

#endif // LITE_WASM_SC
