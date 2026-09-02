#pragma once

// Contract-side declarations for the stable "lhost" import surface.
#ifdef LITE_WASM_TU_BUILD

#define LH_IMPORT(name) __attribute__((import_module("lhost"), import_name(#name)))
#define LH_EXPORT(name) __attribute__((export_name(#name)))

void setMem(void* buffer, unsigned long long size, unsigned char value)
{
    __builtin_memset(buffer, value, size);
}

void copyMem(void* destination, const void* source, unsigned long long length)
{
    __builtin_memcpy(destination, source, length);
}

bool allocatePool(unsigned long long size, void** buffer)
{
    *buffer = __builtin_malloc(size);
    return *buffer != nullptr;
}

void freePool(void* buffer)
{
    __builtin_free(buffer);
}

// These signatures are the contract side of the stable "lhost" ABI.
extern "C" {
LH_IMPORT(beginFn)        void  lh_beginFn(unsigned int id);
LH_IMPORT(endFn)          void  lh_endFn(unsigned int id);
LH_IMPORT(markDirty)      void  lh_markDirty(unsigned int contractIndex);
LH_IMPORT(pauseLog)       void  lh_pauseLog();
LH_IMPORT(resumeLog)      void  lh_resumeLog();
LH_IMPORT(acquireScratch) void* lh_acquireScratch(unsigned long long size, unsigned int initZero);
LH_IMPORT(releaseScratch) void  lh_releaseScratch(void* ptr);
LH_IMPORT(logBytes)       void  lh_logBytes(unsigned int ci, unsigned int level, const void* msg, unsigned int size);
LH_IMPORT(k12)            void  lh_k12(const void* in, unsigned int len, void* out32);
LH_IMPORT(transfer)       long long lh_transfer(const void* dest32, long long amount);
LH_IMPORT(transferTyped)  long long lh_transferTyped(const void* dest32, long long amount, unsigned int transferType);
LH_IMPORT(abort)          void  lh_abort(unsigned int errorCode);
LH_IMPORT(burn)           long long lh_burn(long long amount, unsigned int contractIndexBurnedFor);
LH_IMPORT(epoch)          unsigned int lh_epoch();
LH_IMPORT(tick)           unsigned int lh_tick();
LH_IMPORT(initialTick)    unsigned int lh_initialTick();
LH_IMPORT(numberOfTickTransactions) int lh_numberOfTickTransactions();
LH_IMPORT(getEntity)      unsigned int lh_getEntity(const void* id32, void* entityOut);
LH_IMPORT(queryFeeReserve) long long lh_queryFeeReserve(unsigned int contractIndex);
LH_IMPORT(nextId)         void  lh_nextId(const void* id32, void* out32);
LH_IMPORT(prevId)         void  lh_prevId(const void* id32, void* out32);
LH_IMPORT(isContractId)   unsigned int lh_isContractId(const void* id32);
LH_IMPORT(arbitrator)     void  lh_arbitrator(void* out32);
LH_IMPORT(computor)       void  lh_computor(unsigned int index, void* out32);
LH_IMPORT(day)            unsigned int lh_day();
LH_IMPORT(year)           unsigned int lh_year();
LH_IMPORT(hour)           unsigned int lh_hour();
LH_IMPORT(minute)         unsigned int lh_minute();
LH_IMPORT(month)          unsigned int lh_month();
LH_IMPORT(second)         unsigned int lh_second();
LH_IMPORT(millisecond)    unsigned int lh_millisecond();
LH_IMPORT(now)            void  lh_now(void* dateAndTimeOut);
LH_IMPORT(prevSpectrumDigest) void lh_prevSpectrumDigest(void* out32);
LH_IMPORT(prevUniverseDigest) void lh_prevUniverseDigest(void* out32);
LH_IMPORT(prevComputerDigest) void lh_prevComputerDigest(void* out32);
LH_IMPORT(isAssetIssued)  unsigned int lh_isAssetIssued(const void* issuer32, unsigned long long assetName);
LH_IMPORT(issueAsset) long long lh_issueAsset(unsigned long long name, const void* issuer32, unsigned int decimals, long long shares, unsigned long long unit);
LH_IMPORT(numberOfShares) long long lh_numberOfShares(const void* asset, const void* ownSel, const void* posSel);
LH_IMPORT(numberOfPossessedShares) long long lh_numberOfPossessedShares(unsigned long long name, const void* issuer32, const void* owner32, const void* possessor32, unsigned int om, unsigned int pm);
LH_IMPORT(assetEnumerate) unsigned int lh_assetEnumerate(unsigned int kind, const void* issuance, const void* ownership, const void* possession, void* out, unsigned int capacity);
LH_IMPORT(transferShareOwnershipAndPossession) long long lh_transferShares(unsigned long long name, const void* issuer32, const void* owner32, const void* possessor32, long long shares, const void* newOwner32);
LH_IMPORT(acquireShares) long long lh_acquireShares(unsigned long long name, const void* issuer32, const void* owner32, const void* possessor32, long long shares, unsigned int srcOwnMgmt, unsigned int srcPosMgmt, long long offeredFee);
LH_IMPORT(releaseShares) long long lh_releaseShares(unsigned long long name, const void* issuer32, const void* owner32, const void* possessor32, long long shares, unsigned int dstOwnMgmt, unsigned int dstPosMgmt, long long offeredFee);
LH_IMPORT(dayOfWeek) unsigned int lh_dayOfWeek(unsigned int year, unsigned int month, unsigned int day);
LH_IMPORT(signatureValidity) unsigned int lh_signatureValidity(const void* entity32, const void* digest32, const void* signature64);
LH_IMPORT(bidInIPO) long long lh_bidInIPO(unsigned int ipoContractIndex, long long price, unsigned int quantity);
LH_IMPORT(ipoBidId) void lh_ipoBidId(unsigned int ipoContractIndex, unsigned int ipoBidIndex, void* out32);
LH_IMPORT(ipoBidPrice) long long lh_ipoBidPrice(unsigned int ipoContractIndex, unsigned int ipoBidIndex);
LH_IMPORT(computeMiningFunction) void lh_computeMiningFunction(const void* miningSeed32, const void* publicKey32, const void* nonce32, void* out32);
LH_IMPORT(initMiningSeed) void lh_initMiningSeed(const void* miningSeed32);
LH_IMPORT(getOracleQueryStatus) unsigned int lh_getOracleQueryStatus(long long queryId);
LH_IMPORT(getOcInvocationStatus) unsigned int lh_getOcInvocationStatus(long long invocationId);
LH_IMPORT(invokeOc) long long lh_invokeOc(unsigned int interfaceIndex, const void* request, unsigned int requestSize);
LH_IMPORT(unsubscribeOracle) unsigned int lh_unsubscribeOracle(int oracleSubscriptionId);
LH_IMPORT(queryOracle) long long lh_queryOracle(unsigned int interfaceIndex, const void* query, unsigned int querySize, unsigned int replySize, unsigned int notificationProcId, unsigned int timeoutMillisec, long long fee);
LH_IMPORT(subscribeOracle) int lh_subscribeOracle(unsigned int interfaceIndex, const void* query, unsigned int querySize, unsigned int replySize, unsigned int timestampOffset, unsigned int notificationProcId, unsigned int periodMillisec, unsigned int notifyPrev, long long fee);
LH_IMPORT(getOracleQuery) unsigned int lh_getOracleQuery(long long queryId, void* out, unsigned int size);
LH_IMPORT(getOracleReply) unsigned int lh_getOracleReply(long long queryId, void* out, unsigned int size);
LH_IMPORT(distributeDividends) unsigned int lh_distributeDividends(long long amountPerShare);
LH_IMPORT(liteCallFunction) int lh_liteCallFunction(unsigned int calleeIdx, unsigned int inputType, const void* in, unsigned int inSize, void* out, unsigned int outSize);
LH_IMPORT(liteInvokeProcedure) int lh_liteInvokeProcedure(unsigned int calleeIdx, unsigned int inputType, const void* in, unsigned int inSize, void* out, unsigned int outSize, long long invocationReward);
LH_IMPORT(liteSetShareholderProposal) unsigned int lh_liteSetShareholderProposal(unsigned int calleeIdx, const void* proposal1024, long long invocationReward);
LH_IMPORT(liteSetShareholderVotes) unsigned int lh_liteSetShareholderVotes(unsigned int calleeIdx, const void* voteData, unsigned int voteSize, long long invocationReward);
LH_IMPORT(cheat) long long lh_cheat(unsigned int op, unsigned long long a, unsigned long long b, void* ptr, unsigned int len);
} // extern "C"

namespace Wasm::Sdk
{

int callFunction(const void*, unsigned int calleeIndex, unsigned short inputType, const void* input, unsigned int inputSize, void* output,
    unsigned int outputSize)
{
    return lh_liteCallFunction(calleeIndex, inputType, input, inputSize, output, outputSize);
}

int invokeProcedure(const void*, unsigned int calleeIndex, unsigned short inputType, const void* input, unsigned int inputSize, void* output,
    unsigned int outputSize, long long invocationReward)
{
    return lh_liteInvokeProcedure(calleeIndex, inputType, input, inputSize, output, outputSize, invocationReward);
}

} // namespace Wasm::Sdk

static void __markContractStateDirty(unsigned int contractIndex)
{
    lh_markDirty(contractIndex);
}

static void __beginFunctionOrProcedure(const unsigned int id)
{
    lh_beginFn(id);
}

static void __endFunctionOrProcedure(const unsigned int id)
{
    lh_endFn(id);
}

static void __pauseLogMessage()
{
    lh_pauseLog();
}

static void __resumeLogMessage()
{
    lh_resumeLog();
}

static void* __acquireScratchpad(unsigned long long size, bool initializeToZero)
{
    return lh_acquireScratch(size, initializeToZero ? 1u : 0u);
}

static void __releaseScratchpad(void* pointer)
{
    lh_releaseScratch(pointer);
}

template <typename T>
static void __logContractDebugMessage(unsigned int contractIndex, T& message)
{
    static_assert(__builtin_offsetof(T, _terminator) >= 8, "Invalid contract debug message structure");

    lh_logBytes(contractIndex, 7, &message, (unsigned int)__builtin_offsetof(T, _terminator));
}

template <typename T>
static void __logContractErrorMessage(unsigned int contractIndex, T& message)
{
    static_assert(__builtin_offsetof(T, _terminator) >= 8, "Invalid contract error message structure");

    lh_logBytes(contractIndex, 4, &message, (unsigned int)__builtin_offsetof(T, _terminator));
}

template <typename T>
static void __logContractInfoMessage(unsigned int contractIndex, T& message)
{
    static_assert(__builtin_offsetof(T, _terminator) >= 8, "Invalid contract info message structure");

    lh_logBytes(contractIndex, 6, &message, (unsigned int)__builtin_offsetof(T, _terminator));
}

template <typename T>
static void __logContractWarningMessage(unsigned int contractIndex, T& message)
{
    static_assert(__builtin_offsetof(T, _terminator) >= 8, "Invalid contract warning message structure");

    lh_logBytes(contractIndex, 5, &message, (unsigned int)__builtin_offsetof(T, _terminator));
}


#endif // LITE_WASM_TU_BUILD
